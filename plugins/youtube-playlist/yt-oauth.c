#include "yt-oauth.h"
#include "yt-proc.h"

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/bmem.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#define OAUTH_LOG(level, format, ...) blog(level, "[youtube-playlist oauth] " format, ##__VA_ARGS__)

#define TOKEN_URL "https://oauth2.googleapis.com/token"
#define API_BASE "https://www.googleapis.com/youtube/v3/"
#define CURL_BIN "/usr/bin/curl"

static struct {
	pthread_mutex_t mutex;
	char *path; /* oauth.json */
	char *dir;  /* module config dir, for temp files */
	char *client_id;
	char *client_secret;
	char *refresh_token;
	char *channel;
	char *access_token;
	uint64_t access_expiry_ns;
} o;

/* ------------------------------------------------------------------------- */
/* storage                                                                   */

static void set_str(char **dst, const char *src)
{
	bfree(*dst);
	*dst = (src && *src) ? bstrdup(src) : NULL;
}

static void save_locked(void)
{
	obs_data_t *d = obs_data_create();
	if (o.client_id)
		obs_data_set_string(d, "client_id", o.client_id);
	if (o.client_secret)
		obs_data_set_string(d, "client_secret", o.client_secret);
	if (o.refresh_token)
		obs_data_set_string(d, "refresh_token", o.refresh_token);
	if (o.channel)
		obs_data_set_string(d, "channel", o.channel);

	os_mkdirs(o.dir);
	if (!obs_data_save_json_safe(d, o.path, "tmp", "bak"))
		OAUTH_LOG(LOG_WARNING, "Could not save %s", o.path);
#ifndef _WIN32
	chmod(o.path, 0600);
#endif
	obs_data_release(d);
}

static void load(void)
{
	obs_data_t *d = obs_data_create_from_json_file(o.path);
	if (!d)
		return;
	set_str(&o.client_id, obs_data_get_string(d, "client_id"));
	set_str(&o.client_secret, obs_data_get_string(d, "client_secret"));
	set_str(&o.refresh_token, obs_data_get_string(d, "refresh_token"));
	set_str(&o.channel, obs_data_get_string(d, "channel"));
	obs_data_release(d);
}

/* ------------------------------------------------------------------------- */
/* HTTP via curl                                                             */

static void urlencode_cat(struct dstr *out, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
		if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
			dstr_cat_ch(out, (char)*p);
		} else {
			dstr_cat_ch(out, '%');
			dstr_cat_ch(out, hex[*p >> 4]);
			dstr_cat_ch(out, hex[*p & 15]);
		}
	}
}

static void form_add(struct dstr *body, const char *key, const char *value)
{
	if (body->len)
		dstr_cat_ch(body, '&');
	dstr_cat(body, key);
	dstr_cat_ch(body, '=');
	urlencode_cat(body, value);
}

/* create a 0600 temp file holding `content`; returns its path (bfree) */
static char *secret_file(const char *content)
{
#ifdef _WIN32
	UNUSED_PARAMETER(content);
	return NULL;
#else
	char *uuid = os_generate_uuid();
	struct dstr path = {0};
	dstr_printf(&path, "%s/.tmp-%s", o.dir, uuid);
	bfree(uuid);

	os_mkdirs(o.dir);
	int fd = open(path.array, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		dstr_free(&path);
		return NULL;
	}
	size_t len = strlen(content);
	bool ok = write(fd, content, len) == (ssize_t)len;
	close(fd);
	if (!ok) {
		os_unlink(path.array);
		dstr_free(&path);
		return NULL;
	}
	return path.array;
#endif
}

/* Perform a request.  `bearer` adds an Authorization header, `form` makes it
 * a POST with an urlencoded body.  Returns the HTTP status (0 on failure). */
