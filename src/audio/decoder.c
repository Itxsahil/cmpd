#include "audio/decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdlib.h>
#include <stdio.h>

struct Decoder {
    AVFormatContext *fmt_ctx;
    AVCodecContext  *codec_ctx;
    SwrContext      *swr;
    int              stream_idx;
    int              channels;
    int              sample_rate;
    double           duration;
};

Decoder *decoder_open(const char *path)
{
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

    const AVCodec *codec = avcodec_find_decoder(
        dec->fmt_ctx->streams[idx]->codecpar->codec_id);
    if (!codec) goto fail;

    dec->codec_ctx = avcodec_alloc_context3(codec);
    if (!dec->codec_ctx) goto fail;
    if (avcodec_parameters_to_context(dec->codec_ctx,
        dec->fmt_ctx->streams[idx]->codecpar) < 0)
        goto fail;
    if (avcodec_open2(dec->codec_ctx, codec, NULL) < 0)
        goto fail;

    dec->channels   = dec->codec_ctx->ch_layout.nb_channels;
    dec->sample_rate = dec->codec_ctx->sample_rate;

    AVRational dur = dec->fmt_ctx->streams[idx]->time_base;
    int64_t d = dec->fmt_ctx->streams[idx]->duration;
    if (d != AV_NOPTS_VALUE)
        dec->duration = (double)d * dur.num / dur.den;

    dec->swr = swr_alloc();
    if (!dec->swr) goto fail;

    AVChannelLayout out_ch = AV_CHANNEL_LAYOUT_STEREO;
    int out_rate = dec->sample_rate;
    enum AVSampleFormat out_fmt = AV_SAMPLE_FMT_FLT;

    int ret = swr_alloc_set_opts2(&dec->swr,
                                  &out_ch, out_fmt, out_rate,
                                  &dec->codec_ctx->ch_layout,
                                  dec->codec_ctx->sample_fmt,
                                  dec->codec_ctx->sample_rate,
                                  0, NULL);
    if (ret < 0) goto fail;
    if (swr_init(dec->swr) < 0) goto fail;

    dec->channels   = out_ch.nb_channels;
    dec->sample_rate = out_rate;

    return dec;

fail:
    decoder_close(dec);
    return NULL;
}

int decoder_channels(Decoder *dec) { return dec->channels; }
int decoder_sample_rate(Decoder *dec) { return dec->sample_rate; }
double decoder_duration(Decoder *dec) { return dec->duration; }

int decoder_decode(Decoder *dec, decoder_cb cb, void *userdata)
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();
    if (!pkt || !frame) { av_packet_free(&pkt); av_frame_free(&frame); return -1; }

    int ret = 0;
    while (av_read_frame(dec->fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != dec->stream_idx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec->codec_ctx, pkt) < 0) {
            av_packet_unref(pkt);
            ret = -1;
            break;
        }
        av_packet_unref(pkt);

        while (1) {
            int r = avcodec_receive_frame(dec->codec_ctx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                break;
            if (r < 0) { ret = -1; goto done; }

            uint8_t *out[2] = {0};
            int out_samples = swr_get_out_samples(dec->swr, frame->nb_samples);
            av_samples_alloc(out, NULL, dec->channels,
                             out_samples, AV_SAMPLE_FMT_FLT, 0);

            int converted = swr_convert(dec->swr, out, out_samples,
                                        (const uint8_t **)frame->extended_data,
                                        frame->nb_samples);
            if (converted > 0) {
                int total = converted * dec->channels;
                float *interleaved = malloc(total * sizeof(float));
                if (interleaved) {
                    for (int i = 0; i < converted; i++)
                        for (int c = 0; c < dec->channels; c++)
                            interleaved[i * dec->channels + c] =
                                ((float *)out[c])[i];
                    cb(interleaved, converted, dec->channels,
                       dec->sample_rate, userdata);
                    free(interleaved);
                }
            }
            av_freep(&out[0]);
            av_frame_unref(frame);
        }
    }

    avcodec_send_packet(dec->codec_ctx, NULL);
    while (1) {
        int r = avcodec_receive_frame(dec->codec_ctx, frame);
        if (r == AVERROR_EOF) break;
        if (r < 0) break;

        uint8_t *out[2] = {0};
        int out_samples = swr_get_out_samples(dec->swr, frame->nb_samples);
        av_samples_alloc(out, NULL, dec->channels,
                         out_samples, AV_SAMPLE_FMT_FLT, 0);

        int converted = swr_convert(dec->swr, out, out_samples,
                                    (const uint8_t **)frame->extended_data,
                                    frame->nb_samples);
        if (converted > 0) {
            int total = converted * dec->channels;
            float *interleaved = malloc(total * sizeof(float));
            if (interleaved) {
                for (int i = 0; i < converted; i++)
                    for (int c = 0; c < dec->channels; c++)
                        interleaved[i * dec->channels + c] =
                            ((float *)out[c])[i];
                cb(interleaved, converted, dec->channels,
                   dec->sample_rate, userdata);
                free(interleaved);
            }
        }
        av_freep(&out[0]);
        av_frame_unref(frame);
    }

done:
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret;
}

int decoder_seek(Decoder *dec, double seconds)
{
    AVRational tb = dec->fmt_ctx->streams[dec->stream_idx]->time_base;
    int64_t ts = (int64_t)(seconds / av_q2d(tb));
    int r = av_seek_frame(dec->fmt_ctx, dec->stream_idx, ts, AVSEEK_FLAG_ANY);
    if (r < 0) return r;
    avcodec_flush_buffers(dec->codec_ctx);
    swr_close(dec->swr);
    swr_init(dec->swr);
    return 0;
}

void decoder_close(Decoder *dec)
{
    if (!dec) return;
    swr_free(&dec->swr);
    avcodec_free_context(&dec->codec_ctx);
    avformat_close_input(&dec->fmt_ctx);
    free(dec);
}
