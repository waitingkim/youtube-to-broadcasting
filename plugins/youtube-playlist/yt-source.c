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
#include <string.h>
#include <media-playback/media-playback.h>

#define S_PLAYLIST "playlist"
#define S_LOOP "loop"
#define S_HW_DECODE "hw_decode"

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
	DARRAY(char *) items;
	bool loop;
	bool hw_decode;

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
};

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
	s->pending = true;
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

static struct yt_player *open_player_locked(struct yt_source *s, size_t index)
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
		.path = s->items.array[index],
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
		s->next_index = successor_locked(s, index);

		struct yt_player *p = open_player_locked(s, index);
		if (!p) {
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
		s->next_index = successor_locked(s, index);

		struct yt_player *p = open_player_locked(s, index);
		if (p) {
			s->slots[1 - s->cur] = p;
		} else {
			s->fail_streak++;
			s->pending = true;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* obs_source_info callbacks                                                 */

static const char *yt_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return T_("YouTubePlaylist");
}

static void yt_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct yt_source *s = data;
	struct yt_player *trash[2] = {NULL, NULL};
	bool started, ended;

	pthread_mutex_lock(&s->mutex);
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
	s->emit_started = false;
	s->emit_ended = false;
	pthread_mutex_unlock(&s->mutex);

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
}

static obs_properties_t *yt_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_editable_list(props, S_PLAYLIST, T_("Playlist"), OBS_EDITABLE_LIST_TYPE_FILES,
					 "Media Files (*.mp4 *.m4v *.mov *.mkv *.webm *.ts *.flv);;All Files (*.*)",
					 NULL);
	obs_properties_add_bool(props, S_LOOP, T_("Loop"));
	obs_properties_add_bool(props, S_HW_DECODE, T_("HardwareDecode"));
	return props;
}

static void free_items(struct yt_source *s)
{
	for (size_t i = 0; i < s->items.num; i++)
		bfree(s->items.array[i]);
	da_free(s->items);
}

static void yt_update(void *data, obs_data_t *settings)
{
	struct yt_source *s = data;
	obs_data_array_t *array = obs_data_get_array(settings, S_PLAYLIST);
	size_t count = obs_data_array_count(array);

	pthread_mutex_lock(&s->mutex);

	char *cur_path = (s->cur >= 0 && s->cur_index < s->items.num) ? bstrdup(s->items.array[s->cur_index])
								     : NULL;
	bool was_running = s->state == OBS_MEDIA_STATE_PLAYING || s->state == OBS_MEDIA_STATE_PAUSED;

	free_items(s);
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		const char *path = obs_data_get_string(item, "value");
		if (path && *path) {
			char *copy = bstrdup(path);
			da_push_back(s->items, &copy);
		}
		obs_data_release(item);
	}

	bool hw_changed = s->hw_decode != obs_data_get_bool(settings, S_HW_DECODE);
	s->loop = obs_data_get_bool(settings, S_LOOP);
	s->hw_decode = obs_data_get_bool(settings, S_HW_DECODE);

	if (was_running) {
		if (hw_changed || !s->items.num) {
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
		start_at_locked(s, 0);
	}

	pthread_mutex_unlock(&s->mutex);

	bfree(cur_path);
	obs_data_array_release(array);
}

static void yt_activate(void *data)
{
	struct yt_source *s = data;
	pthread_mutex_lock(&s->mutex);
	if (s->state != OBS_MEDIA_STATE_PLAYING && s->state != OBS_MEDIA_STATE_PAUSED)
		start_at_locked(s, 0);
	pthread_mutex_unlock(&s->mutex);
}

static void *yt_create(obs_data_t *settings, obs_source_t *source)
{
	struct yt_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->cur = -1;
	s->state = OBS_MEDIA_STATE_NONE;
	s->next_index = NO_INDEX;
	pthread_mutex_init(&s->mutex, NULL);
	da_init(s->items);

	yt_update(s, settings);
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
