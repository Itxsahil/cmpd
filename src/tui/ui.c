#include "tui/ui.h"

#include <ncurses.h>
#include <panel.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#define C_NORMAL      1
#define C_HIGHLIGHT   2
#define C_STATUS      3
#define C_PROGRESS    4
#define C_TITLE       5
#define C_DIM         6

static struct {
    WINDOW *top;
    WINDOW *progress;
    WINDOW *left;
    WINDOW *right;
    WINDOW *status;

    PANEL *top_panel;
    PANEL *progress_panel;
    PANEL *left_panel;
    PANEL *right_panel;
    PANEL *status_panel;

    char   status_msg[256];
    time_t status_time;
} tui;

void ui_init(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(250);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_NORMAL,    -1,     -1);
        init_pair(C_HIGHLIGHT, COLOR_BLACK, COLOR_CYAN);
        init_pair(C_STATUS,    COLOR_WHITE, COLOR_BLUE);
        init_pair(C_PROGRESS,  COLOR_GREEN,  -1);
        init_pair(C_TITLE,     COLOR_YELLOW, -1);
        init_pair(C_DIM,       COLOR_WHITE,  -1);
    }

    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    tui.top      = newwin(1, cols, 0, 0);
    tui.progress = newwin(1, cols, 1, 0);
    tui.left     = newwin(rows - 3, cols / 2, 2, 0);
    tui.right    = newwin(rows - 3, cols - cols / 2, 2, cols / 2);
    tui.status   = newwin(1, cols, rows - 1, 0);

    tui.top_panel      = new_panel(tui.top);
    tui.progress_panel = new_panel(tui.progress);
    tui.left_panel     = new_panel(tui.left);
    tui.right_panel    = new_panel(tui.right);
    tui.status_panel   = new_panel(tui.status);
}

static void draw_top(UIState *state)
{
    WINDOW *w = tui.top;
    werase(w);

    if (state->playing && state->song_title[0]) {
        wattron(w, COLOR_PAIR(C_TITLE) | A_BOLD);

        const char *status_str = state->paused ? " ||" : " >>";
        wprintw(w, " %s  %s - %s", status_str, state->song_artist, state->song_title);
        wattroff(w, COLOR_PAIR(C_TITLE) | A_BOLD);

        int rows, cols;
        getmaxyx(w, rows, cols);
        (void)rows;
        int mins = state->song_duration / 60;
        int secs = state->song_duration % 60;
        int pos_mins = (int)state->song_position / 60;
        int pos_secs = (int)state->song_position % 60;
        char time_str[32];
        snprintf(time_str, sizeof(time_str), "%d:%02d / %d:%02d",
                 pos_mins, pos_secs, mins, secs);

        wattron(w, COLOR_PAIR(C_NORMAL));
        mvwprintw(w, 0, cols - (int)strlen(time_str) - 2, " %s ", time_str);
        wattroff(w, COLOR_PAIR(C_NORMAL));
    } else {
        wprintw(w, " cmpd - C Music Player Demonic");
    }

    wnoutrefresh(w);
}

static void draw_progress(UIState *state)
{
    WINDOW *w = tui.progress;
    werase(w);

    int rows, cols;
    getmaxyx(w, rows, cols);
    (void)rows;

    if (state->playing && state->song_duration > 0) {
        float pct = state->song_duration > 0
                        ? state->song_position / state->song_duration
                        : 0.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;

        int bar_w = cols - 14;
        if (bar_w > 0) {
            int filled = (int)(bar_w * pct);
            if (filled > bar_w) filled = bar_w;

            wattron(w, COLOR_PAIR(C_PROGRESS));
            for (int i = 0; i < filled; i++)
                mvwaddstr(w, 0, i, "#");
            wattroff(w, COLOR_PAIR(C_PROGRESS));

            wattron(w, COLOR_PAIR(C_DIM));
            for (int i = filled; i < bar_w; i++)
                mvwaddstr(w, 0, i, "-");
            wattroff(w, COLOR_PAIR(C_DIM));
        }

        char vol_str[16];
        snprintf(vol_str, sizeof(vol_str), " Vol:%d%%", state->volume);
        mvwprintw(w, 0, cols - (int)strlen(vol_str), vol_str);
    }

    wnoutrefresh(w);
}

