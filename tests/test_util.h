#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not every test file uses every helper here. */
#if defined(__GNUC__)
#  define TEST_MAYBE_UNUSED __attribute__((unused))
#else
#  define TEST_MAYBE_UNUSED
#endif

static int tests_run;
static int tests_failed;

#define CHECK(cond) do {                                                    \
    tests_run++;                                                            \
    if (!(cond)) {                                                          \
        tests_failed++;                                                     \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                       \
} while (0)

#define CHECK_EQ(a, b) do {                                                 \
    long long _a = (long long)(a), _b = (long long)(b);                     \
    tests_run++;                                                            \
    if (_a != _b) {                                                         \
        tests_failed++;                                                     \
        fprintf(stderr, "  FAIL %s:%d: %s == %s (%lld vs %lld)\n",          \
                __FILE__, __LINE__, #a, #b, _a, _b);                        \
    }                                                                       \
} while (0)

#define CHECK_STR(a, b) do {                                                \
    tests_run++;                                                            \
    if (strcmp((a), (b)) != 0) {                                            \
        tests_failed++;                                                     \
        fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n",                 \
                __FILE__, __LINE__, (a), (b));                              \
    }                                                                       \
} while (0)

#define CHECK_NEAR(a, b, eps) do {                                          \
    double _a = (double)(a), _b = (double)(b);                              \
    tests_run++;                                                            \
    if (_a - _b > (eps) || _b - _a > (eps)) {                               \
        tests_failed++;                                                     \
        fprintf(stderr, "  FAIL %s:%d: %s ~= %s (%f vs %f)\n",              \
                __FILE__, __LINE__, #a, #b, _a, _b);                        \
    }                                                                       \
} while (0)

#define TEST_REPORT(name)                                                   \
    (printf("%-16s %d checks, %d failed\n", (name), tests_run, tests_failed), \
     tests_failed == 0 ? 0 : 1)

/* Write a 16-bit PCM WAV of `frames` frames so tests need no fixture files
 * and no external tools. Returns 0 on success. */
TEST_MAYBE_UNUSED
static int write_test_wav(const char *path, int channels, int sample_rate,
                          int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int bits = 16;
    int block_align = channels * bits / 8;
    int data_bytes = frames * block_align;

    unsigned char hdr[44];
    memcpy(hdr, "RIFF", 4);
    unsigned riff = 36u + (unsigned)data_bytes;
    hdr[4] = riff & 0xff;        hdr[5] = (riff >> 8) & 0xff;
    hdr[6] = (riff >> 16) & 0xff; hdr[7] = (riff >> 24) & 0xff;
    memcpy(hdr + 8, "WAVEfmt ", 8);
    hdr[16] = 16; hdr[17] = hdr[18] = hdr[19] = 0;   /* fmt chunk size */
    hdr[20] = 1;  hdr[21] = 0;                       /* PCM */
    hdr[22] = (unsigned char)channels; hdr[23] = 0;
    hdr[24] = sample_rate & 0xff;         hdr[25] = (sample_rate >> 8) & 0xff;
    hdr[26] = (sample_rate >> 16) & 0xff; hdr[27] = (sample_rate >> 24) & 0xff;
    unsigned byte_rate = (unsigned)sample_rate * (unsigned)block_align;
    hdr[28] = byte_rate & 0xff;         hdr[29] = (byte_rate >> 8) & 0xff;
    hdr[30] = (byte_rate >> 16) & 0xff; hdr[31] = (byte_rate >> 24) & 0xff;
    hdr[32] = (unsigned char)block_align; hdr[33] = 0;
    hdr[34] = (unsigned char)bits; hdr[35] = 0;
    memcpy(hdr + 36, "data", 4);
    hdr[40] = data_bytes & 0xff;         hdr[41] = (data_bytes >> 8) & 0xff;
    hdr[42] = (data_bytes >> 16) & 0xff; hdr[43] = (data_bytes >> 24) & 0xff;

    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return -1; }

    /* A ramp rather than silence, so a decoder that drops or duplicates
     * samples produces visibly wrong values. */
    for (int i = 0; i < frames; i++) {
        for (int c = 0; c < channels; c++) {
            int v = (i % 1000) * 32 - 16000;
            unsigned char s[2] = { (unsigned char)(v & 0xff),
                                   (unsigned char)((v >> 8) & 0xff) };
            if (fwrite(s, 1, 2, f) != 2) { fclose(f); return -1; }
        }
    }

    fclose(f);
    return 0;
}

#endif
