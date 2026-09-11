#include "audio/decoder.h"
#include "audio/output.h"
#include "core/playlist.h"
#include "library/tags.h"
#include "tui/ui.h"

#include <libavutil/log.h>

#include <fcntl.h>
#include <libgen.h>
#include <locale.h>
#include <ncurses.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── state shared with the output fill thread ── */
static struct {
    Decoder        *dec;
    int             playing;
    int             paused;
    int             channels;
    int             sample_rate;
    double          duration;
    pthread_mutex_t lock;
} player;

static Output *output;

/* Pulled on the fill thread. The output layer parks this thread before any
 * decoder swap, but the lock still guards pause and teardown. */
static int fill_cb(float *buf, int frames, int channels,
                   int sample_rate, void *userdata)
{
    (void)channels; (void)sample_rate; (void)userdata;

    pthread_mutex_lock(&player.lock);
    int got = 0;
    if (player.playing && !player.paused && player.dec)
        got = decoder_read(player.dec, buf, frames);
    pthread_mutex_unlock(&player.lock);

    return got > 0 ? got : 0;
}

static void player_close(void)
{
    pthread_mutex_lock(&player.lock);
    if (player.dec) {
        decoder_close(player.dec);
        player.dec = NULL;
    }
    player.playing = 0;
    player.paused  = 0;
    pthread_mutex_unlock(&player.lock);
}

static int player_open(const char *path)
{
    Decoder *dec = decoder_open(path);
    if (!dec) return -1;

    pthread_mutex_lock(&player.lock);
    if (player.dec) decoder_close(player.dec);
    player.dec         = dec;
    player.channels    = decoder_channels(dec);
    player.sample_rate = decoder_sample_rate(dec);
    player.duration    = decoder_duration(dec);
    player.paused      = 0;
    player.playing     = 1;
    pthread_mutex_unlock(&player.lock);
    return 0;
}

static int player_queued_frames(void)
{
    return output ? output_queued_frames(output) : 0;
}

/* Begin playback of `path`. Returns 0 on success, -1 if the file or the audio
 * device could not be opened. */
static int play_path(const char *path, int volume)
{
    /* Park the fill thread first so the decoder swap cannot race it. */
    if (output) output_pause(output);

    if (player_open(path) != 0)
        return -1;

    if (output && output_matches(output, player.channels, player.sample_rate)) {
        /* Same format: keep the device open and just drop stale audio. */
        output_flush(output);
        output_set_volume(output, volume / 100.0f);
        output_resume(output);
        return 0;
    }

    if (output) {
        output_close(output);
        output = NULL;
    }

    output = output_open(player.channels, player.sample_rate, fill_cb, NULL);
    if (!output) {
        player_close();
        return -1;
    }

    output_set_volume(output, volume / 100.0f);
    output_start(output);
    return 0;
}

/* Play the playlist's current entry, skipping over files that fail to open.
 * Returns 0 once something is playing, -1 if nothing in the list works. */
static int play_current(Playlist *pl, int volume)
{
    int attempts = playlist_count(pl);

    while (attempts-- > 0) {
        Track *t = playlist_current(pl);
        if (!t) break;

        if (play_path(t->path, volume) == 0)
            return 0;

        ui_status("Cannot play %s", t->title[0] ? t->title : t->path);
        if (playlist_next(pl) < 0) break;
    }

    player_close();
    return -1;
}

/* Fill in a Track from the file's tags, falling back to the filename when the
 * file carries no usable metadata. */
static void track_from_path(Track *t, const char *path)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->path, sizeof(t->path), "%s", path);

    SongInfo info;
    if (tags_read(path, &info) == 0) {
        snprintf(t->title,  sizeof(t->title),  "%s", info.title  ? info.title  : "");
        snprintf(t->artist, sizeof(t->artist), "%s", info.artist ? info.artist : "");
        snprintf(t->album,  sizeof(t->album),  "%s", info.album  ? info.album  : "");
        t->duration = info.duration;
        tags_free(&info);
    }

    if (!t->title[0]) {
        /* basename() may modify its argument, so hand it a copy. */
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "%s", path);
        snprintf(t->title, sizeof(t->title), "%s", basename(tmp));
    }
}

