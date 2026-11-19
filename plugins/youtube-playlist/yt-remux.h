#pragma once

#include <util/dstr.h>
#include <stdbool.h>

/* Combine the best video stream of `video_path` and the best audio stream of
 * `audio_path` (may be NULL to take audio from the video file) into a
 * Matroska file at `out_path` without re-encoding.
 * `cancel` is polled between packets. */
bool yt_remux(const char *video_path, const char *audio_path, const char *out_path, volatile bool *cancel,
	      struct dstr *error);
