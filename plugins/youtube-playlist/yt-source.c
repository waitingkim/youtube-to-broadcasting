/*
 * youtube_playlist_source
 *
 * Plays a list of media files back-to-back without gaps.
 *
 * Two media_playback instances ("slots") are used: while the current one is
 * playing, the next playlist item is already opened (demuxer/decoder ready)
 * in the other slot.  When the current item ends, the preloaded one starts
 * immediately and the finished instance is destroyed later from video_tick.
 *
 * Threading rules:
 *  - All playlist/slot state is protected by s->mutex.
 *  - media_playback callbacks run on the media threads.  They may take
 *    s->mutex but must never destroy a media_playback (that joins the
 *    calling thread).  Destruction always happens in yt_tick / destroy,
 *    outside of s->mutex.
 *  - Per-frame callbacks (video/audio) do not lock; they only check an
 *    atomic "active" flag on the player.
 */

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/darray.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <string.h>
#include <media-playback/media-playback.h>

#include "yt-fetcher.h"

#define S_PLAYLIST "playlist"
#define S_LOOP "loop"
#define S_HW_DECODE "hw_decode"
#define S_MAX_HEIGHT "max_height"
#define S_RESUME "resume"
#define S_CLEAR_ON_WAIT "clear_on_wait"
#define S_CACHE_LIMIT "cache_limit_gb"

/* saved by yt_save (not shown in properties) */
#define S_RESUME_ITEM "resume_item"
#define S_RESUME_INDEX "resume_index"
#define S_RESUME_MS "resume_ms"

#define T_(text) obs_module_text(text)

