/* Tag reading: field population, fallbacks and free-safety. */
#include "library/tags.h"
#include "test_util.h"

static void test_untagged_wav(void)
{
    char path[256];
    snprintf(path, sizeof(path), "/tmp/cmpd_test_tags.wav");
    CHECK_EQ(write_test_wav(path, 2, 44100, 44100 * 3), 0);

    SongInfo info;
    CHECK_EQ(tags_read(path, &info), 0);

    /* Strings are never NULL on success, even with no tags present. */
    CHECK(info.title  != NULL);
    CHECK(info.artist != NULL);
    CHECK(info.album  != NULL);
    CHECK(info.genre  != NULL);
    CHECK_EQ(info.duration, 3);
    CHECK(info.cover_data == NULL);
    CHECK_EQ(info.cover_size, 0);

    tags_free(&info);
    /* tags_free clears the struct, so a second call is harmless. */
    CHECK(info.title == NULL);
    tags_free(&info);

    remove(path);
}

static void test_bad_input(void)
{
    SongInfo info;
    CHECK_EQ(tags_read("/nonexistent/nope.mp3", &info), -1);
    CHECK_EQ(tags_read(NULL, &info), -1);
    CHECK_EQ(tags_read("/tmp", NULL), -1);
    tags_free(NULL);
}

int main(void)
{
    test_untagged_wav();
    test_bad_input();
    return TEST_REPORT("tags");
}
