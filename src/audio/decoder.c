#include "audio/decoder.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct Decoder {
    AVFormatContext *fmt_ctx;
    AVCodecContext  *codec_ctx;
    SwrContext      *swr;
    int              stream_idx;
    int              channels;
    int              sample_rate;
    double           duration;
    int64_t          frames_decoded;
    int              eof;

    AVPacket        *pkt;
    int              pkt_done;
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

    AVRational tb = dec->fmt_ctx->streams[idx]->time_base;
    int64_t d = dec->fmt_ctx->streams[idx]->duration;
    if (d != AV_NOPTS_VALUE)
        dec->duration = (double)d * av_q2d(tb);

    dec->channels   = dec->codec_ctx->ch_layout.nb_channels;
    dec->sample_rate = dec->codec_ctx->sample_rate;

    dec->swr = swr_alloc();
    if (!dec->swr) goto fail;

    AVChannelLayout out_ch;
    av_channel_layout_default(&out_ch, dec->channels);

    int ret = swr_alloc_set_opts2(&dec->swr,
                                  &out_ch, AV_SAMPLE_FMT_FLT, dec->sample_rate,
                                  &dec->codec_ctx->ch_layout,
                                  dec->codec_ctx->sample_fmt,
                                  dec->codec_ctx->sample_rate,
                                  0, NULL);
    if (ret < 0) goto fail;
    if (swr_init(dec->swr) < 0) goto fail;
    av_channel_layout_uninit(&out_ch);

    dec->pkt = av_packet_alloc();
    if (!dec->pkt) goto fail;
    dec->pkt_done = 1;

    return dec;

fail:
    decoder_close(dec);
    return NULL;
}

int decoder_channels(Decoder *dec)   { return dec->channels; }
int decoder_sample_rate(Decoder *dec) { return dec->sample_rate; }
double decoder_duration(Decoder *dec) { return dec->duration; }
double decoder_position(Decoder *dec) {
    return dec->sample_rate > 0
        ? (double)dec->frames_decoded / dec->sample_rate : 0.0;
}
int decoder_eof(Decoder *dec) { return dec->eof; }

int decoder_read(Decoder *dec, float *buf, int max_frames)
{
    if (!dec || dec->eof) return 0;

    AVFrame *frame = av_frame_alloc();
    if (!frame) return -1;

    int total = 0;

    while (total < max_frames && !dec->eof) {
        /* try to read a decoded frame first */
        int r = avcodec_receive_frame(dec->codec_ctx, frame);
        if (r == 0) {
            /* convert to float */
            uint8_t *out[8] = {0};
            int out_samples = swr_get_out_samples(dec->swr, frame->nb_samples);
            av_samples_alloc(out, NULL, dec->channels,
                             out_samples, AV_SAMPLE_FMT_FLT, 0);

            int converted = swr_convert(dec->swr, out, out_samples,
                                        (const uint8_t **)frame->extended_data,
                                        frame->nb_samples);
            if (converted > 0) {
                int room = max_frames - total;
                int copy = converted < room ? converted : room;
                for (int i = 0; i < copy; i++)
                    for (int c = 0; c < dec->channels; c++)
                        buf[(total + i) * dec->channels + c] =
                            ((float *)out[c])[i];
                total += copy;
                dec->frames_decoded += copy;
            }
            av_freep(&out[0]);
            av_frame_unref(frame);
            continue;
        }

        if (r == AVERROR(EAGAIN)) {
            /* need more packets — send one */
            if (dec->pkt_done) {
                if (av_read_frame(dec->fmt_ctx, dec->pkt) < 0) {
                    /* no more packets — drain decoder */
                    avcodec_send_packet(dec->codec_ctx, NULL);
                    dec->pkt_done = 0;
                    continue;
                }
                if (dec->pkt->stream_index != dec->stream_idx) {
                    av_packet_unref(dec->pkt);
                    continue;
                }
                dec->pkt_done = 0;
            }

            int ret = avcodec_send_packet(dec->codec_ctx, dec->pkt);
            av_packet_unref(dec->pkt);
            dec->pkt_done = 1;
            if (ret < 0) {
                /* error sending packet */
                av_frame_free(&frame);
                return -1;
            }
            continue;
        }

        if (r == AVERROR_EOF) {
            dec->eof = 1;
            break;
        }

        /* real error */
        av_frame_free(&frame);
        return -1;
    }

    av_frame_free(&frame);
    return total;
}

int decoder_seek(Decoder *dec, double seconds)
{
    if (!dec) return -1;
    AVRational tb = dec->fmt_ctx->streams[dec->stream_idx]->time_base;
    int64_t ts = (int64_t)(seconds / av_q2d(tb));
    int r = av_seek_frame(dec->fmt_ctx, dec->stream_idx, ts, AVSEEK_FLAG_ANY);
    if (r < 0) return r;
    avcodec_flush_buffers(dec->codec_ctx);
    swr_close(dec->swr);
    swr_init(dec->swr);
    dec->frames_decoded = (int64_t)(seconds * dec->sample_rate);
    dec->eof = 0;
    return 0;
}

void decoder_close(Decoder *dec)
{
    if (!dec) return;
    av_packet_free(&dec->pkt);
    swr_free(&dec->swr);
    avcodec_free_context(&dec->codec_ctx);
    avformat_close_input(&dec->fmt_ctx);
    free(dec);
}
