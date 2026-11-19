#include "yt-fetcher.h"
#include "yt-proc.h"
#include "yt-remux.h"

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/bmem.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <stdlib.h>

#define FETCH_LOG(level, format, ...) blog(level, "[youtube-playlist fetcher] " format, ##__VA_ARGS__)

#if defined(__APPLE__)
/* The one-file build re-extracts itself on every launch, and macOS scans the
 * extracted libraries each time (~9 s per run).  The one-dir zip build is
 * extracted once, so later launches start in ~0.2 s. */
#define YTDLP_ASSET "yt-dlp_macos.zip"
#define YTDLP_ZIP_DIR "yt-dlp_macos"
#define YTDLP_REL_PATH "bin/yt-dlp_macos/yt-dlp_macos"
#define SHA256_CMD "/usr/bin/shasum", "-a", "256"
#elif defined(__linux__) && defined(__aarch64__)
#define YTDLP_ASSET "yt-dlp_linux_aarch64"
#define SHA256_CMD "/usr/bin/sha256sum"
#else
#define YTDLP_ASSET "yt-dlp_linux"
#define SHA256_CMD "/usr/bin/sha256sum"
#endif

#define YTDLP_RELEASE_URL "https://github.com/yt-dlp/yt-dlp/releases/latest/download/"
#ifndef YTDLP_REL_PATH
#define YTDLP_REL_PATH "bin/yt-dlp"
#endif

#define CURL_BIN "/usr/bin/curl"

struct fetch_entry {
	char *key;
	char *url;
	int max_height;
	enum yt_fetch_state state;
	bool running;
	char *path;
	uint64_t last_touch_ns;
};

static struct {
	pthread_mutex_t mutex;
	os_sem_t *sem;
	pthread_t thread;
	bool thread_ok;
	volatile bool stop;

	DARRAY(struct fetch_entry) entries;

	char *cache_dir;
	char *bin_dir;
	char *ytdlp_path;
	bool ytdlp_ready;
	bool ytdlp_failed;

	volatile long long cache_limit; /* bytes, 0 = unlimited */
	volatile bool evict_requested;
} f;

#define DEFAULT_CACHE_LIMIT (10LL * 1024 * 1024 * 1024)
#define TOUCH_INTERVAL_NS (60ULL * 1000000000ULL)
#define KEEP_NEWEST 3 /* never evict the most recently used files */

/* ------------------------------------------------------------------------- */
/* keys                                                                      */

static bool is_id_char(char c)
{
	return isalnum((unsigned char)c) || c == '-' || c == '_';
}

/* extract an 11 character YouTube video id following `marker` */
static bool id_after(const char *url, const char *marker, struct dstr *id)
{
	const char *p = strstr(url, marker);
	if (!p)
		return false;
	p += strlen(marker);
	size_t n = 0;
	while (is_id_char(p[n]))
		n++;
	if (n != 11)
		return false;
	dstr_ncopy(id, p, 11);
	return true;
}

static void make_key(const char *url, struct dstr *key)
{
	static const char *markers[] = {"?v=", "&v=", "youtu.be/", "/shorts/", "/live/", "/embed/", "/v/"};
	struct dstr id = {0};

	if (strstr(url, "youtube.com") || strstr(url, "youtu.be")) {
		for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
			if (id_after(url, markers[i], &id)) {
				dstr_printf(key, "yt-%s", id.array);
				dstr_free(&id);
				return;
			}
		}
	}

	/* generic URL: FNV-1a hash */
	uint64_t h = 1469598103934665603ULL;
	for (const char *p = url; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	dstr_printf(key, "url-%016llx", (unsigned long long)h);
}

static char *cache_file_for(const char *key)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s/%s.mkv", f.cache_dir, key);
	return path.array;
}

/* ------------------------------------------------------------------------- */
/* yt-dlp installation                                                       */

static bool file_sha256(const char *path, struct dstr *hex)
{
	const char *argv[] = {SHA256_CMD, path, NULL};
	struct dstr out = {0};
	bool ok = yt_proc_run(argv, &out) == 0 && out.len >= 64;
	if (ok)
		dstr_ncopy(hex, out.array, 64);
	dstr_free(&out);
	return ok;
}

