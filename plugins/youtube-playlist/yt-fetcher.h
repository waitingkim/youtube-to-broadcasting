#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Background downloader shared by all youtube-playlist sources.
 *
 * Remote items (http/https URLs) are downloaded with a private yt-dlp binary
 * (installed on first use into the module config directory, checksum
 * verified) and remuxed into <cache>/<key>.mkv.  Requests are served in FIFO
 * order by a single worker thread. */

enum yt_fetch_state {
	YT_FETCH_PENDING, /* queued or downloading */
	YT_FETCH_READY,
	YT_FETCH_FAILED,
};

void yt_fetcher_init(void);
void yt_fetcher_free(void);

/* true if `item` must go through the fetcher (http/https URL) */
bool yt_fetcher_is_remote(const char *item);

/* Non-blocking.  Queues the item if unknown.  When READY, *path_out receives
 * a bstrdup'ed local file path (caller frees). */
enum yt_fetch_state yt_fetcher_request(const char *url, int max_height, char **path_out);

/* Cache size limit in bytes (LRU eviction after each download); 0 = none. */
void yt_fetcher_set_cache_limit(long long bytes);

/* Playlists and channels -------------------------------------------------- */

/* true for YouTube playlist (list= without v=) and channel URLs */
bool yt_fetcher_is_collection(const char *url);

/* Non-blocking.  Queues expansion if unknown.  When READY, *urls_out receives
 * a strlist copy of the video URLs (free with strlist_free).  `generation`
 * changes whenever the list content/state changes. */
enum yt_fetch_state yt_fetcher_request_list(const char *url, char ***urls_out, uint32_t *generation);

/* Cheap change detection (also queues expansion if unknown). */
uint32_t yt_fetcher_list_generation(const char *url);

/* Re-expand the playlist in the background. */
void yt_fetcher_refresh_list(const char *url);

/* Status for UIs (does not queue anything) ------------------------------- */

struct yt_item_info {
	bool known; /* requested at least once this session */
	enum yt_fetch_state state;
	bool running; /* downloading right now */
	int progress; /* 0-100 while running */
	char title[256];
	int duration; /* seconds, 0 = unknown */
};

bool yt_fetcher_peek(const char *url, struct yt_item_info *info);
