#ifndef DECODER_H
#define DECODER_H

#include <stddef.h>

typedef struct Decoder Decoder;

typedef void (*decoder_cb)(float *samples, int num_frames,
                           int channels, int sample_rate, void *userdata);

Decoder *decoder_open(const char *path);
int      decoder_channels(Decoder *dec);
int      decoder_sample_rate(Decoder *dec);
double   decoder_duration(Decoder *dec);
int      decoder_decode(Decoder *dec, decoder_cb cb, void *userdata);
int      decoder_seek(Decoder *dec, double seconds);
void     decoder_close(Decoder *dec);

#endif