static bool expected_sha256(const char *sums_path, struct dstr *hex)
{
	char *sums = os_quick_read_utf8_file(sums_path);
	bool found = false;
	if (!sums)
		return false;

	char **lines = strlist_split(sums, '\n', false);
	for (char **line = lines; line && *line; line++) {
		/* "<hash>  <asset>" */
		size_t len = strlen(*line);
		size_t alen = strlen(YTDLP_ASSET);
		if (len > 66 && len >= alen + 2 && strcmp(*line + len - alen, YTDLP_ASSET) == 0 &&
		    (*line)[len - alen - 1] == ' ') {
			dstr_ncopy(hex, *line, 64);
			found = true;
			break;
		}
	}
	strlist_free(lines);
	bfree(sums);
	return found;
}

static bool download_file(const char *url, const char *dest)
{
	const char *argv[] = {CURL_BIN, "-fsSL", "--retry", "2", "-o", dest, url, NULL};
	struct dstr out = {0};
	int code = yt_proc_run(argv, &out);
	if (code != 0)
		FETCH_LOG(LOG_WARNING, "download failed (%d) %s: %s", code, url, out.array);
	dstr_free(&out);
	return code == 0;
}

static bool install_ytdlp(void)
{
	struct dstr tmp = {0}, sums = {0}, actual = {0}, expected = {0};
	bool ok = false;

	os_mkdirs(f.bin_dir);
	dstr_printf(&tmp, "%s/yt-dlp.download", f.bin_dir);
	dstr_printf(&sums, "%s/SHA2-256SUMS", f.bin_dir);

	FETCH_LOG(LOG_INFO, "Installing yt-dlp (%s) into %s", YTDLP_ASSET, f.bin_dir);

	if (!download_file(YTDLP_RELEASE_URL YTDLP_ASSET, tmp.array) ||
	    !download_file(YTDLP_RELEASE_URL "SHA2-256SUMS", sums.array))
		goto done;

	if (!expected_sha256(sums.array, &expected) || !file_sha256(tmp.array, &actual)) {
		FETCH_LOG(LOG_WARNING, "Could not compute/find yt-dlp checksum");
		goto done;
	}
	if (dstr_cmpi(&expected, actual.array) != 0) {
		FETCH_LOG(LOG_WARNING, "yt-dlp checksum mismatch (expected %s, got %s)", expected.array, actual.array);
		goto done;
	}
#ifdef YTDLP_ZIP_DIR
	{
		struct dstr dir = {0}, out = {0};
		dstr_printf(&dir, "%s/%s", f.bin_dir, YTDLP_ZIP_DIR);
		const char *argv[] = {"/usr/bin/ditto", "-x", "-k", tmp.array, dir.array, NULL};
		int code = yt_proc_run(argv, &out);
		if (code != 0)
			FETCH_LOG(LOG_WARNING, "Could not extract yt-dlp: %s", out.array);
		dstr_free(&dir);
		dstr_free(&out);
		if (code != 0 || !os_file_exists(f.ytdlp_path))
			goto done;
	}
#else
	if (chmod(tmp.array, 0755) != 0 || os_rename(tmp.array, f.ytdlp_path) != 0) {
		FETCH_LOG(LOG_WARNING, "Could not install yt-dlp binary");
		goto done;
	}
#endif
	FETCH_LOG(LOG_INFO, "yt-dlp installed (sha256 %s)", actual.array);
	ok = true;

done:
	os_unlink(tmp.array);
	os_unlink(sums.array);
	dstr_free(&tmp);
	dstr_free(&sums);
	dstr_free(&actual);
	dstr_free(&expected);
	return ok;
}

static bool ensure_ytdlp(void)
{
	if (f.ytdlp_ready)
		return true;
	if (f.ytdlp_failed)
		return false;

	if (!os_file_exists(f.ytdlp_path) && !install_ytdlp()) {
		f.ytdlp_failed = true;
		return false;
	}

	const char *argv[] = {f.ytdlp_path, "--version", NULL};
	struct dstr out = {0};
	if (yt_proc_run(argv, &out) == 0) {
		dstr_depad(&out);
		FETCH_LOG(LOG_INFO, "Using yt-dlp %s (%s)", out.array, f.ytdlp_path);
		f.ytdlp_ready = true;
	} else {
		FETCH_LOG(LOG_WARNING, "yt-dlp is not runnable: %s", out.array);
		f.ytdlp_failed = true;
	}
	dstr_free(&out);
	return f.ytdlp_ready;
}

