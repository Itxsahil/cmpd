#include "audio/output.h"

#include <portaudio.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Ring capacity in seconds of audio. Large enough to ride out scheduler
 * jitter, small enough that a flush discards little work. */
#define RING_SECONDS 1

/* Frames the fill thread asks for per pull. */
#define FILL_CHUNK 8192

#define MAX_CHANNELS 8

struct Output {
    output_fill_cb fill_cb;
    void          *fill_userdata;

    PaStream      *stream;
    int            channels;
    int            sample_rate;

    /* Single-producer/single-consumer ring. The fill thread owns write_pos and
     * the PortAudio callback owns read_pos, both monotonic in frames, so the
     * callback never takes a lock. */
    float             *ring;
    int                cap_frames;
    atomic_ullong      write_pos;
    atomic_ullong      read_pos;

    float             *scratch;

    /* Control handshake between the main thread and the fill thread. Never
     * touched from the audio callback. */
    pthread_mutex_t ctl_lock;
    pthread_cond_t  ctl_cv;
    int             running;
    int             paused;
    int             fill_idle;      /* fill thread parked, safe to flush */
    int             thread_started;
    pthread_t       fill_thread;

    _Atomic float   volume;
};

/* PortAudio is a process-wide singleton, so initialise it once and count
 * users rather than re-initialising on every track. */
static pthread_mutex_t pa_lock = PTHREAD_MUTEX_INITIALIZER;
static int pa_refcount;

static int pa_acquire(void)
{
    pthread_mutex_lock(&pa_lock);
    if (pa_refcount == 0) {
        PaError err = Pa_Initialize();
        if (err != paNoError) {
            pthread_mutex_unlock(&pa_lock);
            return -1;
        }
    }
    pa_refcount++;
    pthread_mutex_unlock(&pa_lock);
    return 0;
}

static void pa_release(void)
{
    pthread_mutex_lock(&pa_lock);
    if (pa_refcount > 0 && --pa_refcount == 0)
        Pa_Terminate();
    pthread_mutex_unlock(&pa_lock);
}

/* ── ring buffer ───────────────────────────────────────────────────────── */

static int ring_available(Output *out)
{
    unsigned long long w = atomic_load_explicit(&out->write_pos,
                                                memory_order_acquire);
    unsigned long long r = atomic_load_explicit(&out->read_pos,
                                                memory_order_acquire);
    return (int)(w - r);
}

/* ── audio callback (real-time; no locks, no allocation) ───────────────── */

static int pa_callback(const void *input, void *output,
                       unsigned long frame_count,
                       const PaStreamCallbackTimeInfo *time,
                       PaStreamCallbackFlags flags, void *userdata)
{
    (void)input; (void)time; (void)flags;

    Output *out = userdata;
    float *dst = output;
    int ch = out->channels;
    int cap = out->cap_frames;

    unsigned long long w = atomic_load_explicit(&out->write_pos,
                                                memory_order_acquire);
    unsigned long long r = atomic_load_explicit(&out->read_pos,
                                                memory_order_relaxed);

    unsigned long long avail = w - r;
    unsigned long to_read = frame_count;
    if (to_read > avail) to_read = (unsigned long)avail;

    float vol = atomic_load_explicit(&out->volume, memory_order_relaxed);

    for (unsigned long i = 0; i < to_read; i++) {
        const float *src = out->ring + (size_t)((r + i) % cap) * ch;
        for (int c = 0; c < ch; c++)
            dst[(size_t)(i * ch) + c] = src[c] * vol;
    }

    atomic_store_explicit(&out->read_pos, r + to_read, memory_order_release);

    /* Underrun: pad with silence rather than repeating stale audio. */
    for (unsigned long i = to_read * ch; i < frame_count * (unsigned long)ch; i++)
        dst[i] = 0.0f;

    return paContinue;
}

/* ── fill thread ───────────────────────────────────────────────────────── */

