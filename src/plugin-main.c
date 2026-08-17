// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>
#include <util/platform.h>

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define SETTING_DELAY_MS "delay_ms"
#define DEFAULT_DELAY_MS 1000
#define MAX_DELAY_MS 30000
#define NSEC_PER_MSEC UINT64_C(1000000)

struct rist_stale_frame_filter {
	obs_source_t *context;
	atomic_uint_fast64_t last_frame_at_ns;
	atomic_uint_fast64_t delay_ns;
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-rist-stale-frame-filter", "en-US")
OBS_MODULE_AUTHOR("romestylez")

static const char *filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FilterName");
}

static void filter_update(void *data, obs_data_t *settings)
{
	struct rist_stale_frame_filter *filter = data;
	int64_t delay_ms = obs_data_get_int(settings, SETTING_DELAY_MS);

	if (delay_ms < 0)
		delay_ms = 0;
	if (delay_ms > MAX_DELAY_MS)
		delay_ms = MAX_DELAY_MS;

	atomic_store_explicit(&filter->delay_ns, (uint64_t)delay_ms * NSEC_PER_MSEC, memory_order_release);
	atomic_store_explicit(&filter->last_frame_at_ns, os_gettime_ns(), memory_order_release);
}

static void *filter_create(obs_data_t *settings, obs_source_t *context)
{
	struct rist_stale_frame_filter *filter = calloc(1, sizeof(*filter));
	if (!filter)
		return NULL;

	filter->context = context;
	atomic_init(&filter->last_frame_at_ns, os_gettime_ns());
	atomic_init(&filter->delay_ns, (uint64_t)DEFAULT_DELAY_MS * NSEC_PER_MSEC);
	filter_update(filter, settings);
	return filter;
}

static void filter_destroy(void *data)
{
	free(data);
}

static void filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_DELAY_MS, DEFAULT_DELAY_MS);
}

static obs_properties_t *filter_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *properties = obs_properties_create();
	obs_property_t *delay = obs_properties_add_int_slider(properties, SETTING_DELAY_MS,
							     obs_module_text("Delay"), 0, MAX_DELAY_MS, 100);
	obs_property_int_set_suffix(delay, " ms");
	obs_property_set_long_description(delay, obs_module_text("Delay.Description"));
	return properties;
}

static struct obs_source_frame *filter_video(void *data, struct obs_source_frame *frame)
{
	struct rist_stale_frame_filter *filter = data;
	if (frame)
		atomic_store_explicit(&filter->last_frame_at_ns, os_gettime_ns(), memory_order_release);
	return frame;
}

static void filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct rist_stale_frame_filter *filter = data;
	const uint64_t delay_ns = atomic_load_explicit(&filter->delay_ns, memory_order_acquire);

	if (delay_ns == 0) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const uint64_t now_ns = os_gettime_ns();
	const uint64_t last_frame_at_ns =
		atomic_load_explicit(&filter->last_frame_at_ns, memory_order_acquire);
	const bool stale = now_ns >= last_frame_at_ns && now_ns - last_frame_at_ns >= delay_ns;

	if (stale) {
		/* Drawing nothing is intentional: OBS composites the source as transparent. */
		return;
	}

	obs_source_skip_video_filter(filter->context);
}

static struct obs_source_info rist_stale_frame_filter_info = {
	.id = "rist_stale_frame_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_ASYNC,
	.get_name = filter_name,
	.create = filter_create,
	.destroy = filter_destroy,
	.get_defaults = filter_defaults,
	.get_properties = filter_properties,
	.update = filter_update,
	.video_render = filter_render,
	.filter_video = filter_video,
};

bool obs_module_load(void)
{
	obs_register_source(&rist_stale_frame_filter_info);
	blog(LOG_INFO, "[RIST Stale Frame Fix] loaded");
	return true;
}

const char *obs_module_description(void)
{
	return "Makes a media source transparent when it stops delivering fresh video frames.";
}
