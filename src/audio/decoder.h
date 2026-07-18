#ifndef DECODER_H
#define DECODER_H

#include <stddef.h>

typedef struct Decoder Decoder;

Decoder *decoder_open(const char *path);
int      decoder_channels(Decoder *dec);
int      decoder_sample_rate(Decoder *dec);
double   decoder_duration(Decoder *dec);
double   decoder_position(Decoder *dec);
int      decoder_read(Decoder *dec, float *buf, int max_frames);
int      decoder_eof(Decoder *dec);
int      decoder_seek(Decoder *dec, double seconds);
void     decoder_close(Decoder *dec);

#endif
