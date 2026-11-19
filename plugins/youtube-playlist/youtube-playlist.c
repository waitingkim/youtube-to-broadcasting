#include <obs-module.h>

#include "yt-fetcher.h"
#include "yt-oauth.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("youtube-playlist", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "YouTube playlist source (gapless sequential playback)";
}

extern struct obs_source_info yt_playlist_source_info;

bool obs_module_load(void)
{
	yt_fetcher_init();
	yt_oauth_init();
	obs_register_source(&yt_playlist_source_info);
	return true;
}

void obs_module_unload(void)
{
	yt_fetcher_free(); /* joins the worker, which may use the API */
	yt_oauth_free();
}
