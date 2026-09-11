#ifndef UI_H
#define UI_H

#include "core/playlist.h"

typedef enum {
    MODE_PLAYLIST,
    MODE_BROWSER,
} UIMode;

typedef enum {
    ACTION_NONE,
    ACTION_QUIT,
    ACTION_PLAY,
    ACTION_PAUSE,
    ACTION_RESUME,
    ACTION_NEXT,
    ACTION_PREV,
    ACTION_VOL_UP,
    ACTION_VOL_DOWN,
    ACTION_TOGGLE_SHUFFLE,
    ACTION_CYCLE_REPEAT,
    ACTION_PLAY_SELECTED,
    ACTION_SEEK_FWD,
    ACTION_SEEK_BACK,
    ACTION_TOGGLE_HELP,
} UIAction;

typedef struct {
    int      playing;
    int      paused;
    char     song_title[256];
    char     song_artist[256];
    char     song_album[256];
    int      song_duration;
    double   song_position;
    int      volume;
    int      shuffle;
    RepeatMode repeat;

    Playlist *playlist;
    int      cursor;
    int      scroll;

    UIMode   mode;
    int      show_help;
    char     search_buf[256];
    int      search_pos;
} UIState;

/* Seconds moved per seek keypress. */
#define UI_SEEK_STEP 5.0

void ui_init(void);
void ui_draw(UIState *state);
UIAction ui_handle_key(int ch, UIState *state);
void ui_cleanup(void);
void ui_status(const char *fmt, ...);

#endif