static void *fill_thread_fn(void *arg)
{
    Output *out = arg;
    int ch = out->channels;
    int cap = out->cap_frames;

    for (;;) {
        pthread_mutex_lock(&out->ctl_lock);
        while (out->running && out->paused) {
            out->fill_idle = 1;
            pthread_cond_broadcast(&out->ctl_cv);
            pthread_cond_wait(&out->ctl_cv, &out->ctl_lock);
        }
        if (!out->running) {
            out->fill_idle = 1;
            pthread_cond_broadcast(&out->ctl_cv);
            pthread_mutex_unlock(&out->ctl_lock);
            break;
        }
        out->fill_idle = 0;
        pthread_mutex_unlock(&out->ctl_lock);

        int space = cap - ring_available(out);
        if (space <= 0) {
            Pa_Sleep(5);
            continue;
        }

        int want = space < FILL_CHUNK ? space : FILL_CHUNK;
        int got = out->fill_cb(out->scratch, want, ch, out->sample_rate,
                               out->fill_userdata);
        if (got <= 0) {
            /* Source is paused or exhausted; let the ring drain. */
            Pa_Sleep(5);
            continue;
        }
        if (got > want) got = want;

        unsigned long long w = atomic_load_explicit(&out->write_pos,
                                                    memory_order_relaxed);
        for (int i = 0; i < got; i++) {
            float *slot = out->ring + (size_t)((w + i) % cap) * ch;
            memcpy(slot, out->scratch + (size_t)i * ch, (size_t)ch * sizeof(float));
        }
        atomic_store_explicit(&out->write_pos, w + got, memory_order_release);
    }

    return NULL;
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

Output *output_open(int channels, int sample_rate,
                    output_fill_cb cb, void *userdata)
{
    if (!cb || channels < 1 || channels > MAX_CHANNELS || sample_rate < 1)
        return NULL;

    Output *out = calloc(1, sizeof(*out));
    if (!out) return NULL;

    out->fill_cb       = cb;
    out->fill_userdata = userdata;
    out->channels      = channels;
    out->sample_rate   = sample_rate;
    out->cap_frames    = sample_rate * RING_SECONDS;
    atomic_init(&out->volume, 1.0f);
    atomic_init(&out->write_pos, 0ULL);
    atomic_init(&out->read_pos, 0ULL);

    /* Paused until output_start(), so the fill thread cannot race ahead. */
    out->paused    = 1;
    out->fill_idle = 1;

    pthread_mutex_init(&out->ctl_lock, NULL);
    pthread_cond_init(&out->ctl_cv, NULL);

    out->ring = calloc((size_t)out->cap_frames * channels, sizeof(float));
    out->scratch = calloc((size_t)FILL_CHUNK * channels, sizeof(float));
    if (!out->ring || !out->scratch) goto fail;

    if (pa_acquire() < 0) goto fail;

    PaDeviceIndex dev = Pa_GetDefaultOutputDevice();
    if (dev == paNoDevice) goto fail_pa;

    const PaDeviceInfo *info = Pa_GetDeviceInfo(dev);
    if (!info) goto fail_pa;

    if (channels > info->maxOutputChannels) goto fail_pa;

    PaStreamParameters param;
    memset(&param, 0, sizeof(param));
    param.device                    = dev;
    param.channelCount              = channels;
    param.sampleFormat              = paFloat32;
    param.suggestedLatency          = info->defaultLowOutputLatency;
    param.hostApiSpecificStreamInfo = NULL;

    if (Pa_OpenStream(&out->stream, NULL, &param, sample_rate,
                      paFramesPerBufferUnspecified, paClipOff,
                      pa_callback, out) != paNoError)
        goto fail_pa;

    return out;

fail_pa:
    pa_release();
fail:
    pthread_cond_destroy(&out->ctl_cv);
    pthread_mutex_destroy(&out->ctl_lock);
    free(out->scratch);
    free(out->ring);
    free(out);
    return NULL;
}

void output_start(Output *out)
{
    if (!out || out->thread_started) return;

    if (Pa_StartStream(out->stream) != paNoError) return;

    pthread_mutex_lock(&out->ctl_lock);
    out->running = 1;
    out->paused  = 0;
    pthread_mutex_unlock(&out->ctl_lock);

    if (pthread_create(&out->fill_thread, NULL, fill_thread_fn, out) != 0) {
        pthread_mutex_lock(&out->ctl_lock);
        out->running = 0;
        pthread_mutex_unlock(&out->ctl_lock);
        Pa_StopStream(out->stream);
        return;
    }
    out->thread_started = 1;
}

void output_pause(Output *out)
{
    if (!out) return;

    pthread_mutex_lock(&out->ctl_lock);
    out->paused = 1;
    pthread_cond_broadcast(&out->ctl_cv);
    /* Wait for the producer to park so a following flush is race-free. */
    while (out->running && !out->fill_idle)
        pthread_cond_wait(&out->ctl_cv, &out->ctl_lock);
    pthread_mutex_unlock(&out->ctl_lock);

    /* Blocks until the audio callback has returned, quiescing the consumer. */
    if (out->stream && Pa_IsStreamActive(out->stream) == 1)
        Pa_StopStream(out->stream);
}

void output_resume(Output *out)
{
    if (!out) return;

    if (out->stream && Pa_IsStreamStopped(out->stream) == 1)
        Pa_StartStream(out->stream);

    pthread_mutex_lock(&out->ctl_lock);
    out->paused = 0;
    pthread_cond_broadcast(&out->ctl_cv);
    pthread_mutex_unlock(&out->ctl_lock);
}

int output_is_paused(Output *out)
{
    if (!out) return 1;
    pthread_mutex_lock(&out->ctl_lock);
    int p = out->paused;
    pthread_mutex_unlock(&out->ctl_lock);
    return p;
}

void output_flush(Output *out)
{
    if (!out) return;
    /* Both sides are quiescent here (see output_pause), so resetting the
     * counters together is safe. */
    atomic_store_explicit(&out->read_pos, 0ULL, memory_order_relaxed);
    atomic_store_explicit(&out->write_pos, 0ULL, memory_order_release);
}

int output_matches(Output *out, int channels, int sample_rate)
{
    return out && out->channels == channels && out->sample_rate == sample_rate;
}

void output_set_volume(Output *out, float vol)
{
    if (!out) return;
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    atomic_store_explicit(&out->volume, vol, memory_order_relaxed);
}

float output_volume(Output *out)
{
    return out ? atomic_load_explicit(&out->volume, memory_order_relaxed) : 0.0f;
}

int output_queued_frames(Output *out)
{
    return out ? ring_available(out) : 0;
}

void output_close(Output *out)
{
    if (!out) return;

    pthread_mutex_lock(&out->ctl_lock);
    out->running = 0;
    out->paused  = 0;
    pthread_cond_broadcast(&out->ctl_cv);
    pthread_mutex_unlock(&out->ctl_lock);

    /* Only join a thread that was actually created. */
    if (out->thread_started)
        pthread_join(out->fill_thread, NULL);

    if (out->stream) {
        if (Pa_IsStreamActive(out->stream) == 1)
            Pa_StopStream(out->stream);
        Pa_CloseStream(out->stream);
    }
    pa_release();

    pthread_cond_destroy(&out->ctl_cv);
    pthread_mutex_destroy(&out->ctl_lock);
    free(out->scratch);
    free(out->ring);
    free(out);
}