#define YT_LOG(level, format, ...) \
	blog(level, "[youtube-playlist: '%s'] " format, obs_source_get_name(s->source), ##__VA_ARGS__)

#define NO_INDEX ((size_t)-1)

struct yt_source;

struct yt_player {
	struct yt_source *s;
	media_playback_t *mp;
	size_t index;            /* playlist index this player was opened for */
	volatile bool active;    /* frames from this player are forwarded */
	volatile bool got_output; /* at least one frame/packet was produced */
	bool dead;               /* scheduled for destruction (s->mutex) */
};

struct yt_source {
	obs_source_t *source;
	pthread_mutex_t mutex;

	/* settings */
	DARRAY(char *) raw;   /* entries as entered by the user */
	DARRAY(char *) items; /* playable entries (playlists expanded) */
	DARRAY(uint32_t) list_gens; /* per raw entry: list generation used (0 = n/a) */
	DARRAY(size_t) raw_pos;     /* per raw entry: index in the settings array */
	DARRAY(size_t) item_raw;    /* per item: raw entry it came from */
	bool has_lists;
	bool lists_pending; /* some playlist is still being expanded */
	float list_check_t;
	bool loop;
	bool hw_decode;
	int max_height;
	bool resume;
	bool clear_on_wait;

	/* resume state loaded from settings, consumed by the first start */
	char *resume_item;
	size_t resume_index;
	int64_t resume_ms;

	/* seek to apply when the given item starts */
	size_t seek_index;
	int64_t seek_ms;

	/* playback */
	struct yt_player *slots[2];
	int cur; /* slot index of the current player, -1 if none */
	enum obs_media_state state;
	size_t cur_index;  /* playlist index of the current item */
	size_t next_index; /* playlist index to open next, NO_INDEX if none */
	size_t fail_streak;

	/* work for yt_tick */
	bool pending;
	bool need_start;   /* open next_index as the current player */
	bool emit_started; /* signal media_started */
	bool emit_ended;   /* signal media_ended + clear video */
	size_t waiting_for; /* index whose download we are waiting on (log once) */
	bool waiting;       /* nothing playing: waiting for a download */
	bool emit_clear;    /* clear the video while waiting */
};

enum item_res {
	ITEM_READY,
	ITEM_PENDING,
	ITEM_FAILED,
};

/* Map a playlist item to a playable local path.  Remote items are handed to
 * the fetcher (non-blocking); *path is set only when ITEM_READY. */
static enum item_res resolve_item_locked(struct yt_source *s, size_t index, char **path)
{
	const char *item = s->items.array[index];
	*path = NULL;

	if (!yt_fetcher_is_remote(item)) {
		*path = bstrdup(item);
		return ITEM_READY;
	}

	switch (yt_fetcher_request(item, s->max_height, path)) {
	case YT_FETCH_READY:
		return ITEM_READY;
	case YT_FETCH_FAILED:
		return ITEM_FAILED;
	default:
		return ITEM_PENDING;
	}
}

/* Queue downloads for an item without using the result. */
static void prefetch_locked(struct yt_source *s, size_t index)
{
	if (index == NO_INDEX || index >= s->items.num || !yt_fetcher_is_remote(s->items.array[index]))
		return;
	char *path = NULL;
	yt_fetcher_request(s->items.array[index], s->max_height, &path);
	bfree(path);
}

/* ------------------------------------------------------------------------- */
/* helpers (call with s->mutex held)                                         */

static size_t successor_locked(struct yt_source *s, size_t index)
{
	size_t n = s->items.num;
	if (!n || index == NO_INDEX)
		return NO_INDEX;
	if (index + 1 < n)
		return index + 1;
	return s->loop ? 0 : NO_INDEX;
}

static void kill_player_locked(struct yt_player *p)
{
	if (!p)
		return;
	os_atomic_set_bool(&p->active, false);
	p->dead = true;
	p->s->pending = true;
}

static void kill_all_locked(struct yt_source *s)
{
	kill_player_locked(s->slots[0]);
	kill_player_locked(s->slots[1]);
	s->cur = -1;
}

static void begin_player_locked(struct yt_source *s, int slot)
{
	struct yt_player *p = s->slots[slot];

	s->cur = slot;
	s->cur_index = p->index;
	os_atomic_set_bool(&p->active, true);

	media_playback_play(p->mp, false, false);
	/* The player may have been opened (and its clock started) a long time
	 * ago while waiting as the preloaded item.  Re-base its timestamps on
	 * the current time so the first frames are not stale. */
	media_playback_play_pause(p->mp, false);

	if (s->seek_ms > 0 && s->seek_index == p->index) {
		YT_LOG(LOG_INFO, "Resuming item %zu at %.1fs", p->index + 1, (double)s->seek_ms / 1000.0);
		media_playback_seek(p->mp, s->seek_ms);
	}
	s->seek_ms = 0;
	s->waiting = false;

	s->emit_started = true;
	s->pending = true;

	YT_LOG(LOG_INFO, "Playing item %zu/%zu: %s", p->index + 1, s->items.num, s->items.array[p->index]);
}

/* The current item finished (or was skipped): switch to the preloaded one if
 * it is ready, otherwise ask yt_tick to open the next item. */
static void advance_locked(struct yt_source *s)
{
	if (s->cur >= 0)
		kill_player_locked(s->slots[s->cur]);

	int other = s->cur >= 0 ? 1 - s->cur : -1;
	struct yt_player *next = other >= 0 ? s->slots[other] : NULL;

	if (next && !next->dead) {
		begin_player_locked(s, other);
		return;
	}

	s->cur = -1;
	if (s->next_index != NO_INDEX) {
		s->need_start = true;
	} else {
		s->state = OBS_MEDIA_STATE_ENDED;
		s->emit_ended = true;
		YT_LOG(LOG_INFO, "Playlist ended");
	}
	s->pending = true;
}

/* Restart playback at the given playlist index (actual work in yt_tick). */
static void start_at_locked(struct yt_source *s, size_t index)
{
	kill_all_locked(s);

	if (!s->items.num) {
		s->state = OBS_MEDIA_STATE_STOPPED;
		s->next_index = NO_INDEX;
		s->need_start = false;
		return;
	}

	if (index >= s->items.num)
		index = 0;

	s->state = OBS_MEDIA_STATE_PLAYING;
	s->next_index = index;
	s->need_start = true;
	s->fail_streak = 0;
	s->waiting_for = NO_INDEX;
	s->waiting = false;
	s->seek_ms = 0;
	s->pending = true;
}

/* First start after loading: continue where the previous session stopped if
 * the saved item is still in the playlist, otherwise start from the top. */
static void start_initial_locked(struct yt_source *s)
{
	size_t index = 0;
	int64_t ms = 0;

	if (s->resume && s->resume_item) {
		size_t found = NO_INDEX;
		if (s->resume_index < s->items.num && strcmp(s->items.array[s->resume_index], s->resume_item) == 0)
			found = s->resume_index;
		for (size_t i = 0; found == NO_INDEX && i < s->items.num; i++)
			if (strcmp(s->items.array[i], s->resume_item) == 0)
				found = i;
		if (found != NO_INDEX) {
			index = found;
			ms = s->resume_ms;
		}
	}
	/* Wait until every playlist is expanded, otherwise playback would start
	 * in the wrong place (items of a later playlist before an earlier one).
	 * apply_items_locked calls us again when they arrive. */
	if (!s->items.num || s->lists_pending)
		return;

	bfree(s->resume_item);
	s->resume_item = NULL;

	start_at_locked(s, index);
	if (index || ms) {
		s->seek_index = index;
		s->seek_ms = ms;
	}
}

/* ------------------------------------------------------------------------- */
/* media_playback callbacks (media threads)                                  */

static void yt_video_cb(void *opaque, struct obs_source_frame *frame)
{
	struct yt_player *p = opaque;
	if (!os_atomic_load_bool(&p->active))
		return;
	os_atomic_set_bool(&p->got_output, true);
	obs_source_output_video(p->s->source, frame);
}

static void yt_seek_cb(void *opaque, struct obs_source_frame *frame)
{
	struct yt_player *p = opaque;
	if (!os_atomic_load_bool(&p->active))
		return;
	obs_source_set_video_frame(p->s->source, frame);
}

static void yt_audio_cb(void *opaque, struct obs_source_audio *audio)
{
	struct yt_player *p = opaque;
	if (!os_atomic_load_bool(&p->active))
		return;
	os_atomic_set_bool(&p->got_output, true);
	obs_source_output_audio(p->s->source, audio);
}

/* Called when a player reaches the end of its media, or when it fails to
 * open/decode (before or after it started playing). */
static void yt_stop_cb(void *opaque)
{
	struct yt_player *p = opaque;
	struct yt_source *s = p->s;

	pthread_mutex_lock(&s->mutex);

	if (!p->dead) {
		bool is_current = s->cur >= 0 && s->slots[s->cur] == p;
		bool failed = !os_atomic_load_bool(&p->got_output);
		const char *path = p->index < s->items.num ? s->items.array[p->index] : "?";

		if (failed) {
			s->fail_streak++;
			YT_LOG(LOG_WARNING, "Failed to play item %zu, skipping: %s", p->index + 1, path);
		} else {
			s->fail_streak = 0;
		}

		if (is_current)
			advance_locked(s);
		else
			kill_player_locked(p); /* preload failed */
	}

	pthread_mutex_unlock(&s->mutex);
}

/* ------------------------------------------------------------------------- */
/* player creation / destruction                                             */

static struct yt_player *open_player_locked(struct yt_source *s, size_t index, const char *path)
{
	struct yt_player *p = bzalloc(sizeof(*p));
	p->s = s;
	p->index = index;

	struct mp_media_info info = {
		.opaque = p,
		.v_cb = yt_video_cb,
		.v_seek_cb = yt_seek_cb,
		.a_cb = yt_audio_cb,
		.stop_cb = yt_stop_cb,
		.path = path,
		.speed = 100,
		.force_range = VIDEO_RANGE_DEFAULT,
		.hardware_decoding = s->hw_decode,
		.is_local_file = true,
	};

	p->mp = media_playback_create(&info);
	if (!p->mp) {
		YT_LOG(LOG_WARNING, "Failed to open item %zu: %s", index + 1, info.path);
		bfree(p);
		return NULL;
	}
	return p;
}

static void free_player(struct yt_player *p)
{
	if (!p)
		return;
	media_playback_destroy(p->mp);
	bfree(p);
}

/* Open next_index into the free slot (as current, or as preload). */
static void fill_slots_locked(struct yt_source *s)
{
	if (s->state != OBS_MEDIA_STATE_PLAYING && s->state != OBS_MEDIA_STATE_PAUSED)
		return;

	if (s->items.num && s->fail_streak >= s->items.num) {
		YT_LOG(LOG_ERROR, "Every item in the playlist failed, stopping");
		kill_all_locked(s);
		s->state = OBS_MEDIA_STATE_ERROR;
		s->need_start = false;
		s->emit_ended = true;
		return;
	}

	if (s->need_start) {
		int slot = !s->slots[0] ? 0 : (!s->slots[1] ? 1 : -1);
		if (slot < 0 || s->next_index == NO_INDEX)
			return;

		size_t index = s->next_index;
		char *path = NULL;
		enum item_res res = resolve_item_locked(s, index, &path);

		if (res == ITEM_PENDING) {
			/* keep polling from yt_tick until the download finishes */
			if (s->waiting_for != index) {
				s->waiting_for = index;
				s->waiting = true;
				s->emit_clear = s->clear_on_wait;
				YT_LOG(LOG_INFO, "Waiting for download of item %zu: %s", index + 1,
				       s->items.array[index]);
			}
			prefetch_locked(s, successor_locked(s, index));
			s->pending = true;
			return;
		}
		s->waiting_for = NO_INDEX;
		s->next_index = successor_locked(s, index);

		struct yt_player *p = res == ITEM_READY ? open_player_locked(s, index, path) : NULL;
		bfree(path);
		if (!p) {
			if (res == ITEM_FAILED)
				YT_LOG(LOG_WARNING, "Item %zu could not be fetched, skipping: %s", index + 1,
				       s->items.array[index]);
			s->fail_streak++;
			s->pending = true; /* retry with the following item */
			if (s->next_index == NO_INDEX) {
				s->need_start = false;
				s->state = OBS_MEDIA_STATE_ENDED;
				s->emit_ended = true;
			}
			return;
		}

		s->need_start = false;
		s->slots[slot] = p;
		begin_player_locked(s, slot);
	}

	/* preload the following item */
	if (s->cur >= 0 && !s->slots[1 - s->cur] && s->next_index != NO_INDEX) {
		size_t index = s->next_index;
		char *path = NULL;
		enum item_res res = resolve_item_locked(s, index, &path);

		if (res == ITEM_PENDING) {
			/* poll again next tick; the current item keeps playing */
			s->pending = true;
		} else {
			s->next_index = successor_locked(s, index);
			struct yt_player *p = res == ITEM_READY ? open_player_locked(s, index, path) : NULL;
			if (p) {
				s->slots[1 - s->cur] = p;
			} else {
				if (res == ITEM_FAILED)
					YT_LOG(LOG_WARNING, "Item %zu could not be fetched, skipping: %s", index + 1,
					       s->items.array[index]);
				s->fail_streak++;
				s->pending = true;
			}
		}
		bfree(path);
	}

	/* keep the download queue one item ahead of the preload */
	if (s->cur >= 0)
		prefetch_locked(s, s->next_index);
}

/* ------------------------------------------------------------------------- */
/* obs_source_info callbacks                                                 */

static const char *yt_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("YouTubePlaylist");
}

