/*
 * This file is part of sxplayer.
 *
 * Copyright (c) 2015 Stupeflix
 *
 * sxplayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * sxplayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with sxplayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>

#include "decoding.h"
#include "decoders.h"
#include "internal.h"

extern const struct decoder decoder_ffmpeg_sw;
extern const struct decoder decoder_ffmpeg_hw;
static const struct decoder *decoder_def_software = &decoder_ffmpeg_sw;

#if __APPLE__
extern const struct decoder decoder_vt;
static const struct decoder *decoder_def_hwaccel = &decoder_vt;
#else
static const struct decoder *decoder_def_hwaccel = &decoder_ffmpeg_hw;
#endif

struct decoding_ctx {
    void *log_ctx;

    AVThreadMessageQueue *pkt_queue;
    AVThreadMessageQueue *frames_queue;

    struct decoder_ctx *decoder;

    AVRational st_timebase;
    AVFrame *tmp_frame;
    int64_t seek_request;
};

struct decoding_ctx *decoding_alloc(void)
{
    struct decoding_ctx *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->decoder = decoder_alloc();
    if (!ctx->decoder) {
        av_freep(&ctx);
        return NULL;
    }
    return ctx;
}

const AVCodecContext *decoding_get_avctx(struct decoding_ctx *ctx)
{
    return ctx->decoder->avctx;
}

int decoding_init(void *log_ctx,
                  struct decoding_ctx *ctx,
                  AVThreadMessageQueue *pkt_queue,
                  AVThreadMessageQueue *frames_queue,
                  const AVStream *stream,
                  int auto_hwaccel,
                  int export_mvs,
                  void *opaque,
                  int max_pixels)
{
    int ret;
    const struct decoder *dec_def, *dec_def_fallback;

    AVCodecParameters *par = avcodec_parameters_alloc();
    if (!par)
        return AVERROR(ENOMEM);

    ctx->log_ctx = log_ctx;
    ctx->pkt_queue = pkt_queue;
    ctx->frames_queue = frames_queue;

    if (auto_hwaccel && decoder_def_hwaccel) {
        dec_def          = decoder_def_hwaccel;
        dec_def_fallback = decoder_def_software;
    } else {
        dec_def          = decoder_def_software;
        dec_def_fallback = NULL;
    }

    ctx->st_timebase = stream->time_base;

#define DUMP_INFO(par, name) do {                                       \
    if ((par)->codec_type == AVMEDIA_TYPE_AUDIO) {                      \
        char chl[1024];                                                 \
        av_get_channel_layout_string(chl, sizeof(chl), 0,               \
                                     (par)->channel_layout);            \
        TRACE(ctx, name " stream: %s %s @ %dHz tb=%d/%d",               \
              chl, av_get_sample_fmt_name((par)->format),               \
              (par)->sample_rate,                                       \
              ctx->st_timebase.num, ctx->st_timebase.den);              \
    } else {                                                            \
        TRACE(ctx, name " stream: %dx%d in %s tb=%d/%d",                \
              (par)->width, (par)->height,                              \
              av_get_pix_fmt_name((par)->format),                       \
              ctx->st_timebase.num, ctx->st_timebase.den);              \
    }                                                                   \
} while (0)

    DUMP_INFO(stream->codecpar, "original");

    ret = decoder_init(log_ctx, ctx->decoder, dec_def, stream, ctx, opaque, max_pixels);
    if (ret < 0 && dec_def_fallback) {
        TRACE(ctx, "unable to init %s decoder, fallback on %s decoder",
              dec_def->name, dec_def_fallback->name);
        if (ret != AVERROR_DECODER_NOT_FOUND)
            LOG(ctx, ERROR, "Decoder fallback"); // TODO: no fallback here on iOS
        ret = decoder_init(log_ctx, ctx->decoder, dec_def_fallback, stream, ctx, opaque, max_pixels);
    }
    if (ret < 0)
        return ret;

    if (export_mvs)
        av_opt_set(ctx->decoder->avctx, "flags2", "+export_mvs", 0);

    avcodec_parameters_from_context(par, ctx->decoder->avctx);
    DUMP_INFO(par, "initialized");

    LOG(ctx, INFO, "selected decoder: %s", ctx->decoder->dec->name);

    avcodec_parameters_free(&par);

    return 0;
}

static int64_t get_best_effort_ts(const AVFrame *f)
{
    const int64_t t = av_frame_get_best_effort_timestamp(f);
    return t != AV_NOPTS_VALUE ? t : f->pts;
}

static int queue_frame(struct decoding_ctx *ctx, AVFrame *frame)
{
    int ret;
    struct message msg = {
        .type = MSG_FRAME,
        .data = frame,
    };

    TRACE(ctx, "queue frame with ts=%s", PTS2TIMESTR(frame->pts));

    ret = av_thread_message_queue_send(ctx->frames_queue, &msg, 0);
    if (ret < 0) {
        if (ret != AVERROR_EOF && ret != AVERROR_EXIT)
            LOG(ctx, ERROR, "Unable to push frame: %s", av_err2str(ret));
        av_thread_message_queue_set_err_recv(ctx->frames_queue, ret);
    }
    return ret;
}

static int queue_cached_frame(struct decoding_ctx *ctx)
{
    int ret;
    const int64_t cached_ts = av_rescale_q_rnd(get_best_effort_ts(ctx->tmp_frame),
                                               ctx->st_timebase, AV_TIME_BASE_Q,
                                               AV_ROUND_PASS_MINMAX);
    TRACE(ctx, "got a cached frame (t=%s) to push", PTS2TIMESTR(cached_ts));
    AVFrame *prev_frame = ctx->tmp_frame;
    ctx->tmp_frame = NULL;
    prev_frame->pts = cached_ts;
    ret = queue_frame(ctx, prev_frame);
    if (ret < 0) {
        av_frame_free(&prev_frame);
        return ret;
    }
    return 0;
}

int decoding_queue_frame(struct decoding_ctx *ctx, AVFrame *frame)
{
    int ret;

    if (!frame) {
        TRACE(ctx, "async_queue_frame() called for flushing");
        if (ctx->tmp_frame) {
            ret = queue_cached_frame(ctx);
            if (ret < 0)
                return ret;
        }
        return AVERROR_EOF;
    }

    /* Rescale the timestamp to a global large time base: AV_TIME_BASE_Q */
    const int64_t ts = av_rescale_q_rnd(get_best_effort_ts(frame),
                                        ctx->st_timebase, AV_TIME_BASE_Q,
                                        AV_ROUND_PASS_MINMAX);
    TRACE(ctx, "processing frame with ts=%s "
          "(%"PRId64", rescaled from %"PRId64" in %d/%d)",
          PTS2TIMESTR(ts), ts, get_best_effort_ts(frame),
          ctx->st_timebase.num, ctx->st_timebase.den);

    if (ctx->seek_request != AV_NOPTS_VALUE && ts < ctx->seek_request) {
        TRACE(ctx, "frame ts:%s (%"PRId64"), skipping because before %s (%"PRId64")",
              PTS2TIMESTR(ts), ts, PTS2TIMESTR(ctx->seek_request), ctx->seek_request);
        av_frame_free(&ctx->tmp_frame);
        ctx->tmp_frame = frame;
        return 0;
    }

    frame->pts = ts;

    if (ctx->tmp_frame) {
        if (ctx->seek_request != AV_NOPTS_VALUE && ts == ctx->seek_request) {
            av_frame_free(&ctx->tmp_frame);
        } else {
            ret = queue_cached_frame(ctx);
            if (ret < 0)
                return ret;
        }
    } else {
        if (ctx->seek_request != AV_NOPTS_VALUE && ctx->seek_request > 0 && frame->pts > ctx->seek_request) {
            TRACE(ctx, "first frame obtained is after requested time, fixup its ts from %s to %s",
                  PTS2TIMESTR(frame->pts), PTS2TIMESTR(ctx->seek_request));
            frame->pts = ctx->seek_request;
        }
    }

    ctx->seek_request = AV_NOPTS_VALUE;
    return queue_frame(ctx, frame);
}

