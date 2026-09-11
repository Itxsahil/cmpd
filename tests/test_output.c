/* Output ring accounting and lifecycle. Skips the device-backed checks when
 * the machine has no audio output, so the suite still runs headless. */
#include "audio/output.h"
#include "test_util.h"

#include <portaudio.h>
#include <stdatomic.h>

#define CHANNELS    2
#define SAMPLE_RATE 44100

static atomic_llong frames_produced;

/* Produces a ramp so a ring that loses or duplicates frames shows up. */
static int ramp_fill(float *buf, int frames, int channels, int sample_rate,
                     void *userdata)
{
    (void)sample_rate; (void)userdata;

    long long base = atomic_load(&frames_produced);
    for (int i = 0; i < frames; i++)
        for (int c = 0; c < channels; c++)
            buf[(size_t)i * channels + c] = (float)((base + i) % 1000) / 1000.0f;

    atomic_fetch_add(&frames_produced, frames);
    return frames;
}

static int have_device(void)
{
    if (Pa_Initialize() != paNoError) return 0;
    int ok = Pa_GetDefaultOutputDevice() != paNoDevice;
    Pa_Terminate();
    return ok;
}

static void test_rejects_bad_parameters(void)
{
    CHECK(output_open(0, SAMPLE_RATE, ramp_fill, NULL) == NULL);
    CHECK(output_open(-1, SAMPLE_RATE, ramp_fill, NULL) == NULL);
    CHECK(output_open(999, SAMPLE_RATE, ramp_fill, NULL) == NULL);
    CHECK(output_open(CHANNELS, 0, ramp_fill, NULL) == NULL);
    CHECK(output_open(CHANNELS, SAMPLE_RATE, NULL, NULL) == NULL);
}

static void test_null_safety(void)
{
    CHECK_EQ(output_queued_frames(NULL), 0);
    CHECK_EQ(output_is_paused(NULL), 1);
    CHECK_EQ(output_matches(NULL, CHANNELS, SAMPLE_RATE), 0);
    CHECK_NEAR(output_volume(NULL), 0.0f, 0.0001);
    output_set_volume(NULL, 0.5f);
    output_start(NULL);
    output_pause(NULL);
    output_resume(NULL);
    output_flush(NULL);
    output_close(NULL);
}

/* Closing without ever starting must not join a thread that was never made. */
static void test_close_without_start(void)
{
    Output *out = output_open(CHANNELS, SAMPLE_RATE, ramp_fill, NULL);
    CHECK(out != NULL);
    if (!out) return;
    CHECK_EQ(output_is_paused(out), 1);
    CHECK_EQ(output_queued_frames(out), 0);
    output_close(out);
}

static void test_volume_clamped(void)
{
    Output *out = output_open(CHANNELS, SAMPLE_RATE, ramp_fill, NULL);
    CHECK(out != NULL);
    if (!out) return;

    output_set_volume(out, 0.25f);
    CHECK_NEAR(output_volume(out), 0.25f, 0.0001);
    output_set_volume(out, 5.0f);
    CHECK_NEAR(output_volume(out), 1.0f, 0.0001);
    output_set_volume(out, -3.0f);
    CHECK_NEAR(output_volume(out), 0.0f, 0.0001);

    output_close(out);
}

static void test_format_match(void)
{
    Output *out = output_open(CHANNELS, SAMPLE_RATE, ramp_fill, NULL);
    CHECK(out != NULL);
    if (!out) return;

    CHECK_EQ(output_matches(out, CHANNELS, SAMPLE_RATE), 1);
    CHECK_EQ(output_matches(out, 1, SAMPLE_RATE), 0);
    CHECK_EQ(output_matches(out, CHANNELS, 48000), 0);

    output_close(out);
}

/* The ring must never report more frames than it can hold. The original code
 * measured free space in samples but the fill level in frames, which let a
 * stereo stream write twice the capacity. */
static void test_queue_never_exceeds_capacity(void)
{
    atomic_store(&frames_produced, 0);

    Output *out = output_open(CHANNELS, SAMPLE_RATE, ramp_fill, NULL);
    CHECK(out != NULL);
    if (!out) return;

    output_start(out);
    CHECK_EQ(output_is_paused(out), 0);

    int worst = 0;
    for (int i = 0; i < 60; i++) {
        int q = output_queued_frames(out);
        CHECK(q >= 0);
        if (q > worst) worst = q;
        Pa_Sleep(10);
    }

    /* One second of audio is the declared capacity. */
    CHECK(worst <= SAMPLE_RATE);
    CHECK(atomic_load(&frames_produced) > 0);

    output_close(out);
}

static void test_pause_flush_resume(void)
{
    atomic_store(&frames_produced, 0);

    Output *out = output_open(CHANNELS, SAMPLE_RATE, ramp_fill, NULL);
    CHECK(out != NULL);
    if (!out) return;

    output_start(out);
    Pa_Sleep(150);

    output_pause(out);
    CHECK_EQ(output_is_paused(out), 1);

    /* Flushing while paused must empty the ring and stay consistent. */
    output_flush(out);
    CHECK_EQ(output_queued_frames(out), 0);

    output_resume(out);
    CHECK_EQ(output_is_paused(out), 0);
    Pa_Sleep(150);
    CHECK(output_queued_frames(out) >= 0);

    output_close(out);
}

/* Repeated open/close cycles must not exhaust PortAudio's global init. */
static void test_repeated_open_close(void)
{
    for (int i = 0; i < 8; i++) {
        Output *out = output_open(CHANNELS, SAMPLE_RATE, ramp_fill, NULL);
        CHECK(out != NULL);
        if (!out) return;
        output_start(out);
        Pa_Sleep(20);
        output_close(out);
    }
}

int main(void)
{
    test_null_safety();

    if (!have_device()) {
        printf("%-16s no audio device; device checks skipped\n", "output");
        return TEST_REPORT("output");
    }

    test_rejects_bad_parameters();
    test_close_without_start();
    test_volume_clamped();
    test_format_match();
    test_queue_never_exceeds_capacity();
    test_pause_flush_resume();
    test_repeated_open_close();

    return TEST_REPORT("output");
}
