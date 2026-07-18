#include "audio/output.h"

#include <portaudio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ring buffer capacity in samples (interleaved) */
#define RING_CAPACITY (512 * 1024)

struct Output {
    output_fill_cb fill_cb;
    void          *fill_userdata;

    PaStream      *stream;
    int            channels;
    int            sample_rate;

    float         *ring;
    int            ring_wi;
    int            ring_ri;
    int            ring_frames;
    pthread_mutex_t ring_lock;

    pthread_t      fill_thread;
    int            running;
    int            paused;

    float          volume;
};

static int pa_callback(const void *input, void *output,
                       unsigned long frame_count,
                       const PaStreamCallbackTimeInfo *time,
                       PaStreamCallbackFlags flags, void *userdata)
{
    (void)input; (void)time; (void)flags;
    Output *out = userdata;
    float *out_buf = output;

    pthread_mutex_lock(&out->ring_lock);

    int available = out->ring_frames;
    int to_read = frame_count;
    if (to_read > available) to_read = available;

    int ri = out->ring_ri;
    int cap = RING_CAPACITY;
    int ch = out->channels;

    for (int i = 0; i < to_read * ch; i++) {
        out_buf[i] = out->ring[ri] * out->volume;
        ri = (ri + 1) % cap;
    }
    out->ring_ri = ri;
    out->ring_frames -= to_read;

    pthread_mutex_unlock(&out->ring_lock);

    for (int i = to_read * ch; i < (int)(frame_count * ch); i++)
        out_buf[i] = 0.0f;

    return paContinue;
}

static void *fill_thread_fn(void *arg)
{
    Output *out = arg;
    /* 4096 frames per fill call */
    float buf[4096 * 8]; /* up to 8 channels */

    while (out->running) {
        if (out->paused) {
            Pa_Sleep(50);
            continue;
        }

        int max_frames = sizeof(buf) / sizeof(float) / out->channels;
        int got = out->fill_cb(buf, max_frames, out->channels,
                               out->sample_rate, out->fill_userdata);
        if (got <= 0) {
            Pa_Sleep(10);
            continue;
        }

        pthread_mutex_lock(&out->ring_lock);

        int cap = RING_CAPACITY;
        int space = cap - out->ring_frames;
        int can_write = got;
        if (can_write > space) can_write = space;

        int wi = out->ring_wi;
        int n = can_write * out->channels;
        for (int i = 0; i < n; i++) {
            out->ring[wi] = buf[i];
            wi = (wi + 1) % cap;
        }
        out->ring_wi = wi;
        out->ring_frames += can_write;

        pthread_mutex_unlock(&out->ring_lock);
    }
    return NULL;
}

Output *output_open(int channels, int sample_rate,
                    output_fill_cb cb, void *userdata)
{
    Output *out = calloc(1, sizeof(*out));
    if (!out) return NULL;

    out->fill_cb      = cb;
    out->fill_userdata = userdata;
    out->channels     = channels;
    out->sample_rate  = sample_rate;
    out->volume       = 1.0f;
    out->paused       = 1;

    pthread_mutex_init(&out->ring_lock, NULL);

    out->ring = calloc(RING_CAPACITY, sizeof(float));
    if (!out->ring) { free(out); return NULL; }

    PaError err = Pa_Initialize();
    if (err != paNoError) { free(out->ring); free(out); return NULL; }

    PaStreamParameters param;
    param.device                    = Pa_GetDefaultOutputDevice();
    param.channelCount              = channels;
    param.sampleFormat              = paFloat32;
    param.suggestedLatency          = Pa_GetDeviceInfo(param.device)->defaultLowOutputLatency;
    param.hostApiSpecificStreamInfo = NULL;

    err = Pa_OpenStream(&out->stream, NULL, &param,
                        sample_rate, paFramesPerBufferUnspecified,
                        paClipOff, pa_callback, out);
    if (err != paNoError) {
        Pa_Terminate();
        free(out->ring);
        free(out);
        return NULL;
    }

    return out;
}

void output_start(Output *out)
{
    if (!out || out->running) return;
    out->running = 1;
    out->paused  = 0;
    Pa_StartStream(out->stream);
    pthread_create(&out->fill_thread, NULL, fill_thread_fn, out);
}

void output_pause(Output *out)
{
    if (out) out->paused = 1;
}

void output_resume(Output *out)
{
    if (out) out->paused = 0;
}

int output_is_paused(Output *out)
{
    return out ? out->paused : 1;
}

void output_set_volume(Output *out, float vol)
{
    if (out) {
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        out->volume = vol;
    }
}

float output_volume(Output *out)
{
    return out ? out->volume : 0.0f;
}

void output_close(Output *out)
{
    if (!out) return;
    out->running = 0;
    pthread_join(out->fill_thread, NULL);
    Pa_CloseStream(out->stream);
    Pa_Terminate();
    pthread_mutex_destroy(&out->ring_lock);
    free(out->ring);
    free(out);
}

int output_queued_frames(Output *out)
{
    if (!out) return 0;
    pthread_mutex_lock(&out->ring_lock);
    int n = out->ring_frames;
    pthread_mutex_unlock(&out->ring_lock);
    return n;
}
