#include <ncurses.h>
#include <locale.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>

static volatile sig_atomic_t running = 1;

static void cleanup(void)
{
    endwin();
}

static void handle_sigint(int sig)
{
    (void)sig;
    running = 0;
}

int main(void)
{
    setlocale(LC_ALL, "");
    initscr();
    atexit(cleanup);
    signal(SIGINT, handle_sigint);

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    mvaddstr(0, 0, "cmpd — C Music Player Demonic");
    mvaddstr(1, 0, "Press 'q' to quit.");
    refresh();

    while (running) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q')
            break;
    }

    return 0;
}
