// SPDX-License-Identifier: GPL-2.0-or-later

#include <obs-module.h>

#include "rist-media-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-rist-media-source", "en-US")
OBS_MODULE_AUTHOR("romestylez")

bool obs_module_load(void)
{
	if (!rist_media_source_init())
		return false;
	blog(LOG_INFO, "[RIST Media Source] loaded");
	return true;
}

void obs_module_unload(void)
{
	rist_media_source_shutdown();
}

const char *obs_module_description(void)
{
	return "Provides a dedicated RIST media source with private libRIST.";
}