static bool lists_changed_locked(struct yt_source *s);
static void apply_items_locked(struct yt_source *s, bool restart);

static void yt_tick(void *data, float seconds)
{
	struct yt_source *s = data;
	struct yt_player *trash[2] = {NULL, NULL};
	bool started, ended, clear;

	pthread_mutex_lock(&s->mutex);
	if (s->has_lists) {
		s->list_check_t += seconds;
		if (s->list_check_t >= 0.5f) {
			s->list_check_t = 0.0f;
			if (lists_changed_locked(s))
				apply_items_locked(s, false);
		}
	}
	if (!s->pending) {
		pthread_mutex_unlock(&s->mutex);
		return;
	}
	s->pending = false;

	for (int i = 0; i < 2; i++) {
		if (s->slots[i] && s->slots[i]->dead) {
			trash[i] = s->slots[i];
			s->slots[i] = NULL;
			if (s->cur == i)
				s->cur = -1;
		}
	}
	pthread_mutex_unlock(&s->mutex);

	/* joins the media threads, so it must happen without s->mutex */
	free_player(trash[0]);
	free_player(trash[1]);

	pthread_mutex_lock(&s->mutex);
	fill_slots_locked(s);
	started = s->emit_started;
	ended = s->emit_ended;
	clear = s->emit_clear;
	s->emit_started = false;
	s->emit_ended = false;
	s->emit_clear = false;
	pthread_mutex_unlock(&s->mutex);

	if (clear)
		obs_source_output_video(s->source, NULL);
	if (started)
		obs_source_media_started(s->source);
	if (ended) {
		obs_source_output_video(s->source, NULL);
		obs_source_media_ended(s->source);
	}
}