/* ------------------------------------------------------------------------- */
/* download job                                                              */

static void log_ytdlp_errors(const char *url, const char *output)
{
	char **lines = strlist_split(output ? output : "", '\n', false);
	bool any = false;
	for (char **line = lines; line && *line; line++) {
		if (strstr(*line, "ERROR")) {
			FETCH_LOG(LOG_WARNING, "%s: %s", url, *line);
			any = true;
		}
	}
	if (!any)
		FETCH_LOG(LOG_WARNING, "%s: yt-dlp failed: %s", url, output ? output : "");
	strlist_free(lines);
}

static bool run_job(const char *key, const char *url, int max_height, const char *final_path)
{
	struct dstr fmt = {0}, tmpl = {0}, out = {0}, part = {0}, err = {0};
	DARRAY(char *) files;
	bool ok = false;
	da_init(files);

	if (!ensure_ytdlp())
		goto done;

	os_mkdirs(f.cache_dir);

	dstr_printf(&fmt, "bv*[height<=%d][vcodec^=avc1]/bv*[height<=%d]/bv*,ba[ext=m4a]/ba", max_height, max_height);
	dstr_printf(&tmpl, "%s/%s.f%%(format_id)s.%%(ext)s", f.cache_dir, key);

	const char *argv[] = {f.ytdlp_path,
			      "-q",
			      "--no-warnings",
			      "--no-playlist",
			      "--no-progress",
			      "--no-mtime",
			      "--match-filter",
			      "!is_live",
			      "-f",
			      fmt.array,
			      "-o",
			      tmpl.array,
			      "--print",
			      "after_move:filepath",
			      "--",
			      url,
			      NULL};

	uint64_t t0 = os_gettime_ns();
	int code = yt_proc_run(argv, &out);
	if (f.stop)
		goto done;

	char **lines = strlist_split(out.array ? out.array : "", '\n', false);
	for (char **line = lines; line && *line; line++) {
		struct dstr l = {0};
		dstr_copy(&l, *line);
		dstr_depad(&l);
		if (l.len && os_file_exists(l.array)) {
			char *copy = bstrdup(l.array);
			da_push_back(files, &copy);
		}
		dstr_free(&l);
	}
	strlist_free(lines);

	if (code != 0 || !files.num) {
		if (code == 0)
			FETCH_LOG(LOG_WARNING, "%s: nothing downloaded (live stream or unavailable)", url);
		else
			log_ytdlp_errors(url, out.array);
		goto done;
	}

	FETCH_LOG(LOG_INFO, "%s: downloaded %zu file(s) in %.1fs", url, files.num,
		  (double)(os_gettime_ns() - t0) / 1e9);

	dstr_printf(&part, "%s.part", final_path);
	if (!yt_remux(files.array[0], files.num > 1 ? files.array[1] : NULL, part.array, &f.stop, &err)) {
		FETCH_LOG(LOG_WARNING, "%s: remux failed: %s", url, err.array);
		os_unlink(part.array);
		goto done;
	}
	if (os_rename(part.array, final_path) != 0) {
		FETCH_LOG(LOG_WARNING, "%s: could not move %s", url, part.array);
		os_unlink(part.array);
		goto done;
	}
	ok = true;

done:
	for (size_t i = 0; i < files.num; i++) {
		os_unlink(files.array[i]);
		bfree(files.array[i]);
	}
	da_free(files);
	dstr_free(&fmt);
	dstr_free(&tmpl);
	dstr_free(&out);
	dstr_free(&part);
	dstr_free(&err);
	return ok;
}


/* ------------------------------------------------------------------------- */
/* cache maintenance (worker thread)                                         */

struct cache_file {
	char *path;
	char *key;
	long long size;
	time_t mtime;
};

static int cmp_mtime(const void *a, const void *b)
{
	const struct cache_file *x = a, *y = b;
	return (x->mtime > y->mtime) - (x->mtime < y->mtime);
}

