#include "audio/decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Output is always interleaved float, which is what PortAudio's paFloat32
 * expects. DECODER_MAX_CHANNELS bounds what callers must size buffers for. */
#define DECODER_MAX_CHANNELS 8

/* Give up on a stream after this many consecutive undecodable packets rather
 * than spinning forever on a corrupt file. */
#define MAX_DECODE_ERRORS 64

struct Decoder {
    AVFormatContext *fmt_ctx;
    AVCodecContext  *codec_ctx;
    SwrContext      *swr;
    AVPacket        *pkt;
    AVFrame         *frame;

    int              stream_idx;
    int              channels;
    int              sample_rate;
    double           duration;

    /* Frames handed to the caller since the last seek, plus the seek offset.
     * decoder_position() is derived from this. */
    int64_t          frames_out;
    int64_t          seek_offset;

    /* Interleaved float staging buffer. swr_convert can produce more frames
     * than the caller asked for, so the remainder is held here for the next
     * decoder_read() instead of being discarded. */
    float           *conv;
    int              conv_capacity;   /* in frames */
    int              conv_head;       /* frames already consumed */
    int              conv_frames;     /* frames currently held */

    int              sent_eof;        /* flush packet handed to the decoder */
    int              drained;         /* decoder reported AVERROR_EOF */
    int              errors;
};

/* ── conversion buffer ─────────────────────────────────────────────────── */

static int conv_reserve(Decoder *dec, int frames)
{
    if (frames <= dec->conv_capacity) return 0;

    int cap = dec->conv_capacity ? dec->conv_capacity : 4096;
    while (cap < frames) {
        if (cap > INT_MAX / 2) return -1;
        cap *= 2;
    }

    float *p = realloc(dec->conv, (size_t)cap * dec->channels * sizeof(float));
    if (!p) return -1;
    dec->conv = p;
    dec->conv_capacity = cap;
    return 0;
}

/* Move up to max_frames staged frames into buf. Returns frames copied. */
static int conv_drain(Decoder *dec, float *buf, int max_frames)
{
    int avail = dec->conv_frames - dec->conv_head;
    if (avail <= 0 || max_frames <= 0) return 0;

    int n = avail < max_frames ? avail : max_frames;
    memcpy(buf, dec->conv + (size_t)dec->conv_head * dec->channels,
           (size_t)n * dec->channels * sizeof(float));

    dec->conv_head += n;
    if (dec->conv_head >= dec->conv_frames)
        dec->conv_head = dec->conv_frames = 0;

    dec->frames_out += n;
    return n;
}

/* Run swr over one frame (or NULL to flush its internal delay) and stage the
 * result. Returns 0 on success, -1 on an unrecoverable error. */
static int conv_fill(Decoder *dec, const AVFrame *frame)
{
    int in_samples = frame ? frame->nb_samples : 0;

    int out_samples = swr_get_out_samples(dec->swr, in_samples);
    if (out_samples <= 0) return 0;

    /* Compact whatever is still staged to the front before appending. */
    if (dec->conv_head > 0) {
        int keep = dec->conv_frames - dec->conv_head;
        if (keep > 0)
            memmove(dec->conv,
                    dec->conv + (size_t)dec->conv_head * dec->channels,
                    (size_t)keep * dec->channels * sizeof(float));
        dec->conv_frames = keep;
        dec->conv_head = 0;
    }

    if (conv_reserve(dec, dec->conv_frames + out_samples) < 0) return -1;

    /* AV_SAMPLE_FMT_FLT is packed, so swr writes a single interleaved plane. */
    uint8_t *out[1] = {
        (uint8_t *)(dec->conv + (size_t)dec->conv_frames * dec->channels)
    };

    int converted = swr_convert(dec->swr, out, out_samples,
                                frame ? (const uint8_t **)frame->extended_data
                                      : NULL,
                                in_samples);
    if (converted < 0) return -1;

    dec->conv_frames += converted;
    return 0;
}

/* ── demuxing ──────────────────────────────────────────────────────────── */

/* Push one audio packet into the decoder. Returns 0 when the decoder should be
 * polled again, -1 when the stream is unusable. */
static int feed_packet(Decoder *dec)
{
    if (dec->sent_eof) return 0;

    for (;;) {
        int r = av_read_frame(dec->fmt_ctx, dec->pkt);
        if (r < 0) {
            avcodec_send_packet(dec->codec_ctx, NULL);
            dec->sent_eof = 1;
            return 0;
        }

        if (dec->pkt->stream_index != dec->stream_idx) {
            av_packet_unref(dec->pkt);
            continue;
        }

        r = avcodec_send_packet(dec->codec_ctx, dec->pkt);
        av_packet_unref(dec->pkt);

        if (r == 0 || r == AVERROR(EAGAIN)) {
            dec->errors = 0;
            return 0;
        }

        /* A single bad packet should not end the track; a stream of them should. */
        if (++dec->errors > MAX_DECODE_ERRORS) return -1;
    }
}

/* ── lifecycle ─────────────────────────────────────────────────────────── */

