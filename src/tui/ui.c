#include "tui/ui.h"

#include <ncurses.h>
#include <panel.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define C_NORMAL      1
#define C_HIGHLIGHT   2
#define C_STATUS      3
#define C_PROGRESS    4
#define C_TITLE       5
#define C_DIM         6

/* Rows consumed by the top bar, the progress bar and the status bar. */
#define CHROME_ROWS 3

/* Seconds a transient status message stays on screen. */
#define STATUS_TTL 3

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

/* ── window management ─────────────────────────────────────────────────── */

static void destroy_windows(void)
{
    if (tui.status_panel)   del_panel(tui.status_panel);
    if (tui.right_panel)    del_panel(tui.right_panel);
    if (tui.left_panel)     del_panel(tui.left_panel);
    if (tui.progress_panel) del_panel(tui.progress_panel);
    if (tui.top_panel)      del_panel(tui.top_panel);

    if (tui.status)   delwin(tui.status);
    if (tui.right)    delwin(tui.right);
    if (tui.left)     delwin(tui.left);
    if (tui.progress) delwin(tui.progress);
    if (tui.top)      delwin(tui.top);

    tui.status_panel = tui.right_panel = tui.left_panel = NULL;
    tui.progress_panel = tui.top_panel = NULL;
    tui.status = tui.right = tui.left = tui.progress = tui.top = NULL;
}

static void create_windows(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    /* Keep every window at least 1x1 so ncurses never gets a bogus geometry
     * on a very small terminal. */
    if (rows < CHROME_ROWS + 1) rows = CHROME_ROWS + 1;
    if (cols < 2) cols = 2;

    int body_rows = rows - CHROME_ROWS;
    int left_cols = cols / 2;
    if (left_cols < 1) left_cols = 1;
    int right_cols = cols - left_cols;
    if (right_cols < 1) right_cols = 1;

    tui.top      = newwin(1, cols, 0, 0);
    tui.progress = newwin(1, cols, 1, 0);
    tui.left     = newwin(body_rows, left_cols, 2, 0);
    tui.right    = newwin(body_rows, right_cols, 2, left_cols);
    tui.status   = newwin(1, cols, rows - 1, 0);

    tui.top_panel      = new_panel(tui.top);
    tui.progress_panel = new_panel(tui.progress);
    tui.left_panel     = new_panel(tui.left);
    tui.right_panel    = new_panel(tui.right);
    tui.status_panel   = new_panel(tui.status);
}

void ui_init(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    /* Drives the redraw rate; the progress bar ticks at this cadence. */
    timeout(100);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_NORMAL,    -1,           -1);
        init_pair(C_HIGHLIGHT, COLOR_BLACK,  COLOR_CYAN);
        init_pair(C_STATUS,    COLOR_WHITE,  COLOR_BLUE);
        init_pair(C_PROGRESS,  COLOR_GREEN,  -1);
        init_pair(C_TITLE,     COLOR_YELLOW, -1);
        init_pair(C_DIM,       -1,           -1);
    }

    create_windows();
}

static void ui_resize(void)
{
    int rows, cols;

    endwin();
    refresh();
    clear();

    getmaxyx(stdscr, rows, cols);
    (void)rows; (void)cols;

    destroy_windows();
    create_windows();

    clearok(stdscr, TRUE);
}

/* Clamp cursor and scroll to the current playlist length and viewport. */
static void clamp_view(UIState *state)
{
    int n = state->playlist ? playlist_count(state->playlist) : 0;

    if (state->cursor >= n) state->cursor = n - 1;
    if (state->cursor < 0)  state->cursor = 0;

    int rows = tui.left ? getmaxy(tui.left) : 1;
    int visible = rows - 1;          /* one row is the "Playlist" header */
    if (visible < 1) visible = 1;

    if (state->scroll > state->cursor)
        state->scroll = state->cursor;
    if (state->cursor - state->scroll >= visible)
        state->scroll = state->cursor - visible + 1;

    int max_scroll = n - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (state->scroll > max_scroll) state->scroll = max_scroll;
    if (state->scroll < 0) state->scroll = 0;
}

/* ── drawing ───────────────────────────────────────────────────────────── */

static void format_time(char *buf, size_t n, double seconds)
{
    if (seconds < 0) seconds = 0;
    int total = (int)seconds;
    snprintf(buf, n, "%d:%02d", total / 60, total % 60);
}

static void draw_top(UIState *state)
{
    WINDOW *w = tui.top;
    werase(w);

    int cols = getmaxx(w);

    if (state->playing && state->song_title[0]) {
        wattron(w, COLOR_PAIR(C_TITLE) | A_BOLD);
        mvwprintw(w, 0, 0, " %s  %s - %s",
                  state->paused ? "||" : ">>",
                  state->song_artist[0] ? state->song_artist : "Unknown",
                  state->song_title);
        wattroff(w, COLOR_PAIR(C_TITLE) | A_BOLD);

        char pos_str[16], dur_str[16], time_str[40];
        format_time(pos_str, sizeof(pos_str), state->song_position);
        format_time(dur_str, sizeof(dur_str), state->song_duration);
        snprintf(time_str, sizeof(time_str), " %s / %s ", pos_str, dur_str);

        int x = cols - (int)strlen(time_str);
        if (x > 0)
            mvwaddnstr(w, 0, x, time_str, cols - x);
    } else {
        mvwaddnstr(w, 0, 0, " cmpd - C Music Player Daemon", cols);
    }

    wnoutrefresh(w);
}

