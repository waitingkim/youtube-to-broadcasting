#include "yt-fetcher.h"
#include "yt-proc.h"
#include "yt-remux.h"
#include "yt-oauth.h"

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
	int progress; /* 0-100 while running */
};

struct meta_entry {
	char *key;
	char *title;
	int duration; /* seconds, 0 = unknown */
};

/* An expanded playlist / channel: list of video URLs. */
struct list_entry {
	char *key;
	char *url;
	enum yt_fetch_state state;
	bool running;
	bool refresh;         /* re-expand even though READY */
	uint32_t generation;  /* bumped whenever `urls` or `state` changes */
	uint64_t fetched_ns;  /* when `urls` was obtained (0 = from disk, stale) */
	char **urls;          /* strlist, NULL-terminated */
};

#define LIST_REFRESH_NS (60ULL * 60ULL * 1000000000ULL) /* 1 hour */

static struct {
	pthread_mutex_t mutex;
	os_sem_t *sem;
	pthread_t thread;
	bool thread_ok;
	volatile bool stop;

	DARRAY(struct fetch_entry) entries;
	DARRAY(struct list_entry) lists;
	DARRAY(struct meta_entry) meta;
	char *meta_path;
	DARRAY(char *) meta_wanted; /* "key\turl" lookups queued, never repeated */
	DARRAY(char *) meta_tried;  /* keys already looked up this session */

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
/* metadata (titles/durations), persisted in cache/meta.tsv                  */

static struct meta_entry *find_meta(const char *key)
{
	for (size_t i = 0; i < f.meta.num; i++)
		if (strcmp(f.meta.array[i].key, key) == 0)
			return &f.meta.array[i];
	return NULL;
}

/* call with f.mutex held; `persist` appends to meta.tsv */
static void set_meta_locked(const char *key, const char *title, int duration, bool persist)
{
	if (!title || !*title)
		return;
	struct meta_entry *m = find_meta(key);
	if (m && m->duration == duration && strcmp(m->title, title) == 0)
		return;
	if (!m) {
		m = da_push_back_new(f.meta);
		m->key = bstrdup(key);
	} else {
		bfree(m->title);
	}
	m->title = bstrdup(title);
	m->duration = duration;

	if (persist) {
		os_mkdirs(f.cache_dir);
		FILE *fp = os_fopen(f.meta_path, "ab");
		if (fp) {
			fprintf(fp, "%s\t%d\t%s\n", key, duration, title);
			fclose(fp);
		}
	}
}

static bool str_in(char **arr, size_t n, const char *key, size_t key_len)
{
	for (size_t i = 0; i < n; i++)
		if (strncmp(arr[i], key, key_len) == 0 && (arr[i][key_len] == 0 || arr[i][key_len] == '\t'))
			return true;
	return false;
}

/* f.mutex held: schedule a title lookup for `key` unless known or tried */
static void want_meta_locked(const char *key, const char *url)
{
	size_t len = strlen(key);
	if (find_meta(key) || str_in(f.meta_tried.array, f.meta_tried.num, key, len) ||
	    str_in(f.meta_wanted.array, f.meta_wanted.num, key, len))
		return;
	struct dstr item = {0};
	dstr_printf(&item, "%s\t%s", key, url);
	da_push_back(f.meta_wanted, &item.array);
	os_sem_post(f.sem);
}

static void load_meta(void)
{
	char *text = os_quick_read_utf8_file(f.meta_path);
	if (!text)
		return;
	char **lines = strlist_split(text, '\n', false);
	for (char **line = lines; line && *line; line++) {
		char **cols = strlist_split(*line, '\t', true);
		if (cols && cols[0] && cols[1] && cols[2])
			set_meta_locked(cols[0], cols[2], atoi(cols[1]), false); /* later lines win */
		strlist_free(cols);
	}
	strlist_free(lines);
	bfree(text);
}

/* parse "<duration>\t<title>" as printed by yt-dlp ("NA" when unknown) */
static void meta_from_fields(const char *key, const char *fields)
{
	const char *tab = strchr(fields, '\t');
	if (!tab)
		return;
	int duration = atoi(fields); /* "NA" -> 0, "123.0" -> 123 */
	pthread_mutex_lock(&f.mutex);
	set_meta_locked(key, tab + 1, duration, true);
	pthread_mutex_unlock(&f.mutex);
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

static struct fetch_entry *find_entry(const char *key);

struct job_ctx {
	const char *key;
	int part;          /* 1 = video, 2 = audio */
	double last_pct;
};

static void job_line(void *opaque, const char *line)
{
	struct job_ctx *ctx = opaque;

	if (strncmp(line, "META\t", 5) == 0) {
		meta_from_fields(ctx->key, line + 5);
		return;
	}
	if (strncmp(line, "PROG\t", 5) != 0)
		return;

	double pct = atof(line + 5 + strspn(line + 5, " "));
	if (pct + 20.0 < ctx->last_pct && ctx->part == 1)
		ctx->part = 2; /* percentage restarted: second file (audio) */
	ctx->last_pct = pct;

	/* video is most of the bytes: weight it 85% */
	int overall = ctx->part == 1 ? (int)(pct * 0.85) : 85 + (int)(pct * 0.15);
	if (overall > 99)
		overall = 99;

	pthread_mutex_lock(&f.mutex);
	struct fetch_entry *e = find_entry(ctx->key);
	if (e)
		e->progress = overall;
	pthread_mutex_unlock(&f.mutex);
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
			      "--no-mtime",
			      "--match-filter",
			      "!is_live",
			      "-f",
			      fmt.array,
			      "-o",
			      tmpl.array,
			      "--print",
			      "pre_process:META\t%(duration)s\t%(title)s",
			      "--print",
			      "after_move:filepath",
			      "--progress",
			      "--newline",
			      "--progress-template",
			      "download:PROG\t%(progress._percent_str)s",
			      "--",
			      url,
			      NULL};

	struct job_ctx ctx = {.key = key, .part = 1, .last_pct = 0.0};
	uint64_t t0 = os_gettime_ns();
	int code = yt_proc_run_lines(argv, &out, job_line, &ctx);
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
		if (ent->directory || ent->d_name[0] == '.' || ends_with(ent->d_name, ".mkv") ||
		    ends_with(ent->d_name, ".list") || strcmp(ent->d_name, "meta.tsv") == 0)
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


/* ------------------------------------------------------------------------- */
/* playlists / channels                                                      */

static uint64_t fnv1a(const char *str)
{
	uint64_t h = 1469598103934665603ULL;
	for (const char *p = str; *p; p++) {
		h ^= (uint8_t)*p;
		h *= 1099511628211ULL;
	}
	return h;
}

static char *list_file_for(const char *key)
{
	struct dstr path = {0};
	dstr_printf(&path, "%s/%s.list", f.cache_dir, key);
	return path.array;
}

static struct list_entry *find_list(const char *key)
{
	for (size_t i = 0; i < f.lists.num; i++)
		if (strcmp(f.lists.array[i].key, key) == 0)
			return &f.lists.array[i];
	return NULL;
}

/* "/@name" alone lists the channel tabs, not videos: use its videos tab */
static void normalize_list_url(const char *url, struct dstr *out)
{
	dstr_copy(out, url);
	const char *at = strstr(url, "/@");
	if (!at)
		return;
	const char *rest = strchr(at + 2, '/');
	const char *query = strchr(at + 2, '?');
	if (!rest && !query) {
		dstr_cat(out, "/videos");
	} else if (!rest && query) {
		dstr_resize(out, (size_t)(query - url));
		dstr_cat(out, "/videos");
		dstr_cat(out, query);
	}
}

static bool run_list_job(const char *url, const char *list_path, char ***urls_out)
{
	struct dstr norm = {0}, out = {0}, file = {0};
	bool ok = false;
	int code = -1;

	/* Logged in: the YouTube Data API also sees the user's private
	 * playlists (yt-dlp, anonymous, does not). */
	char *playlist_id = strstr(url, "v=") ? NULL : yt_playlist_id_from_url(url);
	if (playlist_id && yt_oauth_logged_in()) {
		if (yt_api_expand_playlist(playlist_id, &out)) {
			code = 0;
			FETCH_LOG(LOG_INFO, "%s: expanded through the YouTube API", url);
		} else {
			FETCH_LOG(LOG_INFO, "%s: API expansion failed, trying yt-dlp", url);
		}
	}
	bfree(playlist_id);

	if (code != 0) {
		if (!ensure_ytdlp())
			goto done;
		normalize_list_url(url, &norm);
		const char *argv[] = {f.ytdlp_path,   "-q", "--no-warnings", "--flat-playlist", "--print",
				      "%(url)s\t%(duration)s\t%(title)s", "--", norm.array, NULL};
		code = yt_proc_run(argv, &out);
	}
	if (f.stop)
		goto done;

	char **lines = strlist_split(out.array ? out.array : "", '\n', false);
	for (char **line = lines; line && *line; line++) {
		struct dstr l = {0};
		dstr_copy(&l, *line);
		dstr_depad(&l);
		char *tab = l.array ? strchr(l.array, '\t') : NULL;
		if (tab)
			*tab = 0;
		if (l.array && yt_fetcher_is_remote(l.array)) {
			dstr_cat(&file, l.array);
			dstr_cat(&file, "\n");
			if (tab) {
				struct dstr key = {0};
				make_key(l.array, &key);
				meta_from_fields(key.array, tab + 1);
				dstr_free(&key);
			}
		}
		dstr_free(&l);
	}
	strlist_free(lines);

	if (!file.len) {
		if (code != 0)
			log_ytdlp_errors(url, out.array);
		else
			FETCH_LOG(LOG_WARNING, "%s: playlist is empty", url);
		goto done;
	}

	os_mkdirs(f.cache_dir);
	if (!os_quick_write_utf8_file(list_path, file.array, file.len, false))
		FETCH_LOG(LOG_WARNING, "%s: could not save playlist to %s", url, list_path);

	*urls_out = strlist_split(file.array, '\n', false);
	ok = true;

done:
	dstr_free(&norm);
	dstr_free(&out);
	dstr_free(&file);
	return ok;
}

static size_t strlist_count(char **list)
{
	size_t n = 0;
	while (list && list[n])
		n++;
	return n;
}

/* Process one pending list job; returns false if there was none. */
static bool work_one_list(void)
{
	char *key = NULL, *url = NULL;

	pthread_mutex_lock(&f.mutex);
	for (size_t i = 0; i < f.lists.num; i++) {
		struct list_entry *l = &f.lists.array[i];
		if (!l->running && (l->state == YT_FETCH_PENDING || l->refresh)) {
			l->running = true;
			l->refresh = false;
			key = bstrdup(l->key);
			url = bstrdup(l->url);
			break;
		}
	}
	pthread_mutex_unlock(&f.mutex);

	if (!key)
		return false;

	char *path = list_file_for(key);
	char **urls = NULL;
	uint64_t t0 = os_gettime_ns();
	FETCH_LOG(LOG_INFO, "Expanding playlist %s", url);
	bool ok = run_list_job(url, path, &urls);
	if (ok)
		FETCH_LOG(LOG_INFO, "Playlist %s: %zu video(s) in %.1fs", url, strlist_count(urls),
			  (double)(os_gettime_ns() - t0) / 1e9);

	pthread_mutex_lock(&f.mutex);
	struct list_entry *l = find_list(key);
	if (l) {
		l->running = false;
		if (ok) {
			bool same = l->state == YT_FETCH_READY && strlist_count(l->urls) == strlist_count(urls);
			for (size_t i = 0; same && urls[i]; i++)
				same = strcmp(l->urls[i], urls[i]) == 0;

			l->fetched_ns = os_gettime_ns();
			if (!same) {
				strlist_free(l->urls);
				l->urls = urls;
				urls = NULL;
				l->state = YT_FETCH_READY;
				l->generation++; /* only signal sources on real changes */
			}
		} else if (l->state != YT_FETCH_READY) {
			/* keep serving a previous (disk) copy if the refresh failed */
			l->state = YT_FETCH_FAILED;
			l->generation++;
		}
	}
	pthread_mutex_unlock(&f.mutex);

	strlist_free(urls);
	bfree(path);
	bfree(key);
	bfree(url);
	return true;
}

static struct fetch_entry *find_entry(const char *key)
{
	for (size_t i = 0; i < f.entries.num; i++)
		if (strcmp(f.entries.array[i].key, key) == 0)
			return &f.entries.array[i];
	return NULL;
}


/* Look up title/duration of one cached video that has none (lowest priority). */
static void work_one_meta(void)
{
	char *key = NULL, *url = NULL;

	pthread_mutex_lock(&f.mutex);
	if (f.meta_wanted.num) {
		char *item = f.meta_wanted.array[0];
		da_erase(f.meta_wanted, 0);
		char *tab = strchr(item, '\t');
		*tab = 0;
		key = bstrdup(item);
		url = bstrdup(tab + 1);
		char *tried = bstrdup(item);
		da_push_back(f.meta_tried, &tried);
		bfree(item);
	}
	pthread_mutex_unlock(&f.mutex);

	if (!key)
		return;

	pthread_mutex_lock(&f.mutex);
	bool known = find_meta(key) != NULL; /* a download may have filled it */
	pthread_mutex_unlock(&f.mutex);

	if (!known && ensure_ytdlp()) {
		const char *argv[] = {f.ytdlp_path, "-q", "--no-warnings", "--no-playlist", "--skip-download",
				      "--print", "%(duration)s\t%(title)s", "--", url, NULL};
		struct dstr out = {0};
		if (yt_proc_run(argv, &out) == 0 && out.len) {
			dstr_depad(&out);
			char *nl = strchr(out.array, '\n');
			if (nl)
				*nl = 0;
			meta_from_fields(key, out.array);
		}
		dstr_free(&out);
	}
	bfree(key);
	bfree(url);

	/* more may be waiting */
	pthread_mutex_lock(&f.mutex);
	if (f.meta_wanted.num)
		os_sem_post(f.sem);
	pthread_mutex_unlock(&f.mutex);
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

		/* playlist expansion unblocks playback: always before downloads */
		if (work_one_list()) {
			os_sem_post(f.sem); /* a download may still be waiting */
			continue;
		}

		pthread_mutex_lock(&f.mutex);
		for (size_t i = 0; i < f.entries.num; i++) {
			struct fetch_entry *e = &f.entries.array[i];
			if (e->state == YT_FETCH_PENDING && !e->running) {
				e->running = true;
				e->progress = 0;
				key = bstrdup(e->key);
				url = bstrdup(e->url);
				max_height = e->max_height;
				break;
			}
		}
		pthread_mutex_unlock(&f.mutex);

		if (!key) {
			work_one_meta();
			continue;
		}

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
	da_init(f.lists);

	f.cache_limit = DEFAULT_CACHE_LIMIT;
	f.cache_dir = obs_module_config_path("cache");
	f.meta_path = obs_module_config_path("cache/meta.tsv");
	da_init(f.meta);
	da_init(f.meta_wanted);
	da_init(f.meta_tried);
	load_meta();
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
	for (size_t i = 0; i < f.lists.num; i++) {
		bfree(f.lists.array[i].key);
		bfree(f.lists.array[i].url);
		strlist_free(f.lists.array[i].urls);
	}
	da_free(f.lists);
	for (size_t i = 0; i < f.meta.num; i++) {
		bfree(f.meta.array[i].key);
		bfree(f.meta.array[i].title);
	}
	da_free(f.meta);
	for (size_t i = 0; i < f.meta_wanted.num; i++)
		bfree(f.meta_wanted.array[i]);
	da_free(f.meta_wanted);
	for (size_t i = 0; i < f.meta_tried.num; i++)
		bfree(f.meta_tried.array[i]);
	da_free(f.meta_tried);
	bfree(f.meta_path);
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
			want_meta_locked(key.array, url);
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

bool yt_fetcher_is_collection(const char *url)
{
	if (!yt_fetcher_is_remote(url))
		return false;
	if (!strstr(url, "youtube.com"))
		return false;
	if (strstr(url, "list=") && !strstr(url, "v="))
		return true;
	return strstr(url, "/@") || strstr(url, "/channel/") || strstr(url, "/c/") || strstr(url, "/user/");
}

static struct list_entry *get_list_locked(const char *url)
{
	struct dstr key = {0};
	dstr_printf(&key, "list-%016llx", (unsigned long long)fnv1a(url));

	struct list_entry *l = find_list(key.array);
	if (!l) {
		l = da_push_back_new(f.lists);
		l->key = bstrdup(key.array);
		l->url = bstrdup(url);
		l->state = YT_FETCH_PENDING;

		/* serve the copy saved by a previous session right away, and
		 * refresh it in the background */
		char *path = list_file_for(key.array);
		char *text = os_quick_read_utf8_file(path);
		if (text && *text) {
			l->urls = strlist_split(text, '\n', false);
			l->state = YT_FETCH_READY;
			l->generation = 1;
			l->refresh = true;
		}
		bfree(text);
		bfree(path);
		os_sem_post(f.sem);
	} else if (l->state == YT_FETCH_READY && !l->running && !l->refresh && l->fetched_ns &&
		   os_gettime_ns() - l->fetched_ns > LIST_REFRESH_NS) {
		l->refresh = true;
		os_sem_post(f.sem);
	}
	dstr_free(&key);
	return l;
}

uint32_t yt_fetcher_list_generation(const char *url)
{
	pthread_mutex_lock(&f.mutex);
	uint32_t gen = get_list_locked(url)->generation;
	pthread_mutex_unlock(&f.mutex);
	return gen;
}

enum yt_fetch_state yt_fetcher_request_list(const char *url, char ***urls_out, uint32_t *generation)
{
	pthread_mutex_lock(&f.mutex);
	struct list_entry *l = get_list_locked(url);
	enum yt_fetch_state state = l->state;
	if (generation)
		*generation = l->generation;
	if (urls_out) {
		*urls_out = NULL;
		if (state == YT_FETCH_READY) {
			/* strlist is a single allocation: rebuild it via split */
			struct dstr joined = {0};
			for (char **u = l->urls; u && *u; u++) {
				dstr_cat(&joined, *u);
				dstr_cat(&joined, "\n");
			}
			*urls_out = strlist_split(joined.array ? joined.array : "", '\n', false);
			dstr_free(&joined);
		}
	}
	pthread_mutex_unlock(&f.mutex);
	return state;
}

void yt_fetcher_refresh_list(const char *url)
{
	pthread_mutex_lock(&f.mutex);
	struct list_entry *l = get_list_locked(url);
	if (!l->running) {
		l->refresh = true;
		os_sem_post(f.sem);
	}
	pthread_mutex_unlock(&f.mutex);
}

bool yt_fetcher_peek(const char *url, struct yt_item_info *info)
{
	struct dstr key = {0};
	memset(info, 0, sizeof(*info));
	make_key(url, &key);

	pthread_mutex_lock(&f.mutex);
	struct fetch_entry *e = find_entry(key.array);
	if (e) {
		info->known = true;
		info->state = e->state;
		info->running = e->running;
		info->progress = e->progress;
	} else {
		/* not requested this session, but maybe cached already */
		char *cached = cache_file_for(key.array);
		if (os_file_exists(cached)) {
			info->known = true;
			info->state = YT_FETCH_READY;
		}
		bfree(cached);
	}
	struct meta_entry *m = find_meta(key.array);
	if (m) {
		snprintf(info->title, sizeof(info->title), "%s", m->title);
		info->duration = m->duration;
	} else if (!(e && e->running)) {
		/* the download prints the title itself; otherwise look it up */
		want_meta_locked(key.array, url);
	}
	pthread_mutex_unlock(&f.mutex);

	dstr_free(&key);
	return info->known;
}
