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

    /* Shuffle is a permutation of track indices plus a cursor into it. Walking
     * the permutation is what guarantees every track plays once per cycle. */
    int        *shuffle_order;
    int         shuffle_pos;
};

static void build_shuffle(Playlist *pl, int anchor_current);
static void resync_shuffle_pos(Playlist *pl);

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

int playlist_add(Playlist *pl, const Track *t)
{
    if (!pl || !t) return -1;

    if (pl->count >= pl->capacity) {
        int cap = pl->capacity ? pl->capacity * 2 : 16;
        /* Keep the existing list intact if the growth fails. */
        Track *grown = realloc(pl->tracks, (size_t)cap * sizeof(Track));
        if (!grown) return -1;
        pl->tracks   = grown;
        pl->capacity = cap;
    }

    pl->tracks[pl->count] = *t;
    pl->count++;
    if (pl->current < 0) pl->current = 0;

    /* The shuffle order is stale once the list changes; rebuild on demand. */
    free(pl->shuffle_order);
    pl->shuffle_order = NULL;
    pl->shuffle_pos = 0;
    return 0;
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
    pl->shuffle_pos = 0;
}

void playlist_clear(Playlist *pl)
{
    if (!pl) return;
    pl->count    = 0;
    pl->current  = -1;
    free(pl->shuffle_order);
    pl->shuffle_order = NULL;
    pl->shuffle_pos = 0;
}

int playlist_count(Playlist *pl) { return pl ? pl->count : 0; }
int playlist_index(Playlist *pl) { return pl ? pl->current : -1; }

void playlist_set_index(Playlist *pl, int idx)
{
    if (!pl || idx < 0 || idx >= pl->count) return;
    pl->current = idx;
    /* Jumping straight to a track keeps the shuffle cycle coherent. */
    resync_shuffle_pos(pl);
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

/* Build a fresh permutation. When anchor_current is set, the track playing now
 * is moved to the front so the cycle starts from it and still covers every
 * other track exactly once. */
static void build_shuffle(Playlist *pl, int anchor_current)
{
    if (!pl || pl->count == 0) return;

    free(pl->shuffle_order);
    pl->shuffle_order = malloc((size_t)pl->count * sizeof(int));
    /* Callers fall back to sequential order when this is NULL. */
    if (!pl->shuffle_order) return;

    for (int i = 0; i < pl->count; i++)
        pl->shuffle_order[i] = i;

    /* Fisher-Yates */
    for (int i = pl->count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = pl->shuffle_order[i];
        pl->shuffle_order[i] = pl->shuffle_order[j];
        pl->shuffle_order[j] = t;
    }

    pl->shuffle_pos = 0;

    if (anchor_current && pl->current >= 0) {
        for (int i = 0; i < pl->count; i++) {
            if (pl->shuffle_order[i] == pl->current) {
                pl->shuffle_order[i] = pl->shuffle_order[0];
                pl->shuffle_order[0] = pl->current;
                break;
            }
        }
    }
}

/* Point shuffle_pos at wherever the current track sits in the permutation. */
static void resync_shuffle_pos(Playlist *pl)
{
    if (!pl || !pl->shuffle_order) return;
    for (int i = 0; i < pl->count; i++) {
        if (pl->shuffle_order[i] == pl->current) {
            pl->shuffle_pos = i;
            return;
        }
    }
}

int playlist_next(Playlist *pl)
{
    if (!pl || pl->count == 0) return -1;
    if (pl->repeat == REPEAT_ONE) return pl->current;

    if (pl->shuffle && !pl->shuffle_order) build_shuffle(pl, 1);

    if (pl->shuffle && pl->shuffle_order) {
        int pos = pl->shuffle_pos + 1;
        if (pos >= pl->count) {
            if (pl->repeat == REPEAT_NONE) return -1;
            /* A completed cycle starts a new permutation. */
            build_shuffle(pl, 0);
            if (!pl->shuffle_order) return -1;
            pos = 0;
        }
        pl->shuffle_pos = pos;
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

    if (pl->shuffle && !pl->shuffle_order) build_shuffle(pl, 1);

    if (pl->shuffle && pl->shuffle_order) {
        int pos = pl->shuffle_pos - 1;
        if (pos < 0) {
            if (pl->repeat == REPEAT_NONE) return -1;
            /* Wrap inside the current permutation; reshuffling here would make
             * stepping backwards jump to unrelated tracks. */
            pos = pl->count - 1;
        }
        pl->shuffle_pos = pos;
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
    pl->shuffle_pos = 0;
    if (shuffle && pl->count > 0) build_shuffle(pl, 1);
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