static int http(const char *url, const char *bearer, const char *form, struct dstr *body)
{
	char *hdr_file = NULL, *body_file = NULL;
	struct dstr hdr = {0}, out = {0}, hdr_arg = {0}, body_arg = {0};
	int status = 0;

	dstr_free(body);

	if (bearer) {
		dstr_printf(&hdr, "Authorization: Bearer %s\n", bearer);
		hdr_file = secret_file(hdr.array);
		dstr_free(&hdr);
		if (!hdr_file)
			goto done;
		dstr_printf(&hdr_arg, "@%s", hdr_file);
	}
	if (form) {
		body_file = secret_file(form);
		if (!body_file)
			goto done;
		dstr_printf(&body_arg, "@%s", body_file);
	}

	const char *argv[24];
	int n = 0;
	argv[n++] = CURL_BIN;
	argv[n++] = "-sS";
	argv[n++] = "--max-time";
	argv[n++] = "30";
	argv[n++] = "-w";
	argv[n++] = "\n%{http_code}";
	if (hdr_file) {
		argv[n++] = "-H";
		argv[n++] = hdr_arg.array;
	}
	if (body_file) {
		argv[n++] = "-X";
		argv[n++] = "POST";
		argv[n++] = "-H";
		argv[n++] = "Content-Type: application/x-www-form-urlencoded";
		argv[n++] = "--data-binary";
		argv[n++] = body_arg.array;
	}
	argv[n++] = "--";
	argv[n++] = url;
	argv[n] = NULL;

	int code = yt_proc_run(argv, &out);
	if (code != 0) {
		OAUTH_LOG(LOG_WARNING, "curl failed (%d): %s", code, out.array);
		goto done;
	}

	/* last line is the status code */
	char *nl = out.array ? strrchr(out.array, '\n') : NULL;
	if (nl) {
		status = atoi(nl + 1);
		*nl = 0;
		dstr_copy(body, out.array);
	}

done:
	if (hdr_file) {
		os_unlink(hdr_file);
		bfree(hdr_file);
	}
	if (body_file) {
		os_unlink(body_file);
		bfree(body_file);
	}
	dstr_free(&out);
	dstr_free(&hdr_arg);
	dstr_free(&body_arg);
	return status;
}

static void api_error(obs_data_t *json, struct dstr *error)
{
	if (!error)
		return;
	obs_data_t *err = json ? obs_data_get_obj(json, "error") : NULL;
	const char *msg = err ? obs_data_get_string(err, "message") : NULL;
	const char *desc = json ? obs_data_get_string(json, "error_description") : NULL;
	const char *code = json ? obs_data_get_string(json, "error") : NULL;
	if (msg && *msg)
		dstr_copy(error, msg);
	else if (desc && *desc)
		dstr_copy(error, desc);
	else if (code && *code)
		dstr_copy(error, code);
	else
		dstr_copy(error, "request failed");
	obs_data_release(err);
}

/* ------------------------------------------------------------------------- */
/* tokens                                                                    */

/* o.mutex held */
static bool token_request_locked(const char *form, struct dstr *error)
{
	struct dstr body = {0};
	int status = http(TOKEN_URL, NULL, form, &body);
	obs_data_t *json = body.array ? obs_data_create_from_json(body.array) : NULL;
	bool ok = false;

	if (status == 200 && json && *obs_data_get_string(json, "access_token")) {
		set_str(&o.access_token, obs_data_get_string(json, "access_token"));
		long long expires = obs_data_get_int(json, "expires_in");
		o.access_expiry_ns = os_gettime_ns() + (uint64_t)(expires > 0 ? expires : 3600) * 1000000000ULL;
		const char *refresh = obs_data_get_string(json, "refresh_token");
		if (refresh && *refresh)
			set_str(&o.refresh_token, refresh);
		ok = true;
	} else {
		api_error(json, error);
		OAUTH_LOG(LOG_WARNING, "Token request failed (HTTP %d): %s", status, error ? error->array : "");
	}

	obs_data_release(json);
	dstr_free(&body);
	return ok;
}

