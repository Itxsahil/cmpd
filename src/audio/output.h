#ifndef OUTPUT_H
#define OUTPUT_H

typedef struct Output Output;

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
int     output_queued_frames(Output *out);

#endif
