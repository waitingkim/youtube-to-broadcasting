#pragma once

#include <stdbool.h>

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
