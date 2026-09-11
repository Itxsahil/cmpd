/* Decoder frame accounting, channel handling and seeking. */
#include "audio/decoder.h"
#include "test_util.h"

#include <libavutil/log.h>

#define TMP_DIR "/tmp"

static void path_for(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s/cmpd_test_%s.wav", TMP_DIR, name);
}

/* Decode a whole file and return the total frame count, checking that no
 * sample is left behind and that the tail is reported exactly once. */
static long decode_all(Decoder *d, int chunk)
{
    int ch = decoder_channels(d);
    float *buf = malloc((size_t)chunk * ch * sizeof(float));
    if (!buf) return -1;

    long total = 0;
    int guard = 0;
    while (!decoder_eof(d) && guard++ < 1000000) {
        int got = decoder_read(d, buf, chunk);
        if (got < 0) { free(buf); return -1; }
        total += got;
    }

    free(buf);
    return total;
}

/* A stereo file is the case the packed/planar bug crashed on. */
static void test_stereo_exact_frames(void)
{
    char path[256];
    path_for(path, sizeof(path), "stereo");
    CHECK_EQ(write_test_wav(path, 2, 44100, 44100), 0);

    Decoder *d = decoder_open(path);
    CHECK(d != NULL);
    if (!d) return;

    CHECK_EQ(decoder_channels(d), 2);
    CHECK_EQ(decoder_sample_rate(d), 44100);
    CHECK_NEAR(decoder_duration(d), 1.0, 0.05);

    CHECK_EQ(decode_all(d, 1024), 44100);
    CHECK_EQ(decoder_eof(d), 1);
    /* Reading past EOF is safe and yields nothing. */
    float tail[64];
    CHECK_EQ(decoder_read(d, tail, 32), 0);

    decoder_close(d);
    remove(path);
}

static void test_mono(void)
{
    char path[256];
    path_for(path, sizeof(path), "mono");
    CHECK_EQ(write_test_wav(path, 1, 48000, 24000), 0);

    Decoder *d = decoder_open(path);
    CHECK(d != NULL);
    if (!d) return;

    CHECK_EQ(decoder_channels(d), 1);
    CHECK_EQ(decode_all(d, 777), 24000);

    decoder_close(d);
    remove(path);
}

/* Multichannel exercises the per-channel interleaving path. */
static void test_multichannel(void)
{
    char path[256];
    path_for(path, sizeof(path), "six");
    CHECK_EQ(write_test_wav(path, 6, 44100, 10000), 0);

    Decoder *d = decoder_open(path);
    CHECK(d != NULL);
    if (!d) return;

    CHECK_EQ(decoder_channels(d), 6);
    CHECK_EQ(decode_all(d, 512), 10000);

    decoder_close(d);
    remove(path);
}

/* An odd chunk size must not drop the remainder swr produces. */
static void test_chunk_sizes_agree(void)
{
    char path[256];
    path_for(path, sizeof(path), "chunks");
    CHECK_EQ(write_test_wav(path, 2, 44100, 33333), 0);

    int sizes[] = { 1, 17, 512, 4096, 65536 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        Decoder *d = decoder_open(path);
        CHECK(d != NULL);
        if (!d) continue;
        CHECK_EQ(decode_all(d, sizes[i]), 33333);
        decoder_close(d);
    }

    remove(path);
}

static void test_position_tracks_output(void)
{
    char path[256];
    path_for(path, sizeof(path), "pos");
    CHECK_EQ(write_test_wav(path, 2, 44100, 44100 * 4), 0);

    Decoder *d = decoder_open(path);
    CHECK(d != NULL);
    if (!d) return;

    CHECK_NEAR(decoder_position(d), 0.0, 0.001);

    float buf[4096 * 2];
    long total = 0;
    for (int i = 0; i < 10; i++)
        total += decoder_read(d, buf, 4096);

    CHECK_NEAR(decoder_position(d), (double)total / 44100.0, 0.001);

    decoder_close(d);
    remove(path);
}

static void test_seek(void)
{
    char path[256];
    path_for(path, sizeof(path), "seek");
    CHECK_EQ(write_test_wav(path, 2, 44100, 44100 * 10), 0);

    Decoder *d = decoder_open(path);
    CHECK(d != NULL);
    if (!d) return;

    CHECK_EQ(decoder_seek(d, 5.0), 0);
    CHECK_NEAR(decoder_position(d), 5.0, 0.2);
    CHECK_EQ(decoder_eof(d), 0);

    float buf[1024 * 2];
    CHECK(decoder_read(d, buf, 1024) > 0);

    /* Seeking back to the start must clear EOF and resume decoding. */
    CHECK_EQ(decoder_seek(d, 0.0), 0);
    CHECK_NEAR(decoder_position(d), 0.0, 0.05);
    CHECK_EQ(decode_all(d, 4096), 44100 * 10);
    CHECK_EQ(decoder_eof(d), 1);

    CHECK_EQ(decoder_seek(d, 1.0), 0);
    CHECK_EQ(decoder_eof(d), 0);

    /* Negative and past-the-end targets are clamped, not errors. */
    CHECK_EQ(decoder_seek(d, -50.0), 0);
    CHECK_NEAR(decoder_position(d), 0.0, 0.05);
    CHECK_EQ(decoder_seek(d, 99999.0), 0);

    decoder_close(d);
    remove(path);
}

static void test_bad_input(void)
{
    CHECK(decoder_open("/nonexistent/nope.wav") == NULL);
    CHECK(decoder_open(NULL) == NULL);

    /* A text file is not audio. */
    char path[256];
    snprintf(path, sizeof(path), "%s/cmpd_test_notaudio.txt", TMP_DIR);
    FILE *f = fopen(path, "w");
    if (f) { fputs("this is not audio at all\n", f); fclose(f); }
    CHECK(decoder_open(path) == NULL);
    remove(path);

    /* Accessors and close must tolerate NULL. */
    CHECK_EQ(decoder_channels(NULL), 0);
    CHECK_EQ(decoder_sample_rate(NULL), 0);
    CHECK_EQ(decoder_eof(NULL), 1);
    CHECK_EQ(decoder_seek(NULL, 1.0), -1);
    CHECK_EQ(decoder_read(NULL, NULL, 0), 0);
    decoder_close(NULL);
}

int main(void)
{
    av_log_set_level(AV_LOG_QUIET);

    test_stereo_exact_frames();
    test_mono();
    test_multichannel();
    test_chunk_sizes_agree();
    test_position_tracks_output();
    test_seek();
    test_bad_input();
    return TEST_REPORT("decoder");
}