static void yt_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, S_LOOP, true);
	obs_data_set_default_bool(settings, S_HW_DECODE, true);
	obs_data_set_default_int(settings, S_MAX_HEIGHT, 1080);
	obs_data_set_default_bool(settings, S_RESUME, true);
	obs_data_set_default_bool(settings, S_CLEAR_ON_WAIT, true);
	obs_data_set_default_int(settings, S_CACHE_LIMIT, 10);
}

static bool refresh_lists_clicked(obs_properties_t *props, obs_property_t *prop, void *data);

static obs_properties_t *yt_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_editable_list(props, S_PLAYLIST, T_("Playlist"), OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS,
					 "Media Files (*.mp4 *.m4v *.mov *.mkv *.webm *.ts *.flv);;All Files (*.*)",
					 NULL);
	obs_property_t *pl = obs_properties_get(props, S_PLAYLIST);
	obs_property_set_long_description(pl, T_("Playlist.Description"));
	obs_properties_add_button2(props, "refresh_lists", T_("RefreshPlaylists"), refresh_lists_clicked, data);
	obs_properties_add_bool(props, S_LOOP, T_("Loop"));

	obs_property_t *h = obs_properties_add_list(props, S_MAX_HEIGHT, T_("MaxHeight"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	static const int heights[] = {360, 480, 720, 1080, 1440, 2160};
	for (size_t i = 0; i < sizeof(heights) / sizeof(heights[0]); i++) {
		char name[16];
		snprintf(name, sizeof(name), "%dp", heights[i]);
		obs_property_list_add_int(h, name, heights[i]);
	}

	obs_properties_add_bool(props, S_RESUME, T_("Resume"));
	obs_property_t *w = obs_properties_add_bool(props, S_CLEAR_ON_WAIT, T_("ClearOnWait"));
	obs_property_set_long_description(w, T_("ClearOnWait.Description"));
	obs_property_t *c = obs_properties_add_int(props, S_CACHE_LIMIT, T_("CacheLimit"), 1, 2000, 1);
	obs_property_int_set_suffix(c, " GB");
	obs_properties_add_bool(props, S_HW_DECODE, T_("HardwareDecode"));
	return props;
}

static void free_items(struct yt_source *s)
{
	for (size_t i = 0; i < s->items.num; i++)
		bfree(s->items.array[i]);
	da_free(s->items);
	da_free(s->item_raw);
}

static void free_raw(struct yt_source *s)
{
	for (size_t i = 0; i < s->raw.num; i++)
		bfree(s->raw.array[i]);
	da_free(s->raw);
	da_free(s->list_gens);
	da_free(s->raw_pos);
}

/* Rebuild s->items from s->raw, expanding playlists/channels that the
 * fetcher has already resolved (others are queued and contribute nothing
 * yet; yt_tick notices when they arrive). */
static void expand_items_locked(struct yt_source *s)
{
	free_items(s);
	da_resize(s->list_gens, s->raw.num);
	s->has_lists = false;
	s->lists_pending = false;

	for (size_t i = 0; i < s->raw.num; i++) {
		const char *entry = s->raw.array[i];
		s->list_gens.array[i] = 0;

		if (!yt_fetcher_is_collection(entry)) {
			char *copy = bstrdup(entry);
			da_push_back(s->items, &copy);
			da_push_back(s->item_raw, &i);
			continue;
		}

		s->has_lists = true;
		char **urls = NULL;
		uint32_t gen = 0;
		enum yt_fetch_state st = yt_fetcher_request_list(entry, &urls, &gen);
		s->list_gens.array[i] = gen;

		if (st == YT_FETCH_READY) {
			for (char **u = urls; u && *u; u++) {
				char *copy = bstrdup(*u);
				da_push_back(s->items, &copy);
				da_push_back(s->item_raw, &i);
			}
		} else if (st == YT_FETCH_FAILED) {
			YT_LOG(LOG_WARNING, "Playlist could not be loaded: %s", entry);
		} else {
			s->lists_pending = true;
		}
		strlist_free(urls);
	}
}

/* true if any playlist changed since expand_items_locked */
static bool lists_changed_locked(struct yt_source *s)
{
	for (size_t i = 0; i < s->raw.num; i++) {
		if (!yt_fetcher_is_collection(s->raw.array[i]))
			continue;
		if (yt_fetcher_list_generation(s->raw.array[i]) != s->list_gens.array[i])
			return true;
	}
	return false;
}

/* Re-expand the playlist and reconcile playback with the new item list. */
static void apply_items_locked(struct yt_source *s, bool restart)
{
	char *cur_path = (s->cur >= 0 && s->cur_index < s->items.num) ? bstrdup(s->items.array[s->cur_index])
								     : NULL;
	bool was_running = s->state == OBS_MEDIA_STATE_PLAYING || s->state == OBS_MEDIA_STATE_PAUSED;
	size_t old_count = s->items.num;

	expand_items_locked(s);
	if (s->items.num != old_count)
		YT_LOG(LOG_INFO, "Playlist now has %zu item(s)", s->items.num);

	if (was_running) {
		if (restart || !s->items.num) {
			start_at_locked(s, 0);
		} else {
			/* keep the current item playing; re-resolve its position and
			 * drop the stale preload */
			size_t pos = NO_INDEX;
			for (size_t i = 0; cur_path && i < s->items.num; i++) {
				if (strcmp(s->items.array[i], cur_path) == 0) {
					pos = i;
					break;
				}
			}
			if (pos != NO_INDEX && s->cur >= 0) {
				s->cur_index = pos;
				s->slots[s->cur]->index = pos;
			}
			if (s->cur >= 0)
				kill_player_locked(s->slots[1 - s->cur]);
			s->next_index = successor_locked(s, pos != NO_INDEX ? pos : s->cur_index);
			s->pending = true;
		}
	} else if (s->items.num && obs_source_active(s->source)) {
		start_initial_locked(s);
	}

	bfree(cur_path);
}

static void yt_update(void *data, obs_data_t *settings)
{
	struct yt_source *s = data;
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	size_t count = obs_data_array_count(array);

	pthread_mutex_lock(&s->mutex);

	free_raw(s);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		if (path && *path) {
			char *copy = bstrdup(path);
			da_push_back(s->raw, &copy);
			da_push_back(s->raw_pos, &i);
		}
		obs_data_release(item);
	}

	bool hw_changed = s->hw_decode != obs_data_get_bool(settings, S_HW_DECODE);
	s->loop = obs_data_get_bool(settings, S_LOOP);
	s->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);
	s->max_height = (int)obs_data_get_int(settings, S_MAX_HEIGHT);
	if (s->max_height <= 0)
		s->max_height = 1080;
	s->resume = obs_data_get_bool(settings, S_RESUME);
	s->clear_on_wait = obs_data_get_bool(settings, S_CLEAR_ON_WAIT);
	yt_fetcher_set_cache_limit(obs_data_get_int(settings, S_CACHE_LIMIT) * 1024LL * 1024LL * 1024LL);

	apply_items_locked(s, hw_changed);

	pthread_mutex_unlock(&s->mutex);
	obs_data_array_release(array);
}