static void draw_progress(UIState *state)
{
    WINDOW *w = tui.progress;
    werase(w);

    int cols = getmaxx(w);

    char vol_str[24];
    snprintf(vol_str, sizeof(vol_str), " Vol:%d%%", state->volume);
    int vol_len = (int)strlen(vol_str);

    int bar_w = cols - vol_len - 1;
    if (bar_w > 0) {
        double pct = 0.0;
        if (state->song_duration > 0)
            pct = state->song_position / (double)state->song_duration;
        if (pct < 0.0) pct = 0.0;
        if (pct > 1.0) pct = 1.0;

        int filled = (int)(bar_w * pct);
        if (filled > bar_w) filled = bar_w;

        wattron(w, COLOR_PAIR(C_PROGRESS));
        for (int i = 0; i < filled; i++)
            mvwaddch(w, 0, i, '#');
        wattroff(w, COLOR_PAIR(C_PROGRESS));

        wattron(w, COLOR_PAIR(C_DIM) | A_DIM);
        for (int i = filled; i < bar_w; i++)
            mvwaddch(w, 0, i, '-');
        wattroff(w, COLOR_PAIR(C_DIM) | A_DIM);
    }

    if (cols - vol_len >= 0)
        mvwaddnstr(w, 0, cols - vol_len, vol_str, vol_len);

    wnoutrefresh(w);
}

static void draw_left(UIState *state)
{
    WINDOW *w = tui.left;
    werase(w);

    int rows = getmaxy(w);
    int cols = getmaxx(w);

    if (state->mode != MODE_PLAYLIST) {
        mvwaddnstr(w, 0, 0, " [Browser - coming soon]", cols);
        wnoutrefresh(w);
        return;
    }

    wattron(w, A_UNDERLINE | A_BOLD);
    mvwaddnstr(w, 0, 0, " Playlist", cols);
    wattroff(w, A_UNDERLINE | A_BOLD);

    int n = state->playlist ? playlist_count(state->playlist) : 0;
    int current = state->playlist ? playlist_index(state->playlist) : -1;

    for (int i = 0; i < rows - 1 && state->scroll + i < n; i++) {
        int idx = state->scroll + i;
        Track *t = playlist_get(state->playlist, idx);
        if (!t) break;

        int is_current = (idx == current);
        int is_cursor  = (idx == state->cursor);

        char marker = ' ';
        if (is_current) marker = state->paused ? '|' : '>';

        /* Render into a fixed line so the highlight spans the full row
         * instead of stopping at the end of the text. */
        char line[1200];
        snprintf(line, sizeof(line), "%c %.511s - %.511s", marker,
                 t->artist[0] ? t->artist : "?",
                 t->title[0]  ? t->title  : "?");

        char row[1200];
        int width = cols < (int)sizeof(row) - 1 ? cols : (int)sizeof(row) - 1;
        snprintf(row, sizeof(row), "%-*.*s", width, width, line);

        if (is_cursor)  wattron(w, COLOR_PAIR(C_HIGHLIGHT));
        if (is_current) wattron(w, A_BOLD);

        mvwaddnstr(w, 1 + i, 0, row, cols);

        if (is_current) wattroff(w, A_BOLD);
        if (is_cursor)  wattroff(w, COLOR_PAIR(C_HIGHLIGHT));
    }

    wnoutrefresh(w);
}

static const char *const help_lines[] = {
    "Keys",
    "",
    "  space    play / pause",
    "  enter    play selected",
    "  n / p    next / previous",
    "  j / k    move cursor",
    "  < / >    seek back / forward",
    "  + / -    volume",
    "  s        shuffle",
    "  r        repeat mode",
    "  ?        toggle this help",
    "  q        quit",
};

static void draw_right(UIState *state)
{
    WINDOW *w = tui.right;
    werase(w);

    int rows = getmaxy(w);
    int cols = getmaxx(w);

    if (state->show_help) {
        int count = (int)(sizeof(help_lines) / sizeof(help_lines[0]));
        for (int i = 0; i < count && i + 1 < rows; i++)
            mvwaddnstr(w, 1 + i, 2, help_lines[i], cols - 2 > 0 ? cols - 2 : 0);
        wnoutrefresh(w);
        return;
    }

    if (state->playing && state->song_title[0]) {
        if (rows > 1) mvwprintw(w, 1, 2, "Title:   %.*s", cols - 12, state->song_title);
        if (rows > 2) mvwprintw(w, 2, 2, "Artist:  %.*s", cols - 12, state->song_artist);
        if (rows > 3) mvwprintw(w, 3, 2, "Album:   %.*s", cols - 12, state->song_album);
        if (rows > 5) mvwaddnstr(w, 5, 2, "Press ? for keys", cols - 2 > 0 ? cols - 2 : 0);
    } else {
        if (rows > 1) mvwaddnstr(w, 1, 2, "No track selected", cols - 2 > 0 ? cols - 2 : 0);
    }

    wnoutrefresh(w);
}