/* o.mutex held; refresh the access token if needed */
static bool ensure_token_locked(bool force, struct dstr *error)
{
	if (!o.client_id || !o.client_secret || !o.refresh_token) {
		if (error)
			dstr_copy(error, "not logged in");
		return false;
	}
	if (!force && o.access_token && os_gettime_ns() + 60ULL * 1000000000ULL < o.access_expiry_ns)
		return true;

	struct dstr form = {0};
	form_add(&form, "client_id", o.client_id);
	form_add(&form, "client_secret", o.client_secret);
	form_add(&form, "refresh_token", o.refresh_token);
	form_add(&form, "grant_type", "refresh_token");
	bool ok = token_request_locked(form.array, error);
	dstr_free(&form);

	if (!ok && error && strstr(error->array ? error->array : "", "invalid_grant")) {
		/* revoked or expired: forget it so the UI asks to log in again */
		set_str(&o.refresh_token, NULL);
		save_locked();
	}
	return ok;
}

/* GET an API endpoint (path relative to API_BASE).  Returns parsed JSON. */
static obs_data_t *api_get(const char *path_and_query, struct dstr *error)
{
	struct dstr url = {0}, body = {0}, token = {0};
	obs_data_t *json = NULL;
	dstr_printf(&url, "%s%s", API_BASE, path_and_query);

	for (int attempt = 0; attempt < 2; attempt++) {
		pthread_mutex_lock(&o.mutex);
		bool ok = ensure_token_locked(attempt > 0, error);
		if (ok)
			dstr_copy(&token, o.access_token);
		pthread_mutex_unlock(&o.mutex);
		if (!ok)
			break;

		int status = http(url.array, token.array, NULL, &body);
		obs_data_release(json);
		json = body.array ? obs_data_create_from_json(body.array) : NULL;
		if (status == 200 && json)
			break;
		if (status == 401 && attempt == 0)
			continue; /* access token expired early: refresh and retry */

		api_error(json, error);
		OAUTH_LOG(LOG_WARNING, "API %s failed (HTTP %d): %s", path_and_query, status,
			  error ? error->array : "");
		obs_data_release(json);
		json = NULL;
		break;
	}

	dstr_free(&url);
	dstr_free(&body);
	dstr_free(&token);
	return json;
}

/* ------------------------------------------------------------------------- */
/* API helpers                                                               */

static int parse_iso8601_duration(const char *s)
{
	/* PT#H#M#S (also P#DT...) */
	int total = 0, num = 0;
	bool time = false;
	for (const char *p = s; p && *p; p++) {
		if (isdigit((unsigned char)*p)) {
			num = num * 10 + (*p - '0');
			continue;
		}
		switch (*p) {
		case 'T':
			time = true;
			break;
		case 'D':
			total += num * 86400;
			break;
		case 'H':
			total += num * 3600;
			break;
		case 'M':
			if (time)
				total += num * 60;
			break;
		case 'S':
			total += num;
			break;
		}
		num = 0;
	}
	return total;
}

struct video_item {
	char *id;
	char *title;
	int duration;
};

char *yt_playlist_id_from_url(const char *url)
{
	const char *p = url ? strstr(url, "list=") : NULL;
	if (!p)
		return NULL;
	p += 5;
	size_t n = 0;
	while (isalnum((unsigned char)p[n]) || p[n] == '-' || p[n] == '_')
		n++;
	return n ? bstrdup_n(p, n) : NULL;
}

