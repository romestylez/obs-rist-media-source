// SPDX-License-Identifier: GPL-2.0-or-later

#include "rist-media-source.h"

#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>

#include <librist/librist.h>
#include <librist/librist_srp.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET rist_socket_t;
typedef HANDLE rist_thread_t;
typedef CRITICAL_SECTION rist_mutex_t;
#define RIST_INVALID_SOCKET INVALID_SOCKET
#define rist_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int rist_socket_t;
typedef pthread_t rist_thread_t;
typedef pthread_mutex_t rist_mutex_t;
#define RIST_INVALID_SOCKET (-1)
#define rist_close_socket close
#endif

#define SETTING_URL "rist_url"
#define SETTING_AUTH_ENABLED "auth_enabled"
#define SETTING_USERNAME "username"
#define SETTING_PASSWORD "password"
#define SETTING_PROFILE "profile"
#define SETTING_RECOVERY_MS "recovery_ms"
#define SETTING_STALE_MS "stale_ms"
#define SETTING_RECONNECT_MS "reconnect_ms"
#define SETTING_HW_DECODE "hw_decode"
#define SETTING_AUDIO_TRACKS_INITIALIZED "audio_tracks_initialized"
#define SETTING_ENCRYPTION_ENABLED "encryption_enabled"
#define SETTING_SECRET "secret"
#define SETTING_KEY_SIZE "key_size"

#define PROBE_OWNER "owner"

#define DEFAULT_RECOVERY_MS 1800
#define DEFAULT_STALE_MS 1000
#define DEFAULT_RECONNECT_MS 1000
#define MAX_RECOVERY_MS 30000
#define MAX_TIMEOUT_MS 60000
#define RECEIVER_POLL_MS 50
#define NSEC_PER_MSEC UINT64_C(1000000)

struct rist_media_source {
	obs_source_t *source;
	obs_source_t *media_source;
	obs_source_t *frame_probe;
	rist_mutex_t child_mutex;
	rist_thread_t receiver_thread;
	bool thread_started;
	atomic_bool stopping;
	atomic_bool received_video;
	atomic_uint_fast64_t last_video_at_ns;
	atomic_uint_fast64_t stale_ns;

	char *url;
	char *username;
	char *password;
	char *secret;
	enum rist_profile profile;
	uint32_t recovery_ms;
	uint32_t reconnect_ms;
	int key_size;
	bool hw_decode;
	bool auth_enabled;
	bool encryption_enabled;
	uint16_t udp_port;
};

struct frame_probe {
	struct rist_media_source *owner;
};

static void mutex_init(rist_mutex_t *mutex)
{
#ifdef _WIN32
	InitializeCriticalSection(mutex);
#else
	pthread_mutex_init(mutex, NULL);
#endif
}

static void mutex_destroy(rist_mutex_t *mutex)
{
#ifdef _WIN32
	DeleteCriticalSection(mutex);
#else
	pthread_mutex_destroy(mutex);
#endif
}

static void mutex_lock(rist_mutex_t *mutex)
{
#ifdef _WIN32
	EnterCriticalSection(mutex);
#else
	pthread_mutex_lock(mutex);
#endif
}

static void mutex_unlock(rist_mutex_t *mutex)
{
#ifdef _WIN32
	LeaveCriticalSection(mutex);
#else
	pthread_mutex_unlock(mutex);
#endif
}

static char *copy_string(const char *value)
{
	return value && *value ? bstrdup(value) : NULL;
}

static void copy_setting(char *destination, size_t size, const char *value)
{
	if (!value || size == 0)
		return;
	strncpy(destination, value, size - 1);
	destination[size - 1] = '\0';
}

static uint32_t clamp_u32(int64_t value, uint32_t minimum, uint32_t maximum)
{
	if (value < (int64_t)minimum)
		return minimum;
	if (value > (int64_t)maximum)
		return maximum;
	return (uint32_t)value;
}