/* ncurses owns the terminal once the UI is up, but FFmpeg, ALSA and JACK all
 * write diagnostics to stderr, which shreds the display. Park stderr on
 * CMPD_LOG (or /dev/null) for the lifetime of the TUI and restore it after, so
 * startup errors still reach the user. */
static int saved_stderr = -1;

static void silence_stderr(void)
{
    const char *log = getenv("CMPD_LOG");
    int fd = open(log && *log ? log : "/dev/null",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;

    saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0) { close(fd); return; }

    fflush(stderr);
    dup2(fd, STDERR_FILENO);
    close(fd);
}

static void restore_stderr(void)
{
    if (saved_stderr < 0) return;
    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);
    saved_stderr = -1;
}

/* A file is worth queueing if either TagLib or FFmpeg can make sense of it. */
static int is_playable(const char *path)
{
    Decoder *d = decoder_open(path);
    if (!d) return 0;
    decoder_close(d);
    return 1;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    /* FFmpeg logs decode warnings by default; they would land on the TUI. */
    av_log_set_level(AV_LOG_QUIET);

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <music files...>\n", argv[0]);
        return 1;
    }

    Playlist *pl = playlist_create();
    if (!pl) {
        fprintf(stderr, "cmpd: out of memory\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (!is_playable(argv[i])) {
            fprintf(stderr, "cmpd: cannot play %s\n", argv[i]);
            continue;
        }
        Track t;
        track_from_path(&t, argv[i]);
        if (playlist_add(pl, &t) != 0) {
            fprintf(stderr, "cmpd: out of memory\n");
            playlist_destroy(pl);
            return 1;
        }
    }

    if (playlist_count(pl) == 0) {
        fprintf(stderr, "cmpd: no playable files\n");
        playlist_destroy(pl);
        return 1;
    }

    memset(&player, 0, sizeof(player));
    pthread_mutex_init(&player.lock, NULL);

    silence_stderr();
    ui_init();

    UIState st;
    memset(&st, 0, sizeof(st));
    st.playlist = pl;
    st.volume   = 75;
    st.repeat   = REPEAT_ALL;
    playlist_set_repeat(pl, REPEAT_ALL);

    playlist_set_index(pl, 0);
    st.cursor = 0;

    if (play_current(pl, st.volume) != 0)
        ui_status("No audio output available");

    int running = 1;
    while (running) {
        /* ── advance first, so the frame below describes one consistent track ── */
        int queued = player_queued_frames();

        pthread_mutex_lock(&player.lock);
        int is_eof  = player.dec ? decoder_eof(player.dec) : 0;
        int playing = player.playing;
        int paused  = player.paused;
        pthread_mutex_unlock(&player.lock);

        /* Advance only once the buffered tail has actually been played, so the
         * end of each track is not cut off. The cursor is deliberately left
         * alone here: auto-advance must not move where the user is browsing. */
        if (is_eof && queued == 0 && playing && !paused) {
            if (playlist_next(pl) < 0) {
                if (output) output_pause(output);
                player_close();
                ui_status("End of playlist");
            } else if (play_current(pl, st.volume) != 0) {
                ui_status("Playback stopped");
            }
        }

        /* ── sync the UI from the player as it stands now ── */
        queued = player_queued_frames();

        pthread_mutex_lock(&player.lock);
        st.playing = player.playing;
        st.paused  = player.paused;
        st.song_position = 0.0;
        st.song_duration = 0;
        if (player.dec) {
            /* decoder_position() counts decoded frames; subtract what is still
             * sitting in the ring so the clock tracks what is actually heard. */
            double pos = decoder_position(player.dec);
            if (player.sample_rate > 0)
                pos -= (double)queued / player.sample_rate;
            st.song_position = pos > 0.0 ? pos : 0.0;
            if (player.duration > 0.0)
                st.song_duration = (int)(player.duration + 0.5);
        }
        pthread_mutex_unlock(&player.lock);

        Track *cur = playlist_current(pl);
        if (cur) {
            /* Track fields are wider than the UI copies; truncation is fine. */
            snprintf(st.song_title,  sizeof(st.song_title),  "%.255s", cur->title);
            snprintf(st.song_artist, sizeof(st.song_artist), "%.255s", cur->artist);
            snprintf(st.song_album,  sizeof(st.song_album),  "%.255s", cur->album);
            if (st.song_duration <= 0) st.song_duration = cur->duration;
        }
        st.shuffle = playlist_shuffle(pl);
        st.repeat  = playlist_repeat(pl);

        ui_draw(&st);

        int ch = getch();
        if (ch == ERR) continue;

        UIAction action = ui_handle_key(ch, &st);

        switch (action) {
        case ACTION_QUIT:
            running = 0;
            break;

        case ACTION_PLAY:
        case ACTION_PLAY_SELECTED:
            if (play_current(pl, st.volume) != 0)
                ui_status("Cannot play track");
            break;

        case ACTION_PAUSE:
            pthread_mutex_lock(&player.lock);
            player.paused = 1;
            pthread_mutex_unlock(&player.lock);
            if (output) output_pause(output);
            break;

        case ACTION_RESUME:
            pthread_mutex_lock(&player.lock);
            player.paused = 0;
            pthread_mutex_unlock(&player.lock);
            if (output) output_resume(output);
            break;

        case ACTION_NEXT:
            if (playlist_next(pl) < 0) break;
            st.cursor = playlist_index(pl);
            if (play_current(pl, st.volume) != 0)
                ui_status("Cannot play track");
            break;

        case ACTION_PREV:
            if (playlist_prev(pl) < 0) break;
            st.cursor = playlist_index(pl);
            if (play_current(pl, st.volume) != 0)
                ui_status("Cannot play track");
            break;

        case ACTION_SEEK_FWD:
        case ACTION_SEEK_BACK: {
            double step = (action == ACTION_SEEK_FWD) ? UI_SEEK_STEP
                                                      : -UI_SEEK_STEP;
            /* Park the fill thread and stop the device before touching the
             * decoder, then drop the audio queued from the old position. */
            if (output) output_pause(output);

            pthread_mutex_lock(&player.lock);
            int ok = 0;
            if (player.dec) {
                double target = decoder_position(player.dec) + step;
                ok = decoder_seek(player.dec, target) == 0;
                if (ok) st.song_position = decoder_position(player.dec);
            }
            int still_paused = player.paused;
            pthread_mutex_unlock(&player.lock);

            if (output) {
                output_flush(output);
                if (!still_paused) output_resume(output);
            }
            if (!ok) ui_status("Seek failed");
            break;
        }

        case ACTION_VOL_UP:
            st.volume += 5;
            if (st.volume > 100) st.volume = 100;
            if (output) output_set_volume(output, st.volume / 100.0f);
            break;

        case ACTION_VOL_DOWN:
            st.volume -= 5;
            if (st.volume < 0) st.volume = 0;
            if (output) output_set_volume(output, st.volume / 100.0f);
            break;

        case ACTION_TOGGLE_SHUFFLE:
            playlist_set_shuffle(pl, !playlist_shuffle(pl));
            ui_status("Shuffle %s", playlist_shuffle(pl) ? "On" : "Off");
            break;

        case ACTION_CYCLE_REPEAT: {
            RepeatMode r = playlist_repeat(pl);
            r = (r == REPEAT_ALL) ? REPEAT_NONE : (RepeatMode)(r + 1);
            playlist_set_repeat(pl, r);
            static const char *const names[] = { "None", "One", "All" };
            ui_status("Repeat: %s", names[r]);
            break;
        }

        case ACTION_TOGGLE_HELP:
            st.show_help = !st.show_help;
            break;

        default:
            break;
        }
    }

    if (output) output_close(output);
    player_close();
    pthread_mutex_destroy(&player.lock);
    ui_cleanup();
    restore_stderr();
    playlist_destroy(pl);

    return 0;
}