Decoder *decoder_open(const char *path)
{
    if (!path) return NULL;

    Decoder *dec = calloc(1, sizeof(*dec));
    if (!dec) return NULL;

    if (avformat_open_input(&dec->fmt_ctx, path, NULL, NULL) < 0)
        goto fail;
    if (avformat_find_stream_info(dec->fmt_ctx, NULL) < 0)
        goto fail;

    int idx = av_find_best_stream(dec->fmt_ctx, AVMEDIA_TYPE_AUDIO,
                                  -1, -1, NULL, 0);
    if (idx < 0) goto fail;
    dec->stream_idx = idx;

    AVStream *st = dec->fmt_ctx->streams[idx];

    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) goto fail;

    dec->codec_ctx = avcodec_alloc_context3(codec);
    if (!dec->codec_ctx) goto fail;
    if (avcodec_parameters_to_context(dec->codec_ctx, st->codecpar) < 0)
        goto fail;
    if (avcodec_open2(dec->codec_ctx, codec, NULL) < 0)
        goto fail;

    dec->channels = dec->codec_ctx->ch_layout.nb_channels;
    if (dec->channels < 1) goto fail;
    if (dec->channels > DECODER_MAX_CHANNELS)
        dec->channels = DECODER_MAX_CHANNELS;

    dec->sample_rate = dec->codec_ctx->sample_rate;
    if (dec->sample_rate < 1) goto fail;

    /* Prefer the stream duration; fall back to the container's. */
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        dec->duration = (double)st->duration * av_q2d(st->time_base);
    else if (dec->fmt_ctx->duration != AV_NOPTS_VALUE &&
             dec->fmt_ctx->duration > 0)
        dec->duration = (double)dec->fmt_ctx->duration / AV_TIME_BASE;

    AVChannelLayout out_ch;
    av_channel_layout_default(&out_ch, dec->channels);

    int ret = swr_alloc_set_opts2(&dec->swr,
                                  &out_ch, AV_SAMPLE_FMT_FLT, dec->sample_rate,
                                  &dec->codec_ctx->ch_layout,
                                  dec->codec_ctx->sample_fmt,
                                  dec->codec_ctx->sample_rate,
                                  0, NULL);
    av_channel_layout_uninit(&out_ch);
    if (ret < 0 || !dec->swr) goto fail;
    if (swr_init(dec->swr) < 0) goto fail;

    dec->pkt = av_packet_alloc();
    if (!dec->pkt) goto fail;
    dec->frame = av_frame_alloc();
    if (!dec->frame) goto fail;

    return dec;

fail:
    decoder_close(dec);
    return NULL;
}

int decoder_channels(Decoder *dec)    { return dec ? dec->channels : 0; }
int decoder_sample_rate(Decoder *dec) { return dec ? dec->sample_rate : 0; }
double decoder_duration(Decoder *dec) { return dec ? dec->duration : 0.0; }

double decoder_position(Decoder *dec)
{
    if (!dec || dec->sample_rate <= 0) return 0.0;
    return (double)(dec->seek_offset + dec->frames_out) / dec->sample_rate;
}

int decoder_eof(Decoder *dec)
{
    if (!dec) return 1;
    return dec->drained && dec->conv_frames - dec->conv_head <= 0;
}

int decoder_read(Decoder *dec, float *buf, int max_frames)
{
    if (!dec || !buf || max_frames <= 0) return 0;

    int ch = dec->channels;
    int total = conv_drain(dec, buf, max_frames);

    while (total < max_frames && !dec->drained) {
        int r = avcodec_receive_frame(dec->codec_ctx, dec->frame);

        if (r == 0) {
            int bad = conv_fill(dec, dec->frame) < 0;
            av_frame_unref(dec->frame);
            if (bad) {
                dec->drained = 1;
                return total > 0 ? total : -1;
            }
            total += conv_drain(dec, buf + (size_t)total * ch,
                                max_frames - total);
            continue;
        }

        if (r == AVERROR(EAGAIN)) {
            /* The flush packet is already in; EAGAIN now means no more data. */
            if (dec->sent_eof) { dec->drained = 1; break; }
            if (feed_packet(dec) < 0) { dec->drained = 1; break; }
            continue;
        }

        if (r == AVERROR_EOF) {
            /* Recover samples swr is still holding before declaring EOF. */
            if (conv_fill(dec, NULL) == 0)
                total += conv_drain(dec, buf + (size_t)total * ch,
                                    max_frames - total);
            dec->drained = 1;
            break;
        }

        dec->drained = 1;
        return total > 0 ? total : -1;
    }

    return total;
}

int decoder_seek(Decoder *dec, double seconds)
{
    if (!dec) return -1;

    if (seconds < 0.0) seconds = 0.0;
    if (dec->duration > 0.0 && seconds > dec->duration)
        seconds = dec->duration;

    AVRational tb = dec->fmt_ctx->streams[dec->stream_idx]->time_base;
    int64_t ts = (int64_t)(seconds / av_q2d(tb));

    /* BACKWARD lands on the keyframe at or before the target, so the decoder
     * has the context it needs; ANY can drop into the middle of a frame. */
    if (av_seek_frame(dec->fmt_ctx, dec->stream_idx, ts,
                      AVSEEK_FLAG_BACKWARD) < 0)
        return -1;

    avcodec_flush_buffers(dec->codec_ctx);

    /* Drop swr's internal delay so resampled output does not carry pre-seek
     * samples across the discontinuity. */
    swr_close(dec->swr);
    if (swr_init(dec->swr) < 0) return -1;

    dec->conv_head   = 0;
    dec->conv_frames = 0;
    dec->frames_out  = 0;
    dec->seek_offset = (int64_t)(seconds * dec->sample_rate);
    dec->sent_eof    = 0;
    dec->drained     = 0;
    dec->errors      = 0;
    return 0;
}

void decoder_close(Decoder *dec)
{
    if (!dec) return;
    av_frame_free(&dec->frame);
    av_packet_free(&dec->pkt);
    swr_free(&dec->swr);
    avcodec_free_context(&dec->codec_ctx);
    avformat_close_input(&dec->fmt_ctx);
    free(dec->conv);
    free(dec);
}