static void free_settings(struct rist_media_source *source)
{
	bfree(source->url);
	bfree(source->username);
	bfree(source->password);
	bfree(source->secret);
	source->url = NULL;
	source->username = NULL;
	source->password = NULL;
	source->secret = NULL;
}

static uint16_t choose_udp_port(void)
{
	rist_socket_t socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (socket_fd == RIST_INVALID_SOCKET)
		return 0;

	struct sockaddr_in address = {0};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;

	uint16_t port = 0;
	if (bind(socket_fd, (const struct sockaddr *)&address, sizeof(address)) == 0) {
#ifdef _WIN32
		int length = sizeof(address);
#else
		socklen_t length = sizeof(address);
#endif
		if (getsockname(socket_fd, (struct sockaddr *)&address, &length) == 0)
			port = ntohs(address.sin_port);
	}

	rist_close_socket(socket_fd);
	return port;
}

static const char *probe_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "RIST Media Source frame probe";
}

static void *probe_create(obs_data_t *settings, obs_source_t *context)
{
	UNUSED_PARAMETER(context);
	struct frame_probe *probe = bzalloc(sizeof(*probe));
	probe->owner = (struct rist_media_source *)(uintptr_t)obs_data_get_int(settings, PROBE_OWNER);
	return probe;
}

static void probe_destroy(void *data)
{
	bfree(data);
}

static struct obs_source_frame *probe_video(void *data, struct obs_source_frame *frame)
{
	struct frame_probe *probe = data;
	if (frame && probe->owner) {
		atomic_store_explicit(&probe->owner->last_video_at_ns, os_gettime_ns(), memory_order_release);
		atomic_store_explicit(&probe->owner->received_video, true, memory_order_release);
	}
	return frame;
}

static struct obs_audio_data *probe_audio(void *data, struct obs_audio_data *audio)
{
	struct frame_probe *probe = data;
	if (!audio || !probe || !probe->owner)
		return audio;

	struct obs_audio_info audio_info;
	if (!obs_get_audio_info(&audio_info))
		return audio;

	struct obs_source_audio output = {0};
	for (size_t plane = 0; plane < MAX_AV_PLANES; ++plane)
		output.data[plane] = audio->data[plane];
	output.frames = audio->frames;
	output.speakers = audio_info.speakers;
	output.format = AUDIO_FORMAT_FLOAT_PLANAR;
	output.samples_per_sec = audio_info.samples_per_sec;
	output.timestamp = audio->timestamp;
	obs_source_output_audio(probe->owner->source, &output);
	return audio;
}

static struct obs_source_info frame_probe_info = {
	.id = "rist_media_source_frame_probe",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC | OBS_SOURCE_CAP_DISABLED,
	.get_name = probe_name,
	.create = probe_create,
	.destroy = probe_destroy,
	.filter_video = probe_video,
	.filter_audio = probe_audio,
};

static obs_source_t *create_media_child(struct rist_media_source *source)
{
	struct dstr input = {0};
	dstr_printf(&input, "udp://127.0.0.1:%u?fifo_size=1000000&overrun_nonfatal=1", source->udp_port);

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_string(settings, "input", input.array);
	obs_data_set_string(settings, "input_format", "mpegts");
	obs_data_set_bool(settings, "hw_decode", source->hw_decode);
	obs_data_set_bool(settings, "clear_on_media_end", true);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	obs_data_set_bool(settings, "log_changes", false);
	obs_data_set_int(settings, "buffering_mb", 0);
	obs_data_set_int(settings, "reconnect_delay_sec", 1);

	obs_source_t *child = obs_source_create_private("ffmpeg_source", NULL, settings);
	obs_data_release(settings);
	dstr_free(&input);
	return child;
}

static obs_source_t *create_frame_probe(struct rist_media_source *source)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, PROBE_OWNER, (int64_t)(uintptr_t)source);
	obs_source_t *probe = obs_source_create_private(frame_probe_info.id, NULL, settings);
	obs_data_release(settings);
	return probe;
}