static bool refresh_lists_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);
	struct yt_source *s = data;
	if (!s)
		return false;
	pthread_mutex_lock(&s->mutex);
	for (size_t i = 0; i < s->raw.num; i++)
		if (yt_fetcher_is_collection(s->raw.array[i]))
			yt_fetcher_refresh_list(s->raw.array[i]);
	pthread_mutex_unlock(&s->mutex);
	return false;
}

static void yt_activate(void *data)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->state != OBS_MEDIA_STATE_PLAYING && s->state != OBS_MEDIA_STATE_PAUSED)
		start_initial_locked(s);
	pthread_mutex_unlock(&s->mutex);
}

static void yt_save(void *data, obs_data_t *settings)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->cur >= 0 && s->cur_index < s->items.num) {
		obs_data_set_string(settings, S_RESUME_ITEM, s->items.array[s->cur_index]);
		obs_data_set_int(settings, S_RESUME_INDEX, (long long)s->cur_index);
		obs_data_set_int(settings, S_RESUME_MS, media_playback_get_current_time(s->slots[s->cur]->mp));
	} else if (s->state == OBS_MEDIA_STATE_ENDED || s->state == OBS_MEDIA_STATE_ERROR) {
		obs_data_erase(settings, S_RESUME_ITEM);
		obs_data_erase(settings, S_RESUME_INDEX);
		obs_data_erase(settings, S_RESUME_MS);
	}
	/* otherwise (not started yet / waiting) keep what was saved before */
	pthread_mutex_unlock(&s->mutex);
}