bool yt_api_expand_playlist(const char *playlist_id, struct dstr *lines)
{
	DARRAY(struct video_item) videos;
	struct dstr q = {0}, err = {0}, page = {0};
	bool ok = true;
	da_init(videos);
	dstr_free(lines);

	/* 1) playlist items (50 per page) */
	for (;;) {
		dstr_copy(&q, "playlistItems?part=snippet,contentDetails,status&maxResults=50&playlistId=");
		urlencode_cat(&q, playlist_id);
		if (page.len) {
			dstr_cat(&q, "&pageToken=");
			urlencode_cat(&q, page.array);
		}
		obs_data_t *json = api_get(q.array, &err);
		if (!json) {
			ok = false;
			break;
		}
		obs_data_array_t *items = obs_data_get_array(json, "items");
		for (size_t i = 0, n = obs_data_array_count(items); i < n; i++) {
			obs_data_t *it = obs_data_array_item(items, i);
			obs_data_t *snippet = obs_data_get_obj(it, "snippet");
			obs_data_t *details = obs_data_get_obj(it, "contentDetails");
			obs_data_t *status = obs_data_get_obj(it, "status");
			const char *privacy = status ? obs_data_get_string(status, "privacyStatus") : "";
			const char *vid = details ? obs_data_get_string(details, "videoId") : "";
			/* private videos of others and deleted ones cannot be played */
			if (vid && *vid && (strcmp(privacy, "public") == 0 || strcmp(privacy, "unlisted") == 0)) {
				struct video_item *v = da_push_back_new(videos);
				v->id = bstrdup(vid);
				v->title = bstrdup(snippet ? obs_data_get_string(snippet, "title") : "");
			}
			obs_data_release(snippet);
			obs_data_release(details);
			obs_data_release(status);
			obs_data_release(it);
		}
		obs_data_array_release(items);
		dstr_copy(&page, obs_data_get_string(json, "nextPageToken"));
		obs_data_release(json);
		if (!page.len)
			break;
	}

	/* 2) durations, 50 ids per request (best effort) */
	for (size_t start = 0; ok && start < videos.num; start += 50) {
		dstr_copy(&q, "videos?part=contentDetails&maxResults=50&id=");
		for (size_t i = start; i < videos.num && i < start + 50; i++) {
			if (i > start)
				dstr_cat(&q, ",");
			dstr_cat(&q, videos.array[i].id);
		}
		obs_data_t *json = api_get(q.array, &err);
		if (!json)
			break;
		obs_data_array_t *items = obs_data_get_array(json, "items");
		for (size_t i = 0, n = obs_data_array_count(items); i < n; i++) {
			obs_data_t *it = obs_data_array_item(items, i);
			obs_data_t *details = obs_data_get_obj(it, "contentDetails");
			const char *id = obs_data_get_string(it, "id");
			int dur = parse_iso8601_duration(details ? obs_data_get_string(details, "duration") : "");
			for (size_t k = start; k < videos.num && k < start + 50; k++)
				if (strcmp(videos.array[k].id, id) == 0)
					videos.array[k].duration = dur;
			obs_data_release(details);
			obs_data_release(it);
		}
		obs_data_array_release(items);
		obs_data_release(json);
	}

	if (ok) {
		for (size_t i = 0; i < videos.num; i++) {
			struct video_item *v = &videos.array[i];
			/* titles must stay on one line */
			for (char *c = v->title; *c; c++)
				if (*c == '\t' || *c == '\n' || *c == '\r')
					*c = ' ';
			dstr_catf(lines, "https://www.youtube.com/watch?v=%s\t%d\t%s\n", v->id, v->duration, v->title);
		}
		if (!lines->array)
			dstr_copy(lines, "");
	}

	for (size_t i = 0; i < videos.num; i++) {
		bfree(videos.array[i].id);
		bfree(videos.array[i].title);
	}
	da_free(videos);
	dstr_free(&q);
	dstr_free(&err);
	dstr_free(&page);
	return ok;
}

bool yt_oauth_logged_in(void)
{
	pthread_mutex_lock(&o.mutex);
	bool in = o.client_id && o.client_secret && o.refresh_token;
	pthread_mutex_unlock(&o.mutex);
	return in;
}

static void fetch_channel_title(void)
{
	struct dstr err = {0};
	obs_data_t *json = api_get("channels?part=snippet&mine=true", &err);
	if (json) {
		obs_data_array_t *items = obs_data_get_array(json, "items");
		obs_data_t *ch = obs_data_array_item(items, 0);
		obs_data_t *snippet = ch ? obs_data_get_obj(ch, "snippet") : NULL;
		pthread_mutex_lock(&o.mutex);
		set_str(&o.channel, snippet ? obs_data_get_string(snippet, "title") : NULL);
		save_locked();
		pthread_mutex_unlock(&o.mutex);
		obs_data_release(snippet);
		obs_data_release(ch);
		obs_data_array_release(items);
		obs_data_release(json);
	}
	dstr_free(&err);
}