static int rist_log_callback(void *arg, enum rist_log_level level, const char *message)
{
	UNUSED_PARAMETER(arg);
	if (level <= RIST_LOG_ERROR)
		blog(LOG_ERROR, "[RIST Media Source/libRIST] %s", message);
	else if (level <= RIST_LOG_WARN)
		blog(LOG_WARNING, "[RIST Media Source/libRIST] %s", message);
	return 0;
}

static int run_receiver(struct rist_media_source *source)
{
	struct rist_logging_settings *logging = NULL;
	struct rist_ctx *context = NULL;
	struct rist_peer_config *peer_config = NULL;
	struct rist_peer *peer = NULL;
	rist_socket_t udp_socket = RIST_INVALID_SOCKET;
	int result = rist_logging_set(&logging, RIST_LOG_WARN, rist_log_callback, source, NULL, NULL);
	if (result < 0)
		return result;

	result = rist_receiver_create(&context, source->profile, logging);
	if (result < 0)
		goto cleanup;

	result = rist_parse_address2(source->url, &peer_config);
	if (result != 0 || !peer_config) {
		result = result != 0 ? result : -1;
		goto cleanup;
	}

	peer_config->profile = source->profile;
	peer_config->profile_set = 1;
	peer_config->recovery_length_min = source->recovery_ms;
	peer_config->recovery_length_max = source->recovery_ms;
	if (source->profile != RIST_PROFILE_SIMPLE) {
		copy_setting(peer_config->srp_username, sizeof(peer_config->srp_username), source->username);
		copy_setting(peer_config->srp_password, sizeof(peer_config->srp_password), source->password);
	} else {
		peer_config->srp_username[0] = '\0';
		peer_config->srp_password[0] = '\0';
	}
	if (source->profile != RIST_PROFILE_SIMPLE && source->secret &&
	    (source->key_size == 128 || source->key_size == 256)) {
		copy_setting(peer_config->secret, sizeof(peer_config->secret), source->secret);
		peer_config->key_size = source->key_size;
	} else {
		peer_config->secret[0] = '\0';
		peer_config->key_size = 0;
	}

	result = rist_peer_create(context, &peer, peer_config);
	if (result < 0)
		goto cleanup;

	if (source->profile != RIST_PROFILE_SIMPLE && peer_config->srp_username[0] &&
	    peer_config->srp_password[0]) {
		result = rist_enable_eap_srp_2(peer, peer_config->srp_username, peer_config->srp_password, NULL, NULL);
		if (result != 0)
			goto cleanup;
	}

	result = rist_start(context);
	if (result < 0)
		goto cleanup;

	udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp_socket == RIST_INVALID_SOCKET) {
		result = -1;
		goto cleanup;
	}

	struct sockaddr_in destination = {0};
	destination.sin_family = AF_INET;
	destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	destination.sin_port = htons(source->udp_port);

	while (!atomic_load_explicit(&source->stopping, memory_order_acquire)) {
		struct rist_data_block *block = NULL;
		result = rist_receiver_data_read2(context, &block, RECEIVER_POLL_MS);
		if (result > 0 && block && block->payload && block->payload_len > 0) {
#ifdef _WIN32
			sendto(udp_socket, (const char *)block->payload, (int)block->payload_len, 0,
			       (const struct sockaddr *)&destination, sizeof(destination));
#else
			sendto(udp_socket, block->payload, block->payload_len, 0,
			       (const struct sockaddr *)&destination, sizeof(destination));
#endif
		}
		if (block)
			rist_receiver_data_block_free2(&block);
		if (result < 0)
			break;
	}

	if (atomic_load_explicit(&source->stopping, memory_order_acquire))
		result = 0;