/* Append to buf without letting snprintf's would-be-length return value run
 * the offset past the end of the buffer. */
static void append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    if (*len >= cap - 1) return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);

    if (n < 0) return;
    *len += (size_t)n;
    if (*len > cap - 1) *len = cap - 1;
}

static void draw_status(UIState *state)
{
    WINDOW *w = tui.status;
    werase(w);

    int cols = getmaxx(w);

    char buf[256];
    size_t len = 0;
    buf[0] = '\0';

    if (state->playing)
        append(buf, sizeof(buf), &len, " %s ",
               state->paused ? "|| Paused" : ">> Playing");
    else
        append(buf, sizeof(buf), &len, " ** Stopped ");

    append(buf, sizeof(buf), &len, "| Shuffle %s ",
           state->shuffle ? "On" : "Off");

    const char *r = "None";
    if (state->repeat == REPEAT_ONE) r = "One";
    if (state->repeat == REPEAT_ALL) r = "All";
    append(buf, sizeof(buf), &len, "| Repeat %s ", r);

    if (state->playlist)
        append(buf, sizeof(buf), &len, "| %d songs ",
               playlist_count(state->playlist));

    append(buf, sizeof(buf), &len, "| Vol:%d%%", state->volume);

    wattron(w, COLOR_PAIR(C_STATUS));
    /* Pad to full width so the status bar reads as a solid bar. */
    for (int i = 0; i < cols; i++)
        mvwaddch(w, 0, i, ' ');
    mvwaddnstr(w, 0, 0, buf, cols);

    if (tui.status_msg[0]) {
        if (time(NULL) - tui.status_time < STATUS_TTL) {
            int msg_len = (int)strlen(tui.status_msg);
            if (msg_len > cols) msg_len = cols;
            int x = cols - msg_len - 1;
            if (x > (int)len)
                mvwaddnstr(w, 0, x, tui.status_msg, msg_len);
        } else {
            tui.status_msg[0] = '\0';
        }
    }
    wattroff(w, COLOR_PAIR(C_STATUS));

    wnoutrefresh(w);
}

void ui_draw(UIState *state)
{
    clamp_view(state);
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
    int n = state->playlist ? playlist_count(state->playlist) : 0;

    switch (ch) {
    case KEY_RESIZE:
        ui_resize();
        return ACTION_NONE;

    case 'q': case 'Q':
        return ACTION_QUIT;

    case ' ':
        if (!state->playing)   return ACTION_PLAY;
        if (state->paused)     return ACTION_RESUME;
        return ACTION_PAUSE;

    case 'n': case 'N': return ACTION_NEXT;
    case 'p': case 'P': return ACTION_PREV;
    case '+': case '=': return ACTION_VOL_UP;
    case '-': case '_': return ACTION_VOL_DOWN;
    case 's': case 'S': return ACTION_TOGGLE_SHUFFLE;
    case 'r': case 'R': return ACTION_CYCLE_REPEAT;
    case '?':           return ACTION_TOGGLE_HELP;

    case '>': case '.': case KEY_RIGHT: return ACTION_SEEK_FWD;
    case '<': case ',': case KEY_LEFT:  return ACTION_SEEK_BACK;

    case 'j': case KEY_DOWN:
        if (state->mode == MODE_PLAYLIST && state->cursor < n - 1)
            state->cursor++;
        break;

    case 'k': case KEY_UP:
        if (state->mode == MODE_PLAYLIST && state->cursor > 0)
            state->cursor--;
        break;

    case KEY_NPAGE:
        if (state->mode == MODE_PLAYLIST && n > 0) {
            int page = tui.left ? getmaxy(tui.left) - 1 : 1;
            if (page < 1) page = 1;
            state->cursor += page;
            if (state->cursor > n - 1) state->cursor = n - 1;
        }
        break;

    case KEY_PPAGE:
        if (state->mode == MODE_PLAYLIST && n > 0) {
            int page = tui.left ? getmaxy(tui.left) - 1 : 1;
            if (page < 1) page = 1;
            state->cursor -= page;
            if (state->cursor < 0) state->cursor = 0;
        }
        break;

    case KEY_HOME: case 'g':
        if (state->mode == MODE_PLAYLIST) state->cursor = 0;
        break;

    case KEY_END: case 'G':
        if (state->mode == MODE_PLAYLIST && n > 0) state->cursor = n - 1;
        break;

    case '\n': case '\r': case KEY_ENTER:
        if (state->mode == MODE_PLAYLIST && n > 0) {
            playlist_set_index(state->playlist, state->cursor);
            return ACTION_PLAY_SELECTED;
        }
        break;
    }

    clamp_view(state);
    return ACTION_NONE;
}

void ui_cleanup(void)
{
    destroy_windows();
    endwin();
}
