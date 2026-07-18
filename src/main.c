#include "audio/decoder.h"
#include "audio/output.h"
#include "core/playlist.h"
#include "library/tags.h"
#include "tui/ui.h"

#include <locale.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <ncurses.h>

/* ── state shared with fill thread ── */
static struct {
    Decoder    *dec;
    int         playing;
    int         paused;
    int         channels;
    int         sample_rate;
    double      duration;
    int64_t     total_frames;
    pthread_mutex_t lock;
} player;

static int fill_cb(float *buf, int frames, int channels,
                   int sample_rate, void *userdata)
{
    (void)channels;
    (void)sample_rate;
    (void)userdata;
    pthread_mutex_lock(&player.lock);

    if (!player.playing || player.paused || !player.dec) {
        pthread_mutex_unlock(&player.lock);
        return 0;
    }

    int got = decoder_read(player.dec, buf, frames);
    pthread_mutex_unlock(&player.lock);
    return got;
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
    player.total_frames = 0;
    pthread_mutex_unlock(&player.lock);
}

static int player_open(const char *path)
{
    player_close();

    Decoder *dec = decoder_open(path);
    if (!dec) return -1;

    pthread_mutex_lock(&player.lock);
    player.dec         = dec;
    player.channels    = decoder_channels(dec);
    player.sample_rate = decoder_sample_rate(dec);
    player.duration    = decoder_duration(dec);
    player.total_frames = 0;
    player.paused      = 0;
    player.playing     = 1;
    pthread_mutex_unlock(&player.lock);
    return 0;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");

    /* ── init playlist ── */
    Playlist *pl = playlist_create();
    if (!pl) return 1;

    if (argc < 2) {
        fprintf(stderr, "Usage: cmpd <music files...>\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        SongInfo info;
        if (tags_read(argv[i], &info) == 0) {
            Track t;
            memset(&t, 0, sizeof(t));
            strncpy(t.path,   argv[i], sizeof(t.path) - 1);
            strncpy(t.title,  info.title  ? info.title  : "", sizeof(t.title) - 1);
            strncpy(t.artist, info.artist ? info.artist : "", sizeof(t.artist) - 1);
            strncpy(t.album,  info.album  ? info.album  : "", sizeof(t.album) - 1);
            t.duration = info.duration;
            playlist_add(pl, &t);
            tags_free(&info);
        } else {
            fprintf(stderr, "cmpd: cannot read %s\n", argv[i]);
        }
    }

    if (playlist_count(pl) == 0) {
        fprintf(stderr, "cmpd: no playable files\n");
        playlist_destroy(pl);
        return 1;
    }

    /* ── init player state ── */
    memset(&player, 0, sizeof(player));
    pthread_mutex_init(&player.lock, NULL);

    /* ── init TUI ── */
    ui_init();

    UIState st;
    memset(&st, 0, sizeof(st));
    st.playlist = pl;
    st.volume   = 75;
    st.repeat   = REPEAT_ALL;

    /* ── audio output (created on first play) ── */
    Output *output = NULL;

    /* auto-start first track */
    if (playlist_count(pl) > 0) {
        st.cursor = 0;
        playlist_set_index(pl, 0);
        Track *t = playlist_current(pl);
        if (t && player_open(t->path) == 0) {
            output = output_open(player.channels, player.sample_rate,
                                 fill_cb, NULL);
            if (output) {
                output_set_volume(output, st.volume / 100.0f);
                output_start(output);
            }
        }
    }

    /* ── main event loop ── */
    int running = 1;
    while (running) {
        /* sync UI state */
        pthread_mutex_lock(&player.lock);
        st.playing = player.playing;
        st.paused  = player.paused;
        if (player.dec) {
            st.song_position = decoder_position(player.dec);
            st.song_duration = player.duration;
        }
        int is_eof = player.dec ? decoder_eof(player.dec) : 0;
        st.song_duration = player.duration;
        pthread_mutex_unlock(&player.lock);

        /* auto-advance on EOF */
        if (is_eof && st.playing && !st.paused) {
            if (output) output_pause(output);
            player_close();

            int next = playlist_next(pl);
            if (next < 0) {
                st.playing = 0;
            } else {
                Track *t = playlist_current(pl);
                if (t) {
                    if (player_open(t->path) == 0) {
                        if (output) output_close(output);
                        output = output_open(player.channels,
                                             player.sample_rate,
                                             fill_cb, NULL);
                        if (output) {
                            output_set_volume(output, st.volume / 100.0f);
                            output_start(output);
                        }
                    }
                }
            }
        }

        /* update UI state from playlist */
        Track *cur = playlist_current(pl);
        if (cur) {
            strncpy(st.song_title,  cur->title,  sizeof(st.song_title) - 1);
            strncpy(st.song_artist, cur->artist, sizeof(st.song_artist) - 1);
            strncpy(st.song_album,  cur->album,  sizeof(st.song_album) - 1);
            st.song_duration = cur->duration;
        }
        st.shuffle = playlist_shuffle(pl);
        st.repeat  = playlist_repeat(pl);

        ui_draw(&st);

        /* ── handle input ── */
        int ch = getch();
        if (ch == ERR) continue;

        UIAction action = ui_handle_key(ch, &st);

        switch (action) {
        case ACTION_QUIT:
            running = 0;
            break;

        case ACTION_PLAY:
        case ACTION_PLAY_SELECTED: {
            Track *t = playlist_current(pl);
            if (!t) break;
            if (output) output_close(output);
            output = NULL;

            if (player_open(t->path) == 0) {
                output = output_open(player.channels, player.sample_rate,
                                     fill_cb, NULL);
                if (output) {
                    output_set_volume(output, st.volume / 100.0f);
                    output_start(output);
                }
            }
            break;
        }

        case ACTION_PAUSE:
            player.paused = 1;
            if (output) output_pause(output);
            break;

        case ACTION_RESUME:
            player.paused = 0;
            if (output) output_resume(output);
            break;

        case ACTION_NEXT: {
            if (playlist_next(pl) < 0) break;
            if (output) output_close(output);
            output = NULL;
            Track *t = playlist_current(pl);
            if (t && player_open(t->path) == 0) {
                output = output_open(player.channels, player.sample_rate,
                                     fill_cb, NULL);
                if (output) {
                    output_set_volume(output, st.volume / 100.0f);
                    output_start(output);
                }
            }
            break;
        }

        case ACTION_PREV: {
            if (playlist_prev(pl) < 0) break;
            if (output) output_close(output);
            output = NULL;
            Track *t = playlist_current(pl);
            if (t && player_open(t->path) == 0) {
                output = output_open(player.channels, player.sample_rate,
                                     fill_cb, NULL);
                if (output) {
                    output_set_volume(output, st.volume / 100.0f);
                    output_start(output);
                }
            }
            break;
        }

        case ACTION_VOL_UP:
            if (st.volume < 100) st.volume += 5;
            if (output) output_set_volume(output, st.volume / 100.0f);
            break;

        case ACTION_VOL_DOWN:
            if (st.volume > 5) st.volume -= 5;
            if (output) output_set_volume(output, st.volume / 100.0f);
            break;

        case ACTION_TOGGLE_SHUFFLE:
            playlist_set_shuffle(pl, !playlist_shuffle(pl));
            ui_status("Shuffle %s", playlist_shuffle(pl) ? "On" : "Off");
            break;

        case ACTION_CYCLE_REPEAT: {
            RepeatMode r = playlist_repeat(pl);
            if (r == REPEAT_ALL) r = REPEAT_NONE;
            else r = (RepeatMode)(r + 1);
            playlist_set_repeat(pl, r);
            const char *names[] = {"None", "One", "All"};
            ui_status("Repeat: %s", names[r]);
            break;
        }

        default:
            break;
        }
    }

    /* ── cleanup ── */
    if (output) output_close(output);
    player_close();
    pthread_mutex_destroy(&player.lock);
    ui_cleanup();
    playlist_destroy(pl);

    return 0;
}