cleanup:
	if (udp_socket != RIST_INVALID_SOCKET)
		rist_close_socket(udp_socket);
	if (peer_config)
		rist_peer_config_free2(&peer_config);
	if (context)
		rist_destroy(context);
	if (logging)
		rist_logging_settings_free2(&logging);
	return result;
}

static bool wait_for_reconnect(struct rist_media_source *source)
{
	uint32_t remaining = source->reconnect_ms;
	while (remaining > 0 && !atomic_load_explicit(&source->stopping, memory_order_acquire)) {
		uint32_t step = remaining > RECEIVER_POLL_MS ? RECEIVER_POLL_MS : remaining;
		os_sleep_ms(step);
		remaining -= step;
	}
	return !atomic_load_explicit(&source->stopping, memory_order_acquire);
}

#ifdef _WIN32
static DWORD WINAPI receiver_thread(LPVOID data)
#else
static void *receiver_thread(void *data)
#endif
{
	struct rist_media_source *source = data;
	while (!atomic_load_explicit(&source->stopping, memory_order_acquire)) {
		const int result = run_receiver(source);
		if (atomic_load_explicit(&source->stopping, memory_order_acquire))
			break;
		blog(LOG_WARNING, "[RIST Media Source] receiver stopped (%d); retrying in %u ms", result,
		     source->reconnect_ms);
		if (!wait_for_reconnect(source))
			break;
	}
#ifdef _WIN32
	return 0;
#else
	return NULL;
#endif
}

static void detach_child(struct rist_media_source *source)
{
	mutex_lock(&source->child_mutex);
	obs_source_t *child = source->media_source;
	obs_source_t *probe = source->frame_probe;
	source->media_source = NULL;
	source->frame_probe = NULL;
	mutex_unlock(&source->child_mutex);

	if (child && probe)
		obs_source_filter_remove(child, probe);
	if (child)
		obs_source_remove_active_child(source->source, child);
	if (probe)
		obs_source_release(probe);
	if (child)
		obs_source_release(child);
}

static void stop_source(struct rist_media_source *source)
{
	atomic_store_explicit(&source->stopping, true, memory_order_release);
	if (source->thread_started) {
#ifdef _WIN32
		WaitForSingleObject(source->receiver_thread, INFINITE);
		CloseHandle(source->receiver_thread);
#else
		pthread_join(source->receiver_thread, NULL);
#endif
		source->thread_started = false;
	}
	detach_child(source);
}

static bool start_source(struct rist_media_source *source)
{
	if (!source->url)
		return false;
	if (source->auth_enabled && (!source->username || !source->password)) {
		blog(LOG_ERROR, "[RIST Media Source] authentication requires both username and password");
		return false;
	}
	if (source->encryption_enabled && !source->secret) {
		blog(LOG_ERROR, "[RIST Media Source] encryption requires a secret");
		return false;
	}

	source->udp_port = choose_udp_port();
	if (!source->udp_port) {
		blog(LOG_ERROR, "[RIST Media Source] could not choose a loopback UDP port");
		return false;
	}

	obs_source_t *child = create_media_child(source);
	obs_source_t *probe = child ? create_frame_probe(source) : NULL;
	if (!child || !probe) {
		blog(LOG_ERROR, "[RIST Media Source] OBS FFmpeg media source is unavailable");
		if (probe)
			obs_source_release(probe);
		if (child)
			obs_source_release(child);
		return false;
	}

	obs_source_filter_add(child, probe);
	obs_source_add_active_child(source->source, child);
	mutex_lock(&source->child_mutex);
	source->media_source = child;
	source->frame_probe = probe;
	mutex_unlock(&source->child_mutex);

	atomic_store_explicit(&source->received_video, false, memory_order_release);
	atomic_store_explicit(&source->last_video_at_ns, os_gettime_ns(), memory_order_release);
	atomic_store_explicit(&source->stopping, false, memory_order_release);
#ifdef _WIN32
	source->receiver_thread = CreateThread(NULL, 0, receiver_thread, source, 0, NULL);
	const bool thread_created = source->receiver_thread != NULL;
#else
	const bool thread_created = pthread_create(&source->receiver_thread, NULL, receiver_thread, source) == 0;
#endif
	if (!thread_created) {
		blog(LOG_ERROR, "[RIST Media Source] could not create receiver thread");
		stop_source(source);
		return false;
	}
	source->thread_started = true;
	return true;
}