/* ------------------------------------------------------------------------- */
/* global procedures                                                         */

static void proc_status(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	obs_data_t *d = obs_data_create();
	pthread_mutex_lock(&o.mutex);
	obs_data_set_bool(d, "configured", o.client_id && o.client_secret);
	obs_data_set_bool(d, "logged_in", o.client_id && o.client_secret && o.refresh_token);
	obs_data_set_string(d, "client_id", o.client_id ? o.client_id : "");
	obs_data_set_string(d, "channel", o.channel ? o.channel : "");
	pthread_mutex_unlock(&o.mutex);
	calldata_set_string(cd, "json", obs_data_get_json(d));
	obs_data_release(d);
}

static void proc_set_client(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	const char *id = calldata_string(cd, "client_id");
	const char *secret = calldata_string(cd, "client_secret");
	pthread_mutex_lock(&o.mutex);
	bool changed = !o.client_id || !id || strcmp(o.client_id, id) != 0;
	set_str(&o.client_id, id);
	set_str(&o.client_secret, secret);
	if (changed) {
		/* tokens belong to the old client */
		set_str(&o.refresh_token, NULL);
		set_str(&o.access_token, NULL);
		set_str(&o.channel, NULL);
	}
	save_locked();
	pthread_mutex_unlock(&o.mutex);
	OAUTH_LOG(LOG_INFO, "OAuth client configured");
}

static void proc_exchange(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	struct dstr form = {0}, err = {0};
	bool ok = false;

	pthread_mutex_lock(&o.mutex);
	if (o.client_id && o.client_secret) {
		form_add(&form, "client_id", o.client_id);
		form_add(&form, "client_secret", o.client_secret);
		form_add(&form, "code", calldata_string(cd, "code"));
		form_add(&form, "code_verifier", calldata_string(cd, "verifier"));
		form_add(&form, "redirect_uri", calldata_string(cd, "redirect_uri"));
		form_add(&form, "grant_type", "authorization_code");
		ok = token_request_locked(form.array, &err);
		if (ok && !o.refresh_token) {
			ok = false;
			dstr_copy(&err, "no refresh token returned");
		}
		if (ok)
			save_locked();
	} else {
		dstr_copy(&err, "OAuth client is not configured");
	}
	pthread_mutex_unlock(&o.mutex);

	if (ok) {
		fetch_channel_title();
		OAUTH_LOG(LOG_INFO, "Logged in to YouTube%s%s", o.channel ? " as " : "", o.channel ? o.channel : "");
	}

	calldata_set_bool(cd, "ok", ok);
	calldata_set_string(cd, "error", err.array ? err.array : "");
	dstr_free(&form);
	dstr_free(&err);
}

static void proc_logout(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	char *token = NULL;

	pthread_mutex_lock(&o.mutex);
	if (o.refresh_token)
		token = bstrdup(o.refresh_token);
	set_str(&o.refresh_token, NULL);
	set_str(&o.access_token, NULL);
	set_str(&o.channel, NULL);
	save_locked();
	pthread_mutex_unlock(&o.mutex);

	if (token) {
		/* revoke on Google's side too (best effort) */
		struct dstr form = {0}, body = {0};
		form_add(&form, "token", token);
		http("https://oauth2.googleapis.com/revoke", NULL, form.array, &body);
		dstr_free(&form);
		dstr_free(&body);
		bfree(token);
	}
	OAUTH_LOG(LOG_INFO, "Logged out");
}

static void add_playlist(obs_data_array_t *arr, const char *id, const char *title, long long count,
			 const char *privacy)
{
	obs_data_t *p = obs_data_create();
	obs_data_set_string(p, "id", id);
	obs_data_set_string(p, "title", title);
	obs_data_set_int(p, "count", count);
	obs_data_set_string(p, "privacy", privacy);
	obs_data_array_push_back(arr, p);
	obs_data_release(p);
}

