#ifndef OUTPUT_H
#define OUTPUT_H

typedef struct Output Output;

/* Called on the fill thread to pull interleaved float frames. Must return the
 * number of frames written, 0 when none are available, or negative on error. */
typedef int (*output_fill_cb)(float *buf, int frames, int channels,
                              int sample_rate, void *userdata);

Output *output_open(int channels, int sample_rate, output_fill_cb cb,
                    void *userdata);
void    output_start(Output *out);
void    output_pause(Output *out);
void    output_resume(Output *out);
int     output_is_paused(Output *out);
void    output_set_volume(Output *out, float vol);
float   output_volume(Output *out);
void    output_close(Output *out);

/* Frames buffered but not yet handed to the audio device. */
int     output_queued_frames(Output *out);

/* True when the stream is already running at this format, so a new track can
 * reuse it instead of tearing the device down. */
int     output_matches(Output *out, int channels, int sample_rate);

/* Discard buffered audio. Only valid while paused, which output_pause()
 * guarantees by parking the fill thread before it returns. */
void    output_flush(Output *out);

#endif