static obs_source_t *get_child(struct rist_media_source *source)
{
	mutex_lock(&source->child_mutex);
	obs_source_t *child = source->media_source ? obs_source_get_ref(source->media_source) : NULL;
	mutex_unlock(&source->child_mutex);
	return child;
}

static const char *source_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("SourceName");
}

static void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, SETTING_URL, "");
	obs_data_set_default_int(settings, SETTING_PROFILE, RIST_PROFILE_MAIN);
	obs_data_set_default_bool(settings, SETTING_AUTH_ENABLED, false);
	obs_data_set_default_int(settings, SETTING_RECOVERY_MS, DEFAULT_RECOVERY_MS);
	obs_data_set_default_int(settings, SETTING_STALE_MS, DEFAULT_STALE_MS);
	obs_data_set_default_int(settings, SETTING_RECONNECT_MS, DEFAULT_RECONNECT_MS);
	obs_data_set_default_bool(settings, SETTING_HW_DECODE, false);
	obs_data_set_default_bool(settings, SETTING_AUDIO_TRACKS_INITIALIZED, false);
	obs_data_set_default_bool(settings, SETTING_ENCRYPTION_ENABLED, false);
	obs_data_set_default_int(settings, SETTING_KEY_SIZE, 256);
}

static bool security_modified(obs_properties_t *properties, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	const bool supports_security = obs_data_get_int(settings, SETTING_PROFILE) != RIST_PROFILE_SIMPLE;
	const bool show_auth = supports_security && obs_data_get_bool(settings, SETTING_AUTH_ENABLED);
	const bool show_encryption =
		supports_security && obs_data_get_bool(settings, SETTING_ENCRYPTION_ENABLED);

	obs_property_set_enabled(obs_properties_get(properties, SETTING_AUTH_ENABLED), supports_security);
	obs_property_set_visible(obs_properties_get(properties, SETTING_USERNAME), show_auth);
	obs_property_set_visible(obs_properties_get(properties, SETTING_PASSWORD), show_auth);
	obs_property_set_enabled(obs_properties_get(properties, SETTING_ENCRYPTION_ENABLED), supports_security);
	obs_property_set_visible(obs_properties_get(properties, SETTING_SECRET), show_encryption);
	obs_property_set_visible(obs_properties_get(properties, SETTING_KEY_SIZE), show_encryption);
	return true;
}