static bool ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Remove leftovers of interrupted downloads/remuxes. */
static void cache_remove_partials(void)
{
	os_dir_t *dir = os_opendir(f.cache_dir);
	if (!dir)
		return;
	struct os_dirent *ent;
	struct dstr path = {0};
	size_t removed = 0;
	while ((ent = os_readdir(dir)) != NULL) {
		if (ent->directory || ent->d_name[0] == '.' || ends_with(ent->d_name, ".mkv"))
			continue;
		dstr_printf(&path, "%s/%s", f.cache_dir, ent->d_name);
		if (os_unlink(path.array) == 0)
			removed++;
	}
	os_closedir(dir);
	dstr_free(&path);
	if (removed)
		FETCH_LOG(LOG_INFO, "Removed %zu partial file(s) from cache", removed);
}

static bool key_is_running(const char *key)
{
	bool running = false;
	pthread_mutex_lock(&f.mutex);
	for (size_t i = 0; i < f.entries.num; i++) {
		if (f.entries.array[i].running && strcmp(f.entries.array[i].key, key) == 0) {
			running = true;
			break;
		}
	}
	pthread_mutex_unlock(&f.mutex);
	return running;
}

/* Least-recently-used eviction down to the configured limit.  Files are
 * "used" when a source requests them (mtime is refreshed, see request). */
static void cache_evict(void)
{
	long long limit = f.cache_limit;
	if (limit <= 0)
		return;

	os_dir_t *dir = os_opendir(f.cache_dir);
	if (!dir)
		return;

	DARRAY(struct cache_file) files;
	da_init(files);
	long long total = 0;
	struct os_dirent *ent;

	while ((ent = os_readdir(dir)) != NULL) {
		if (ent->directory || !ends_with(ent->d_name, ".mkv"))
			continue;
		struct cache_file cf = {0};
		struct dstr path = {0};
		struct stat st;
		dstr_printf(&path, "%s/%s", f.cache_dir, ent->d_name);
		if (stat(path.array, &st) != 0) {
			dstr_free(&path);
			continue;
		}
		cf.path = path.array;
		cf.key = bstrdup_n(ent->d_name, strlen(ent->d_name) - 4);
		cf.size = (long long)st.st_size;
		cf.mtime = st.st_mtime;
		total += cf.size;
		da_push_back(files, &cf);
	}
	os_closedir(dir);

	if (total > limit && files.num > KEEP_NEWEST) {
		qsort(files.array, files.num, sizeof(struct cache_file), cmp_mtime);
		for (size_t i = 0; i + KEEP_NEWEST < files.num && total > limit; i++) {
			struct cache_file *cf = &files.array[i];
			if (key_is_running(cf->key))
				continue;
			/* unlinking a file that is being played is safe on POSIX: the
			 * open descriptor stays valid until the player closes it */
			if (os_unlink(cf->path) == 0) {
				total -= cf->size;
				FETCH_LOG(LOG_INFO, "Cache limit: evicted %s (%.1f MB)", cf->key,
					  (double)cf->size / (1024.0 * 1024.0));
			}
		}
	}

	FETCH_LOG(LOG_DEBUG, "Cache usage %.1f MB / limit %.1f MB", (double)total / (1024.0 * 1024.0),
		  (double)limit / (1024.0 * 1024.0));

	for (size_t i = 0; i < files.num; i++) {
		bfree(files.array[i].path);
		bfree(files.array[i].key);
	}
	da_free(files);
}

static struct fetch_entry *find_entry(const char *key)
{
	for (size_t i = 0; i < f.entries.num; i++)
		if (strcmp(f.entries.array[i].key, key) == 0)
			return &f.entries.array[i];
	return NULL;
}

