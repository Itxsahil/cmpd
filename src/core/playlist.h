#ifndef PLAYLIST_H
#define PLAYLIST_H

typedef enum {
    REPEAT_NONE,
    REPEAT_ONE,
    REPEAT_ALL,
} RepeatMode;

typedef struct {
    char path[4096];
    char title[512];
    char artist[512];
    char album[512];
    int  duration;
} Track;

typedef struct Playlist Playlist;

Playlist *playlist_create(void);
void      playlist_add(Playlist *pl, const Track *t);
void      playlist_remove(Playlist *pl, int idx);
void      playlist_clear(Playlist *pl);
int       playlist_count(Playlist *pl);
int       playlist_index(Playlist *pl);
void      playlist_set_index(Playlist *pl, int idx);
Track    *playlist_get(Playlist *pl, int idx);
Track    *playlist_current(Playlist *pl);
int       playlist_next(Playlist *pl);
int       playlist_prev(Playlist *pl);
void      playlist_set_shuffle(Playlist *pl, int shuffle);
int       playlist_shuffle(Playlist *pl);
void      playlist_set_repeat(Playlist *pl, RepeatMode mode);
RepeatMode playlist_repeat(Playlist *pl);
void      playlist_destroy(Playlist *pl);

#endif