/* --- procedures for UIs (youtube-playlist-ui dock) ------------------------ */

static const char *state_name(enum obs_media_state st)
{
	switch (st) {
	case OBS_MEDIA_STATE_PLAYING:
		return "playing";
	case OBS_MEDIA_STATE_OPENING:
		return "opening";
	case OBS_MEDIA_STATE_BUFFERING:
		return "buffering";
	case OBS_MEDIA_STATE_PAUSED:
		return "paused";
	case OBS_MEDIA_STATE_STOPPED:
		return "stopped";
	case OBS_MEDIA_STATE_ENDED:
		return "ended";
	case OBS_MEDIA_STATE_ERROR:
		return "error";
	default:
		return "none";
	}
}

/* void get_queue(out string json)
 * {"state": "...", "current": <index or -1>,
 *  "items": [{"entry", "title", "duration", "status", "progress"}]} */
static void proc_get_queue(void *data, calldata_t *cd)
{
	struct yt_source *s = data;
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();

	pthread_mutex_lock(&s->mutex);
	enum obs_media_state st = s->state;
	if (st == OBS_MEDIA_STATE_PLAYING && s->waiting)
		st = OBS_MEDIA_STATE_BUFFERING;
	obs_data_set_string(root, "state", state_name(st));
	obs_data_set_int(root, "current", s->cur >= 0 ? (long long)s->cur_index : -1);
	obs_data_set_bool(root, "lists_pending", s->lists_pending);
	obs_data_set_bool(root, "loop", s->loop);

	for (size_t i = 0; i < s->items.num; i++) {
		const char *entry = s->items.array[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "entry", entry);
		size_t raw = i < s->item_raw.num ? s->item_raw.array[i] : 0;
		if (raw < s->raw.num) {
			/* index into settings["playlist"] of the entry this item
			 * comes from, and whether that entry is a playlist/channel */
			obs_data_set_int(item, "setting_index", (long long)s->raw_pos.array[raw]);
			obs_data_set_bool(item, "from_list", yt_fetcher_is_collection(s->raw.array[raw]));
			obs_data_set_string(item, "list", s->raw.array[raw]);
		}

		const char *status = "";
		if (!yt_fetcher_is_remote(entry)) {
			const char *slash = strrchr(entry, '/');
			obs_data_set_string(item, "title", slash ? slash + 1 : entry);
			status = "local";
		} else {
			struct yt_item_info info;
			yt_fetcher_peek(entry, &info);
			obs_data_set_string(item, "title", info.title[0] ? info.title : entry);
			obs_data_set_int(item, "duration", info.duration);
			if (info.known) {
				if (info.state == YT_FETCH_READY)
					status = "ready";
				else if (info.state == YT_FETCH_FAILED)
					status = "failed";
				else if (info.running)
					status = "downloading";
				else
					status = "queued";
				obs_data_set_int(item, "progress", info.progress);
			}
		}
		if (s->cur >= 0 && i == s->cur_index)
			status = "playing";
		else if (s->waiting && i == s->waiting_for && strcmp(status, "downloading") != 0)
			status = "waiting";
		obs_data_set_string(item, "status", status);

		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&s->mutex);

	obs_data_set_array(root, "items", arr);
	calldata_set_string(cd, "json", obs_data_get_json(root));
	obs_data_array_release(arr);
	obs_data_release(root);
}