static void proc_my_playlists(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	struct dstr err = {0}, q = {0}, page = {0};
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	bool ok = true;

	/* the "liked videos" playlist of the channel */
	obs_data_t *ch_json = api_get("channels?part=contentDetails&mine=true", &err);
	if (ch_json) {
		obs_data_array_t *items = obs_data_get_array(ch_json, "items");
		obs_data_t *ch = obs_data_array_item(items, 0);
		obs_data_t *details = ch ? obs_data_get_obj(ch, "contentDetails") : NULL;
		obs_data_t *related = details ? obs_data_get_obj(details, "relatedPlaylists") : NULL;
		const char *likes = related ? obs_data_get_string(related, "likes") : "";
		if (likes && *likes)
			add_playlist(arr, likes, obs_module_text("LikedVideos"), -1, "private");
		obs_data_release(related);
		obs_data_release(details);
		obs_data_release(ch);
		obs_data_array_release(items);
		obs_data_release(ch_json);
	} else {
		ok = false;
	}

	while (ok) {
		dstr_copy(&q, "playlists?part=snippet,contentDetails,status&mine=true&maxResults=50");
		if (page.len) {
			dstr_cat(&q, "&pageToken=");
			urlencode_cat(&q, page.array);
		}
		obs_data_t *json = api_get(q.array, &err);
		if (!json) {
			ok = false;
			break;
		}
		obs_data_array_t *items = obs_data_get_array(json, "items");
		for (size_t i = 0, n = obs_data_array_count(items); i < n; i++) {
			obs_data_t *it = obs_data_array_item(items, i);
			obs_data_t *snippet = obs_data_get_obj(it, "snippet");
			obs_data_t *details = obs_data_get_obj(it, "contentDetails");
			obs_data_t *status = obs_data_get_obj(it, "status");
			add_playlist(arr, obs_data_get_string(it, "id"),
				     snippet ? obs_data_get_string(snippet, "title") : "",
				     details ? obs_data_get_int(details, "itemCount") : 0,
				     status ? obs_data_get_string(status, "privacyStatus") : "");
			obs_data_release(snippet);
			obs_data_release(details);
			obs_data_release(status);
			obs_data_release(it);
		}
		obs_data_array_release(items);
		dstr_copy(&page, obs_data_get_string(json, "nextPageToken"));
		obs_data_release(json);
		if (!page.len)
			break;
	}

	obs_data_set_array(root, "playlists", arr);
	calldata_set_bool(cd, "ok", ok);
	calldata_set_string(cd, "json", obs_data_get_json(root));
	calldata_set_string(cd, "error", err.array ? err.array : "");

	obs_data_array_release(arr);
	obs_data_release(root);
	dstr_free(&err);
	dstr_free(&q);
	dstr_free(&page);
}

/* ------------------------------------------------------------------------- */

void yt_oauth_init(void)
{
	memset(&o, 0, sizeof(o));
	pthread_mutex_init(&o.mutex, NULL);
	o.dir = obs_module_config_path("");
	o.path = obs_module_config_path("oauth.json");
	load();

	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void youtube_playlist_oauth_status(out string json)", proc_status, NULL);
	proc_handler_add(ph, "void youtube_playlist_oauth_set_client(in string client_id, in string client_secret)",
			 proc_set_client, NULL);
	proc_handler_add(ph,
			 "void youtube_playlist_oauth_exchange(in string code, in string verifier, "
			 "in string redirect_uri, out bool ok, out string error)",
			 proc_exchange, NULL);
	proc_handler_add(ph, "void youtube_playlist_oauth_logout()", proc_logout, NULL);
	proc_handler_add(ph, "void youtube_playlist_my_playlists(out bool ok, out string json, out string error)",
			 proc_my_playlists, NULL);
}

void yt_oauth_free(void)
{
	bfree(o.dir);
	bfree(o.path);
	bfree(o.client_id);
	bfree(o.client_secret);
	bfree(o.refresh_token);
	bfree(o.channel);
	bfree(o.access_token);
	pthread_mutex_destroy(&o.mutex);
}