static obs_properties_t *source_properties(void *data)
{
	struct rist_media_source *source = data;
	obs_properties_t *properties = obs_properties_create();
	obs_properties_add_text(properties, SETTING_URL, obs_module_text("Source.URL"), OBS_TEXT_DEFAULT);

	obs_property_t *profile = obs_properties_add_list(properties, SETTING_PROFILE, obs_module_text("Source.Profile"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(profile, obs_module_text("Source.Profile.Simple"), RIST_PROFILE_SIMPLE);
	obs_property_list_add_int(profile, obs_module_text("Source.Profile.Main"), RIST_PROFILE_MAIN);
	obs_property_list_add_int(profile, obs_module_text("Source.Profile.Advanced"), RIST_PROFILE_ADVANCED);
	obs_property_set_modified_callback(profile, security_modified);
	obs_property_set_long_description(profile, obs_module_text("Source.Profile.Description"));

	obs_property_t *auth = obs_properties_add_bool(properties, SETTING_AUTH_ENABLED,
						       obs_module_text("Source.Auth"));
	obs_property_set_modified_callback(auth, security_modified);
	obs_properties_add_text(properties, SETTING_USERNAME, obs_module_text("Source.Username"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(properties, SETTING_PASSWORD, obs_module_text("Source.Password"), OBS_TEXT_PASSWORD);

	obs_property_t *encryption = obs_properties_add_bool(properties, SETTING_ENCRYPTION_ENABLED,
						             obs_module_text("Source.Encryption"));
	obs_property_set_modified_callback(encryption, security_modified);
	obs_properties_add_text(properties, SETTING_SECRET, obs_module_text("Source.Secret"), OBS_TEXT_PASSWORD);

	obs_property_t *key_size = obs_properties_add_list(properties, SETTING_KEY_SIZE,
						      obs_module_text("Source.Encryption.Mode"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(key_size, "AES-128", 128);
	obs_property_list_add_int(key_size, "AES-256", 256);

	obs_property_t *stale = obs_properties_add_int(properties, SETTING_STALE_MS,
						     obs_module_text("Source.Stale"), 0, MAX_TIMEOUT_MS, 50);
	obs_property_int_set_suffix(stale, " ms");
	obs_property_set_long_description(stale, obs_module_text("Source.Stale.Description"));
	obs_property_t *recovery = obs_properties_add_int(properties, SETTING_RECOVERY_MS,
							obs_module_text("Source.Recovery"), 1, MAX_RECOVERY_MS, 50);
	obs_property_int_set_suffix(recovery, " ms");
	obs_property_t *reconnect = obs_properties_add_int(properties, SETTING_RECONNECT_MS,
							  obs_module_text("Source.Reconnect"), 100, MAX_TIMEOUT_MS, 100);
	obs_property_int_set_suffix(reconnect, " ms");
	obs_properties_add_bool(properties, SETTING_HW_DECODE, obs_module_text("Source.HardwareDecode"));
	obs_data_t *settings = source ? obs_source_get_settings(source->source) : obs_data_create();
	if (!source)
		source_defaults(settings);
	security_modified(properties, NULL, settings);
	obs_data_release(settings);
	return properties;
}

static void source_update(void *data, obs_data_t *settings)
{
	struct rist_media_source *source = data;
	stop_source(source);
	free_settings(source);

	source->url = copy_string(obs_data_get_string(settings, SETTING_URL));
	int64_t profile = obs_data_get_int(settings, SETTING_PROFILE);
	if (profile < RIST_PROFILE_SIMPLE || profile > RIST_PROFILE_ADVANCED)
		profile = RIST_PROFILE_MAIN;
	source->profile = (enum rist_profile)profile;
	const bool supports_security = source->profile != RIST_PROFILE_SIMPLE;
	source->auth_enabled = supports_security && obs_data_get_bool(settings, SETTING_AUTH_ENABLED);
	source->encryption_enabled =
		supports_security && obs_data_get_bool(settings, SETTING_ENCRYPTION_ENABLED);
	if (source->auth_enabled) {
		source->username = copy_string(obs_data_get_string(settings, SETTING_USERNAME));
		source->password = copy_string(obs_data_get_string(settings, SETTING_PASSWORD));
	}
	if (source->encryption_enabled)
		source->secret = copy_string(obs_data_get_string(settings, SETTING_SECRET));
	source->recovery_ms = clamp_u32(obs_data_get_int(settings, SETTING_RECOVERY_MS), 1, MAX_RECOVERY_MS);
	source->reconnect_ms = clamp_u32(obs_data_get_int(settings, SETTING_RECONNECT_MS), 100, MAX_TIMEOUT_MS);
	uint32_t stale_ms = clamp_u32(obs_data_get_int(settings, SETTING_STALE_MS), 0, MAX_TIMEOUT_MS);
	atomic_store_explicit(&source->stale_ns, (uint64_t)stale_ms * NSEC_PER_MSEC, memory_order_release);
	source->key_size = source->secret ? (int)obs_data_get_int(settings, SETTING_KEY_SIZE) : 0;
	if (source->key_size != 128 && source->key_size != 256)
		source->key_size = 0;
	source->hw_decode = obs_data_get_bool(settings, SETTING_HW_DECODE);
	start_source(source);
}

static void source_load(void *data, obs_data_t *settings)
{
	struct rist_media_source *source = data;

	if (obs_data_get_bool(settings, SETTING_AUDIO_TRACKS_INITIALIZED))
		return;

	/* Sources saved before audio output support have a zero mixer mask. */
	if (obs_source_get_audio_mixers(source->source) == 0)
		obs_source_set_audio_mixers(source->source, 0x3F);

	obs_data_set_bool(settings, SETTING_AUDIO_TRACKS_INITIALIZED, true);
}

static void *source_create(obs_data_t *settings, obs_source_t *context)
{
	struct rist_media_source *source = bzalloc(sizeof(*source));
	source->source = context;
	mutex_init(&source->child_mutex);
	atomic_init(&source->stopping, true);
	atomic_init(&source->received_video, false);
	atomic_init(&source->last_video_at_ns, 0);
	atomic_init(&source->stale_ns, (uint64_t)DEFAULT_STALE_MS * NSEC_PER_MSEC);
	source_update(source, settings);
	return source;
}

static void source_destroy(void *data)
{
	struct rist_media_source *source = data;
	stop_source(source);
	free_settings(source);
	mutex_destroy(&source->child_mutex);
	bfree(source);
}

static bool video_is_stale(struct rist_media_source *source)
{
	if (!atomic_load_explicit(&source->received_video, memory_order_acquire))
		return true;
	const uint64_t stale_ns = atomic_load_explicit(&source->stale_ns, memory_order_acquire);
	if (stale_ns == 0)
		return false;
	const uint64_t now = os_gettime_ns();
	const uint64_t last = atomic_load_explicit(&source->last_video_at_ns, memory_order_acquire);
	return now >= last && now - last >= stale_ns;
}

static void source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct rist_media_source *source = data;
	if (video_is_stale(source))
		return;
	obs_source_t *child = get_child(source);
	if (child) {
		obs_source_video_render(child);
		obs_source_release(child);
	}
}

static uint32_t source_width(void *data)
{
	obs_source_t *child = get_child(data);
	uint32_t width = child ? obs_source_get_width(child) : 0;
	if (child)
		obs_source_release(child);
	return width;
}

static uint32_t source_height(void *data)
{
	obs_source_t *child = get_child(data);
	uint32_t height = child ? obs_source_get_height(child) : 0;
	if (child)
		obs_source_release(child);
	return height;
}


static enum gs_color_space source_color_space(void *data, size_t count,
					       const enum gs_color_space *preferred_spaces)
{
	obs_source_t *child = get_child(data);
	enum gs_color_space color_space =
		child ? obs_source_get_color_space(child, count, preferred_spaces) : GS_CS_SRGB;
	if (child)
		obs_source_release(child);
	return color_space;
}

static struct obs_source_info rist_media_source_info = {
	.id = "rist_media_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = source_name,
	.create = source_create,
	.destroy = source_destroy,
	.get_defaults = source_defaults,
	.get_properties = source_properties,
	.update = source_update,
	.load = source_load,
	.video_render = source_render,
	.get_width = source_width,
	.get_height = source_height,
	.video_get_color_space = source_color_space,
	.icon_type = OBS_ICON_TYPE_MEDIA,
};

bool rist_media_source_init(void)
{
#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		blog(LOG_ERROR, "[RIST Media Source] Winsock initialization failed");
		return false;
	}
#endif
	obs_register_source(&frame_probe_info);
	obs_register_source(&rist_media_source_info);
	blog(LOG_INFO, "[RIST Media Source] private libRIST %s", librist_version());
	return true;
}

void rist_media_source_shutdown(void)
{
#ifdef _WIN32
	WSACleanup();
#endif
}