/* void play_index(in int index) */
static void proc_play_index(void *data, calldata_t *cd)
{
	struct yt_source *s = data;
	long long index = calldata_int(cd, "index");
	pthread_mutex_lock(&s->mutex);
	if (index >= 0 && (size_t)index < s->items.num) {
		YT_LOG(LOG_INFO, "Jump to item %lld", index + 1);
		start_at_locked(s, (size_t)index);
	}
	pthread_mutex_unlock(&s->mutex);
}

/* void refresh_lists() */
static void proc_refresh_lists(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	refresh_lists_clicked(NULL, NULL, data);
}

static void *yt_create(obs_data_t *settings, obs_source_t *source)
{
	struct yt_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->cur = -1;
	s->state = OBS_MEDIA_STATE_NONE;
	s->next_index = NO_INDEX;
	s->waiting_for = NO_INDEX;
	pthread_mutex_init(&s->mutex, NULL);
	da_init(s->items);
	da_init(s->raw);
	da_init(s->list_gens);
	da_init(s->raw_pos);
	da_init(s->item_raw);

	const char *resume_item = obs_data_get_string(settings, S_RESUME_ITEM);
	if (resume_item && *resume_item) {
		s->resume_item = bstrdup(resume_item);
		s->resume_index = (size_t)obs_data_get_int(settings, S_RESUME_INDEX);
		s->resume_ms = obs_data_get_int(settings, S_RESUME_MS);
	}

	yt_update(s, settings);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void get_queue(out string json)", proc_get_queue, s);
	proc_handler_add(ph, "void play_index(in int index)", proc_play_index, s);
	proc_handler_add(ph, "void refresh_lists()", proc_refresh_lists, s);
	return s;
}