static void *worker(void *unused)
{
	UNUSED_PARAMETER(unused);
	os_set_thread_name("yt-playlist-fetcher");

	cache_remove_partials();
	cache_evict();

	while (os_sem_wait(f.sem) == 0 && !f.stop) {
		char *key = NULL, *url = NULL;
		int max_height = 0;

		if (os_atomic_exchange_bool(&f.evict_requested, false))
			cache_evict();

		pthread_mutex_lock(&f.mutex);
		for (size_t i = 0; i < f.entries.num; i++) {
			struct fetch_entry *e = &f.entries.array[i];
			if (e->state == YT_FETCH_PENDING && !e->running) {
				e->running = true;
				key = bstrdup(e->key);
				url = bstrdup(e->url);
				max_height = e->max_height;
				break;
			}
		}
		pthread_mutex_unlock(&f.mutex);

		if (!key)
			continue;

		char *path = cache_file_for(key);
		FETCH_LOG(LOG_INFO, "Fetching %s", url);
		bool ok = run_job(key, url, max_height, path);
		if (ok)
			FETCH_LOG(LOG_INFO, "Ready: %s -> %s", url, path);

		pthread_mutex_lock(&f.mutex);
		struct fetch_entry *e = find_entry(key);
		if (e) {
			e->running = false;
			e->state = ok ? YT_FETCH_READY : YT_FETCH_FAILED;
			if (ok) {
				e->path = path;
				path = NULL;
			}
		}
		pthread_mutex_unlock(&f.mutex);

		bfree(path);
		bfree(key);
		bfree(url);

		if (ok)
			cache_evict();
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* public API                                                                */

void yt_fetcher_init(void)
{
	memset(&f, 0, sizeof(f));
	pthread_mutex_init(&f.mutex, NULL);
	os_sem_init(&f.sem, 0);
	da_init(f.entries);

	f.cache_limit = DEFAULT_CACHE_LIMIT;
	f.cache_dir = obs_module_config_path("cache");
	f.bin_dir = obs_module_config_path("bin");
	f.ytdlp_path = obs_module_config_path(YTDLP_REL_PATH);

	yt_proc_init();
	f.thread_ok = pthread_create(&f.thread, NULL, worker, NULL) == 0;
}

void yt_fetcher_free(void)
{
	f.stop = true;
	yt_proc_cancel_all();
	if (f.thread_ok) {
		os_sem_post(f.sem);
		pthread_join(f.thread, NULL);
	}
	yt_proc_free();

	for (size_t i = 0; i < f.entries.num; i++) {
		bfree(f.entries.array[i].key);
		bfree(f.entries.array[i].url);
		bfree(f.entries.array[i].path);
	}
	da_free(f.entries);
	os_sem_destroy(f.sem);
	pthread_mutex_destroy(&f.mutex);
	bfree(f.cache_dir);
	bfree(f.bin_dir);
	bfree(f.ytdlp_path);
}

bool yt_fetcher_is_remote(const char *item)
{
	return item && (astrcmpi_n(item, "http://", 7) == 0 || astrcmpi_n(item, "https://", 8) == 0);
}

enum yt_fetch_state yt_fetcher_request(const char *url, int max_height, char **path_out)
{
	struct dstr key = {0};
	enum yt_fetch_state state;

	make_key(url, &key);

	pthread_mutex_lock(&f.mutex);
	struct fetch_entry *e = find_entry(key.array);
	if (!e) {
		struct fetch_entry n = {0};
		n.key = bstrdup(key.array);
		n.url = bstrdup(url);
		n.max_height = max_height;

		char *cached = cache_file_for(key.array);
		if (os_file_exists(cached)) {
			n.state = YT_FETCH_READY;
			n.path = cached;
		} else {
			n.state = YT_FETCH_PENDING;
			bfree(cached);
			os_sem_post(f.sem);
		}
		e = da_push_back_new(f.entries);
		*e = n;
	}
	if (e->state == YT_FETCH_READY && !os_file_exists(e->path)) {
		/* evicted from the cache (or deleted by hand): fetch again */
		bfree(e->path);
		e->path = NULL;
		e->state = YT_FETCH_PENDING;
		e->max_height = max_height;
		os_sem_post(f.sem);
	}
	if (e->state == YT_FETCH_READY) {
		uint64_t now = os_gettime_ns();
		if (!e->last_touch_ns || now - e->last_touch_ns > TOUCH_INTERVAL_NS) {
			e->last_touch_ns = now;
			utime(e->path, NULL); /* mark as recently used for LRU */
		}
	}
	state = e->state;
	if (state == YT_FETCH_READY && path_out)
		*path_out = bstrdup(e->path);
	pthread_mutex_unlock(&f.mutex);

	dstr_free(&key);
	return state;
}

void yt_fetcher_set_cache_limit(long long bytes)
{
	long long old = f.cache_limit;
	f.cache_limit = bytes;
	if (bytes > 0 && (old <= 0 || bytes < old)) {
		FETCH_LOG(LOG_INFO, "Cache limit set to %.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
		/* a lower limit may already be exceeded: let the worker trim now */
		os_atomic_set_bool(&f.evict_requested, true);
		if (f.thread_ok)
			os_sem_post(f.sem);
	}
}