void decoding_run(struct decoding_ctx *ctx)
{
    int ret;
    int in_err, out_err;

    TRACE(ctx, "decoding packets from %p into %p", ctx->pkt_queue, ctx->frames_queue);

    ctx->seek_request = AV_NOPTS_VALUE;

    for (;;) {
        AVPacket *pkt;
        struct message msg;

        TRACE(ctx, "fetching a packet");
        ret = av_thread_message_queue_recv(ctx->pkt_queue, &msg, 0);
        if (ret < 0)
            break;

        if (msg.type == MSG_SEEK) {
            const int64_t seek_ts = *(int64_t *)msg.data;

            TRACE(ctx, "got a seek message (to %s) in the pkt queue",
                  PTS2TIMESTR(seek_ts));

            /* Make sure the decoder has no packet remaining to consume and
             * pushed (or dropped) all its cached frames. After this flush, we
             * can assume that the decoder will not called async_queue_frame()
             * until a new packet is pushed. */
            decoder_flush(ctx->decoder);

            /* Let's save some little time by dropping frames in the queue so
             * the user don't get a shit ton of false positives before the
             * frames he requested. */
            av_thread_message_flush(ctx->frames_queue);

            /* Mark the seek request so async_queue_frame() can do its
             * "filtering" work. */
            ctx->seek_request = seek_ts;

            /* Forward seek message */
            ret = av_thread_message_queue_send(ctx->frames_queue, &msg, 0);
            if (ret < 0) {
                async_free_message_data(&msg);
                break;
            }

            continue;
        }

        pkt = msg.data;
        TRACE(ctx, "got a packet of size %d, push it to decoder", pkt->size);
        ret = decoder_push_packet(ctx->decoder, pkt);
        av_packet_unref(pkt);
        av_freep(&pkt);
        if (ret < 0)
            break;
    }

    /* Fetch remaining frames */
    if (ret == AVERROR_EOF) {
        TRACE(ctx, "flush cached frames");
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;
        do {
            ret = decoder_push_packet(ctx->decoder, &pkt);
        } while (ret == 0 || ret == AVERROR(EAGAIN));
    }

    /* We pushed everything we could to the decoder, now we make sure frame
     * queuing callback won't be called anymore */
    decoder_flush(ctx->decoder);

    av_frame_free(&ctx->tmp_frame);

    if (ret < 0 && ret != AVERROR_EOF) {
        in_err = out_err = ret;
    } else {
        in_err = AVERROR_EXIT;
        out_err = AVERROR_EOF;
    }
    TRACE(ctx, "notify demuxer with %s and frames queue with %s",
          av_err2str(in_err), av_err2str(out_err));
    av_thread_message_queue_set_err_send(ctx->pkt_queue,    in_err);
    av_thread_message_flush(ctx->pkt_queue);
    av_thread_message_queue_set_err_recv(ctx->frames_queue, out_err);
}

void decoding_free(struct decoding_ctx **ctxp)
{
    struct decoding_ctx *ctx = *ctxp;
    if (!ctx)
        return;
    decoder_free(&ctx->decoder);
    av_freep(ctxp);
}