static void yt_destroy(void *data)
{
	struct yt_source *s = data;
	struct yt_player *a, *b;

	pthread_mutex_lock(&s->mutex);
	kill_all_locked(s);
	a = s->slots[0];
	b = s->slots[1];
	s->slots[0] = s->slots[1] = NULL;
	pthread_mutex_unlock(&s->mutex);

	free_player(a);
	free_player(b);

	free_items(s);
	free_raw(s);
	bfree(s->resume_item);
	pthread_mutex_destroy(&s->mutex);
	bfree(s);
}

/* --- media controls ------------------------------------------------------ */

static void yt_play_pause(void *data, bool pause)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->cur >= 0) {
		media_playback_play_pause(s->slots[s->cur]->mp, pause);
		s->state = pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
	} else if (!pause && s->state != OBS_MEDIA_STATE_PLAYING) {
		start_at_locked(s, 0);
	}
	pthread_mutex_unlock(&s->mutex);
}

static void yt_stop(void *data)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	kill_all_locked(s);
	s->need_start = false;
	s->state = OBS_MEDIA_STATE_STOPPED;
	pthread_mutex_unlock(&s->mutex);
	obs_source_output_video(s->source, NULL);
}

static void yt_restart(void *data)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	start_at_locked(s, 0);
	pthread_mutex_unlock(&s->mutex);
}

static void yt_next(void *data)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->cur >= 0 && s->state == OBS_MEDIA_STATE_PLAYING)
		advance_locked(s);
	pthread_mutex_unlock(&s->mutex);
}

static void yt_previous(void *data)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->items.num) {
		size_t index = s->cur_index;
		if (index == 0 || index >= s->items.num)
			index = s->loop ? s->items.num - 1 : 0;
		else
			index--;
		start_at_locked(s, index);
	}
	pthread_mutex_unlock(&s->mutex);
}

static int64_t yt_get_duration(void *data)
{
	struct yt_source *s = data;
	int64_t dur = 0;
	pthread_mutex_lock(&s->mutex);
	if (s->cur >= 0)
		dur = media_playback_get_duration(s->slots[s->cur]->mp) / INT64_C(1000);
	pthread_mutex_unlock(&s->mutex);
	return dur;
}

static int64_t yt_get_time(void *data)
{
	struct yt_source *s = data;
	int64_t t = 0;
	pthread_mutex_lock(&s->mutex);
	if (s->cur >= 0)
		t = media_playback_get_current_time(s->slots[s->cur]->mp);
	pthread_mutex_unlock(&s->mutex);
	return t;
}

static void yt_set_time(void *data, int64_t ms)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->cur >= 0)
		media_playback_seek(s->slots[s->cur]->mp, ms);
	pthread_mutex_unlock(&s->mutex);
}

static enum obs_media_state yt_get_state(void *data)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	enum obs_media_state state = s->state;
	if (state == OBS_MEDIA_STATE_PLAYING && s->waiting)
		state = OBS_MEDIA_STATE_BUFFERING;
	pthread_mutex_unlock(&s->mutex);
	return state;
}

struct obs_source_info yt_playlist_source_info = {
	.id = "youtube_playlist_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = yt_get_name,
	.create = yt_create,
	.destroy = yt_destroy,
	.get_defaults = yt_defaults,
	.get_properties = yt_properties,
	.update = yt_update,
	.save = yt_save,
	.activate = yt_activate,
	.video_tick = yt_tick,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_play_pause = yt_play_pause,
	.media_restart = yt_restart,
	.media_stop = yt_stop,
	.media_next = yt_next,
	.media_previous = yt_previous,
	.media_get_duration = yt_get_duration,
	.media_get_time = yt_get_time,
	.media_set_time = yt_set_time,
	.media_get_state = yt_get_state,
};