static void draw_left(UIState *state)
{
    WINDOW *w = tui.left;
    werase(w);

    int rows, cols;
    getmaxyx(w, rows, cols);
    (void)cols;

    if (state->mode == MODE_PLAYLIST) {
        wattron(w, A_UNDERLINE | A_BOLD);
        mvwaddstr(w, 0, 0, " Playlist");
        wattroff(w, A_UNDERLINE | A_BOLD);

        int n = state->playlist ? playlist_count(state->playlist) : 0;
        int scroll = state->scroll;
        int cursor = state->cursor;

        for (int i = 0; i < rows - 1 && scroll + i < n; i++) {
            int idx = scroll + i;
            Track *t = playlist_get(state->playlist, idx);
            if (!t) break;

            int is_current = (idx == playlist_index(state->playlist));
            int is_cursor  = (idx == cursor);

            if (is_cursor)
                wattron(w, COLOR_PAIR(C_HIGHLIGHT));

            char marker = ' ';
            if (is_current) {
                marker = state->paused ? '|' : '>';
                wattron(w, A_BOLD);
            }

            mvwprintw(w, 1 + i, 0, "%c %.200s - %.200s", marker,
                      t->artist[0] ? t->artist : "?",
                      t->title[0] ? t->title : "?");

            if (is_current)
                wattroff(w, A_BOLD);
            if (is_cursor)
                wattroff(w, COLOR_PAIR(C_HIGHLIGHT));
        }
    } else {
        mvwaddstr(w, 0, 0, " [Browser - coming soon]");
    }

    wnoutrefresh(w);
}

static void draw_right(UIState *state)
{
    WINDOW *w = tui.right;
    werase(w);

    if (state->playing && state->song_title[0]) {
        mvwprintw(w, 1, 2, "Title:   %s", state->song_title);
        mvwprintw(w, 2, 2, "Artist:  %s", state->song_artist);
        mvwprintw(w, 3, 2, "Album:   %s", state->song_album);
    } else {
        mvwaddstr(w, 1, 2, "No track selected");
    }

    wnoutrefresh(w);
}

static void draw_status(UIState *state)
{
    WINDOW *w = tui.status;
    werase(w);

    char buf[256];
    int pos = 0;

    if (state->playing)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        " %s ", state->paused ? "|| Paused" : ">> Playing");
    else
        pos += snprintf(buf + pos, sizeof(buf) - pos, " ** Stopped ");

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "| # %s ", state->shuffle ? "On" : "Off");

    const char *r = "None";
    if (state->repeat == REPEAT_ONE)  r = "One";
    if (state->repeat == REPEAT_ALL)  r = "All";
    pos += snprintf(buf + pos, sizeof(buf) - pos, "| R %s ", r);

    if (state->playlist)
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "| %d songs ", playlist_count(state->playlist));

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "| Vol:%d%%", state->volume);

    wattron(w, COLOR_PAIR(C_STATUS));
    mvwaddstr(w, 0, 0, buf);
    wattroff(w, COLOR_PAIR(C_STATUS));

    if (tui.status_msg[0]) {
        time_t now = time(NULL);
        if (now - tui.status_time < 3) {
            int rows, cols;
            getmaxyx(w, rows, cols);
            (void)rows;
            int len = (int)strlen(tui.status_msg);
            if (len > cols) len = cols;
            mvwaddstr(w, 0, cols - len - 1, tui.status_msg);
        } else {
            tui.status_msg[0] = '\0';
        }
    }

    wnoutrefresh(w);
}

void ui_draw(UIState *state)
{
    draw_top(state);
    draw_progress(state);
    draw_left(state);
    draw_right(state);
    draw_status(state);
    update_panels();
    doupdate();
}

void ui_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tui.status_msg, sizeof(tui.status_msg), fmt, ap);
    va_end(ap);
    tui.status_time = time(NULL);
}

UIAction ui_handle_key(int ch, UIState *state)
{
    int rows, cols;
    getmaxyx(tui.left, rows, cols);
    (void)cols;
    int n = state->playlist ? playlist_count(state->playlist) : 0;

    switch (ch) {
    case 'q': case 'Q':
        return ACTION_QUIT;
    case ' ':
        if (!state->playing)
            return ACTION_PLAY;
        else if (state->paused)
            return ACTION_RESUME;
        else
            return ACTION_PAUSE;
    case 'n': case 'N':
        return ACTION_NEXT;
    case 'p': case 'P':
        return ACTION_PREV;
    case '+': case '=':
        return ACTION_VOL_UP;
    case '-': case '_':
        return ACTION_VOL_DOWN;
    case 's':
        return ACTION_TOGGLE_SHUFFLE;
    case 'r':
        return ACTION_CYCLE_REPEAT;
    case 'j': case KEY_DOWN:
        if (state->mode == MODE_PLAYLIST && state->cursor < n - 1) {
            state->cursor++;
            if (state->cursor - state->scroll >= rows - 1)
                state->scroll++;
        }
        break;
    case 'k': case KEY_UP:
        if (state->cursor > 0) {
            state->cursor--;
            if (state->cursor < state->scroll)
                state->scroll--;
        }
        break;
    case '\n': case '\r':
        if (state->mode == MODE_PLAYLIST && n > 0) {
            playlist_set_index(state->playlist, state->cursor);
            return ACTION_PLAY_SELECTED;
        }
        break;
    }
    return ACTION_NONE;
}

void ui_cleanup(void)
{
    del_panel(tui.status_panel);
    del_panel(tui.right_panel);
    del_panel(tui.left_panel);
    del_panel(tui.progress_panel);
    del_panel(tui.top_panel);

    delwin(tui.status);
    delwin(tui.right);
    delwin(tui.left);
    delwin(tui.progress);
    delwin(tui.top);

    endwin();
}
