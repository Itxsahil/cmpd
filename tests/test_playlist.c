/* Playlist ordering, repeat modes and shuffle. Pure logic, no audio. */
#include "core/playlist.h"
#include "test_util.h"

static Track make_track(const char *title)
{
    Track t;
    memset(&t, 0, sizeof(t));
    snprintf(t.path,  sizeof(t.path),  "/tmp/%s.wav", title);
    snprintf(t.title, sizeof(t.title), "%s", title);
    return t;
}

static Playlist *build(int n)
{
    Playlist *pl = playlist_create();
    for (int i = 0; i < n; i++) {
        char name[32];
        snprintf(name, sizeof(name), "t%d", i);
        Track t = make_track(name);
        if (playlist_add(pl, &t) != 0) return NULL;
    }
    return pl;
}

static void test_add_and_index(void)
{
    Playlist *pl = playlist_create();
    CHECK(pl != NULL);
    CHECK_EQ(playlist_count(pl), 0);
    CHECK_EQ(playlist_index(pl), -1);
    CHECK(playlist_current(pl) == NULL);

    Track t = make_track("first");
    CHECK_EQ(playlist_add(pl, &t), 0);
    CHECK_EQ(playlist_count(pl), 1);
    /* The first add selects the track. */
    CHECK_EQ(playlist_index(pl), 0);
    CHECK_STR(playlist_current(pl)->title, "first");

    /* Out-of-range access must not fault. */
    CHECK(playlist_get(pl, -1) == NULL);
    CHECK(playlist_get(pl, 1) == NULL);
    playlist_set_index(pl, 99);
    CHECK_EQ(playlist_index(pl), 0);

    playlist_destroy(pl);
}

/* Growth past the initial capacity must preserve every entry. */
static void test_growth(void)
{
    Playlist *pl = build(1000);
    CHECK(pl != NULL);
    CHECK_EQ(playlist_count(pl), 1000);
    CHECK_STR(playlist_get(pl, 0)->title, "t0");
    CHECK_STR(playlist_get(pl, 999)->title, "t999");
    playlist_destroy(pl);
}

static void test_repeat_none(void)
{
    Playlist *pl = build(3);
    playlist_set_repeat(pl, REPEAT_NONE);
    playlist_set_index(pl, 0);

    CHECK_EQ(playlist_next(pl), 1);
    CHECK_EQ(playlist_next(pl), 2);
    /* At the end, next reports "no more" and stays put. */
    CHECK_EQ(playlist_next(pl), -1);
    CHECK_EQ(playlist_index(pl), 2);

    playlist_set_index(pl, 0);
    CHECK_EQ(playlist_prev(pl), -1);
    CHECK_EQ(playlist_index(pl), 0);

    playlist_destroy(pl);
}

static void test_repeat_all(void)
{
    Playlist *pl = build(3);
    playlist_set_repeat(pl, REPEAT_ALL);
    playlist_set_index(pl, 2);

    CHECK_EQ(playlist_next(pl), 0);     /* wraps forward */
    CHECK_EQ(playlist_prev(pl), 2);     /* wraps backward */

    playlist_destroy(pl);
}

static void test_repeat_one(void)
{
    Playlist *pl = build(3);
    playlist_set_repeat(pl, REPEAT_ONE);
    playlist_set_index(pl, 1);

    CHECK_EQ(playlist_next(pl), 1);
    CHECK_EQ(playlist_prev(pl), 1);
    CHECK_EQ(playlist_index(pl), 1);

    playlist_destroy(pl);
}

/* Shuffle must visit every track exactly once per cycle. */
static void test_shuffle_covers_all(void)
{
    const int n = 50;
    Playlist *pl = build(n);
    playlist_set_repeat(pl, REPEAT_ALL);
    playlist_set_shuffle(pl, 1);
    CHECK_EQ(playlist_shuffle(pl), 1);

    playlist_set_index(pl, 0);

    int seen[50];
    memset(seen, 0, sizeof(seen));
    seen[playlist_index(pl)] = 1;

    for (int i = 0; i < n - 1; i++) {
        int idx = playlist_next(pl);
        CHECK(idx >= 0 && idx < n);
        if (idx >= 0 && idx < n) seen[idx]++;
    }

    int missing = 0, repeated = 0;
    for (int i = 0; i < n; i++) {
        if (seen[i] == 0) missing++;
        if (seen[i] > 1) repeated++;
    }
    CHECK_EQ(missing, 0);
    CHECK_EQ(repeated, 0);

    playlist_destroy(pl);
}

static void test_shuffle_toggle_off(void)
{
    Playlist *pl = build(4);
    playlist_set_shuffle(pl, 1);
    playlist_set_shuffle(pl, 0);
    CHECK_EQ(playlist_shuffle(pl), 0);

    playlist_set_repeat(pl, REPEAT_ALL);
    playlist_set_index(pl, 0);
    CHECK_EQ(playlist_next(pl), 1);
    playlist_destroy(pl);
}

static void test_remove_and_clear(void)
{
    Playlist *pl = build(3);

    playlist_remove(pl, 1);
    CHECK_EQ(playlist_count(pl), 2);
    CHECK_STR(playlist_get(pl, 1)->title, "t2");

    /* Removing out of range is a no-op, not a fault. */
    playlist_remove(pl, 99);
    playlist_remove(pl, -1);
    CHECK_EQ(playlist_count(pl), 2);

    playlist_set_index(pl, 1);
    playlist_remove(pl, 1);
    CHECK_EQ(playlist_index(pl), 0);

    playlist_clear(pl);
    CHECK_EQ(playlist_count(pl), 0);
    CHECK_EQ(playlist_index(pl), -1);

    /* Navigating an empty list must be safe. */
    CHECK_EQ(playlist_next(pl), -1);
    CHECK_EQ(playlist_prev(pl), -1);

    playlist_destroy(pl);
}

static void test_null_safety(void)
{
    CHECK_EQ(playlist_count(NULL), 0);
    CHECK_EQ(playlist_index(NULL), -1);
    CHECK(playlist_get(NULL, 0) == NULL);
    CHECK(playlist_current(NULL) == NULL);
    CHECK_EQ(playlist_next(NULL), -1);
    CHECK_EQ(playlist_prev(NULL), -1);
    CHECK_EQ(playlist_add(NULL, NULL), -1);
    CHECK_EQ(playlist_shuffle(NULL), 0);
    CHECK_EQ(playlist_repeat(NULL), REPEAT_NONE);
    playlist_remove(NULL, 0);
    playlist_clear(NULL);
    playlist_set_index(NULL, 0);
    playlist_set_shuffle(NULL, 1);
    playlist_set_repeat(NULL, REPEAT_ALL);
    playlist_destroy(NULL);
}

int main(void)
{
    test_add_and_index();
    test_growth();
    test_repeat_none();
    test_repeat_all();
    test_repeat_one();
    test_shuffle_covers_all();
    test_shuffle_toggle_off();
    test_remove_and_clear();
    test_null_safety();
    return TEST_REPORT("playlist");
}
