#include "core/playlist.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Playlist {
    Track      *tracks;
    int         count;
    int         capacity;
    int         current;
    int         shuffle;
    RepeatMode  repeat;
    int        *shuffle_order;
};

Playlist *playlist_create(void)
{
    Playlist *pl = calloc(1, sizeof(*pl));
    if (!pl) return NULL;
    pl->current  = -1;
    pl->capacity = 16;
    pl->tracks   = malloc(pl->capacity * sizeof(Track));
    if (!pl->tracks) { free(pl); return NULL; }
    srand((unsigned)time(NULL));
    return pl;
}

void playlist_add(Playlist *pl, const Track *t)
{
    if (!pl) return;
    if (pl->count >= pl->capacity) {
        pl->capacity *= 2;
        pl->tracks = realloc(pl->tracks, pl->capacity * sizeof(Track));
    }
    pl->tracks[pl->count] = *t;
    pl->count++;
    if (pl->current < 0) pl->current = 0;
    /* rebuild shuffle order */
    free(pl->shuffle_order);
    pl->shuffle_order = NULL;
}

void playlist_remove(Playlist *pl, int idx)
{
    if (!pl || idx < 0 || idx >= pl->count) return;
    if (idx < pl->count - 1)
        memmove(&pl->tracks[idx], &pl->tracks[idx + 1],
                (pl->count - idx - 1) * sizeof(Track));
    pl->count--;
    if (pl->current >= pl->count)
        pl->current = pl->count - 1;
    free(pl->shuffle_order);
    pl->shuffle_order = NULL;
}

void playlist_clear(Playlist *pl)
{
    if (!pl) return;
    pl->count    = 0;
    pl->current  = -1;
    free(pl->shuffle_order);
    pl->shuffle_order = NULL;
}

int playlist_count(Playlist *pl) { return pl ? pl->count : 0; }
int playlist_index(Playlist *pl) { return pl ? pl->current : -1; }

void playlist_set_index(Playlist *pl, int idx)
{
    if (pl && idx >= 0 && idx < pl->count)
        pl->current = idx;
}

Track *playlist_get(Playlist *pl, int idx)
{
    if (!pl || idx < 0 || idx >= pl->count) return NULL;
    return &pl->tracks[idx];
}

Track *playlist_current(Playlist *pl)
{
    return playlist_get(pl, pl ? pl->current : -1);
}

static void build_shuffle(Playlist *pl)
{
    if (!pl || pl->count == 0) return;
    free(pl->shuffle_order);
    pl->shuffle_order = malloc(pl->count * sizeof(int));
    for (int i = 0; i < pl->count; i++)
        pl->shuffle_order[i] = i;
    /* Fisher-Yates */
    for (int i = pl->count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = pl->shuffle_order[i];
        pl->shuffle_order[i] = pl->shuffle_order[j];
        pl->shuffle_order[j] = t;
    }
}

int playlist_next(Playlist *pl)
{
    if (!pl || pl->count == 0) return -1;
    if (pl->repeat == REPEAT_ONE) return pl->current;

    if (pl->shuffle) {
        if (!pl->shuffle_order) build_shuffle(pl);
        /* find current position in shuffle order */
        int pos = -1;
        for (int i = 0; i < pl->count; i++) {
            if (pl->shuffle_order[i] == pl->current) {
                pos = i;
                break;
            }
        }
        pos++;
        if (pos >= pl->count) {
            if (pl->repeat == REPEAT_NONE) return -1;
            build_shuffle(pl);
            pos = 0;
        }
        pl->current = pl->shuffle_order[pos];
    } else {
        pl->current++;
        if (pl->current >= pl->count) {
            if (pl->repeat == REPEAT_NONE) {
                pl->current = pl->count - 1;
                return -1;
            }
            pl->current = 0;
        }
    }
    return pl->current;
}

int playlist_prev(Playlist *pl)
{
    if (!pl || pl->count == 0) return -1;
    if (pl->repeat == REPEAT_ONE) return pl->current;

    if (pl->shuffle) {
        if (!pl->shuffle_order) build_shuffle(pl);
        int pos = -1;
        for (int i = 0; i < pl->count; i++) {
            if (pl->shuffle_order[i] == pl->current) {
                pos = i;
                break;
            }
        }
        pos--;
        if (pos < 0) {
            if (pl->repeat == REPEAT_NONE) return -1;
            build_shuffle(pl);
            pos = pl->count - 1;
        }
        pl->current = pl->shuffle_order[pos];
    } else {
        pl->current--;
        if (pl->current < 0) {
            if (pl->repeat == REPEAT_NONE) {
                pl->current = 0;
                return -1;
            }
            pl->current = pl->count - 1;
        }
    }
    return pl->current;
}

void playlist_set_shuffle(Playlist *pl, int shuffle)
{
    if (!pl) return;
    pl->shuffle = shuffle;
    free(pl->shuffle_order);
    pl->shuffle_order = NULL;
    if (shuffle && pl->count > 0) build_shuffle(pl);
}

int playlist_shuffle(Playlist *pl) { return pl ? pl->shuffle : 0; }

void playlist_set_repeat(Playlist *pl, RepeatMode mode)
{
    if (pl) pl->repeat = mode;
}

RepeatMode playlist_repeat(Playlist *pl)
{
    return pl ? pl->repeat : REPEAT_NONE;
}

void playlist_destroy(Playlist *pl)
{
    if (!pl) return;
    free(pl->tracks);
    free(pl->shuffle_order);
    free(pl);
}
