#include "media.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static int frame_rgb(const AVFrame *frame, SfImage *image) {
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 8192 || frame->height > 8192) return 0;
    int stride = (frame->width * 3 + 31) & ~31;
    /* swscale SIMD paths need aligned rows and padding after the last row. */
    uint8_t *pixels = calloc(1, (size_t)stride * (frame->height + 1) + 64);
    if (!pixels) return 0;
    struct SwsContext *sws = sws_getContext(frame->width, frame->height, frame->format,
        frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) { free(pixels); return 0; }
    uint8_t *dst[] = {pixels, NULL, NULL, NULL};
    int strides[] = {stride, 0, 0, 0};
    int rows = sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height, dst, strides);
    sws_freeContext(sws);
    if (rows != frame->height) { free(pixels); return 0; }
    memset(image, 0, sizeof(*image));
    image->pixels = pixels; image->width = frame->width; image->height = frame->height; image->stride = stride;
    double sum[3] = {0}, squares[3] = {0};
    size_t count = (size_t)frame->width * frame->height;
    for (int y = 0; y < frame->height; ++y) for (int x = 0; x < frame->width; ++x) for (int c = 0; c < 3; ++c) {
        double v = pixels[(size_t)y*stride+3*x+c]; sum[c] += v; squares[c] += v*v;
    }
    double std = 0, low = 255, high = 0;
    for (int c = 0; c < 3; ++c) {
        double mean = sum[c]/count;
        std += sqrt(fmax(0, squares[c]/count - mean*mean))/3;
        low = fmin(low, mean); high = fmax(high, mean);
    }
    image->quality = fmin(1, std/45)*.75 + fmin(1, (high-low)/80)*.25;
    return 1;
}
void sf_image_free(SfImage *image) {
    if (image) { free((void *)image->pixels); memset(image, 0, sizeof(*image)); }
}

int sf_image_decode(const uint8_t *data, size_t size, SfCodec codec, SfImage *image) {
    if (!data || !size || size > INT_MAX || !image || (codec != SF_CODEC_H264 && codec != SF_CODEC_H265)) return 0;
    const AVCodec *decoder = avcodec_find_decoder(codec == SF_CODEC_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(decoder);
    AVPacket *packet = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    int ok = 0;
    if (!decoder || !ctx || !packet || !frame) goto done;
    ctx->thread_count = 1; ctx->err_recognition = AV_EF_IGNORE_ERR;
    ctx->max_pixels = 8192LL * 8192;
    if (avcodec_open2(ctx, decoder, NULL) < 0 || av_new_packet(packet, (int)size) < 0) goto done;
    memcpy(packet->data, data, size);
    if (avcodec_send_packet(ctx, packet) < 0) goto done;
    if (avcodec_receive_frame(ctx, frame) == 0) ok = frame_rgb(frame, image);
    else if (avcodec_send_packet(ctx, NULL) >= 0 && avcodec_receive_frame(ctx, frame) == 0) ok = frame_rgb(frame, image);
done:
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&ctx);
    return ok;
}

int sf_image_read(const char *path, SfImage *image) {
    AVFormatContext *format = NULL; AVCodecContext *ctx = NULL;
    AVPacket *packet = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    int ok = 0;
    if (!path || !image || !packet || !frame || avformat_open_input(&format, path, NULL, NULL) < 0) goto done;
    if (avformat_find_stream_info(format, NULL) < 0) goto done;
    int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream < 0) goto done;
    const AVCodec *decoder = avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);
    ctx = avcodec_alloc_context3(decoder);
    if (!ctx || avcodec_parameters_to_context(ctx, format->streams[stream]->codecpar) < 0) goto done;
    ctx->thread_count = 1; ctx->max_pixels = 8192LL * 8192;
    if (avcodec_open2(ctx, decoder, NULL) < 0) goto done;
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == stream && avcodec_send_packet(ctx, packet) >= 0 && avcodec_receive_frame(ctx, frame) == 0) {
            ok = frame_rgb(frame, image); av_packet_unref(packet); break;
        }
        av_packet_unref(packet);
    }
    if (!ok && avcodec_send_packet(ctx, NULL) >= 0 && avcodec_receive_frame(ctx, frame) == 0) ok = frame_rgb(frame, image);
done:
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&ctx); avformat_close_input(&format);
    return ok;
}

int sf_image_write_png(const char *path, const SfImage *image) {
    if (!path || !image || !image->pixels || image->width <= 0 || image->width > 8192 ||
        image->height <= 0 || image->height > 8192 || image->stride < image->width*3) return 0;
    const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *ctx = avcodec_alloc_context3(encoder);
    AVFrame *frame = av_frame_alloc(); AVPacket *packet = av_packet_alloc();
    int ok = 0;
    if (!ctx || !frame || !packet) goto done;
    ctx->width = image->width; ctx->height = image->height;
    ctx->pix_fmt = AV_PIX_FMT_RGB24; ctx->time_base = (AVRational){1, 1}; ctx->thread_count = 1;
    if (avcodec_open2(ctx, encoder, NULL) < 0) goto done;
    frame->width = ctx->width; frame->height = ctx->height; frame->format = ctx->pix_fmt;
    if (av_frame_get_buffer(frame, 32) < 0) goto done;
    for (int y = 0; y < image->height; ++y)
        memcpy(frame->data[0] + (size_t)y*frame->linesize[0],
               image->pixels + (size_t)y*image->stride, (size_t)image->width*3);
    if (avcodec_send_frame(ctx, frame) < 0 || avcodec_receive_packet(ctx, packet) < 0) goto done;
    FILE *f = fopen(path, "wb");
    if (f) { ok = fwrite(packet->data, 1, packet->size, f) == (size_t)packet->size; if (fclose(f)) ok = 0; }
done:
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&ctx); return ok;
}

struct SfVideoOutput {
    AVFormatContext *format;
    AVCodecContext *encoder;
    AVFrame *frame;
    AVFrame *source;
    AVPacket *packet;
    struct SwsContext *sws;
    AVStream *stream;
    int64_t pts;
    int header, failed;
};

static int send_packets(SfVideoOutput *out) {
    int r;
    while ((r = avcodec_receive_packet(out->encoder, out->packet)) >= 0) {
        av_packet_rescale_ts(out->packet, out->encoder->time_base, out->stream->time_base);
        out->packet->stream_index = out->stream->index;
        int sent = av_interleaved_write_frame(out->format, out->packet);
        av_packet_unref(out->packet);
        if (sent < 0) return 0;
    }
    return r == AVERROR(EAGAIN) || r == AVERROR_EOF;
}
SfVideoOutput *sf_video_output_open(const char *url, int width, int height, int fps) {
    if (!url || width <= 0 || height <= 0 || width > 8192 || height > 8192 || (width&1) || (height&1) || fps < 1 || fps > 60) return NULL;
    SfVideoOutput *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
    if (avformat_alloc_output_context2(&out->format, NULL, "mpegts", url) < 0) goto fail;
    out->encoder = avcodec_alloc_context3(codec); out->frame = av_frame_alloc(); out->packet = av_packet_alloc();
    out->source = av_frame_alloc();
    out->stream = avformat_new_stream(out->format, NULL);
    if (!out->encoder || !out->frame || !out->source || !out->packet || !out->stream) goto fail;
    out->encoder->width = width; out->encoder->height = height;
    out->encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    out->encoder->time_base = (AVRational){1, fps};
    out->encoder->framerate = (AVRational){fps, 1};
    out->encoder->gop_size = 1; out->encoder->max_b_frames = 0;
    out->encoder->bit_rate = 500000; out->encoder->thread_count = 1;
    out->encoder->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    if (avcodec_open2(out->encoder, codec, NULL) < 0 || avcodec_parameters_from_context(out->stream->codecpar, out->encoder) < 0) goto fail;
    out->stream->time_base = out->encoder->time_base;
    out->format->max_delay = 0;
    out->format->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    if (avio_open(&out->format->pb, url, AVIO_FLAG_WRITE) < 0) goto fail;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "mpegts_flags", "resend_headers", 0);
    int r = avformat_write_header(out->format, &opts); av_dict_free(&opts);
    if (r < 0) goto fail;
    out->header = 1;
    out->frame->width = width; out->frame->height = height; out->frame->format = AV_PIX_FMT_YUV420P;
    if (av_frame_get_buffer(out->frame, 32) < 0) goto fail;
    return out;
fail:
    sf_video_output_close(out); return NULL;
}
int sf_video_output_send(SfVideoOutput *out, const SfImage *image) {
    if (!out || out->failed || !image || !image->pixels || image->width <= 0 || image->height <= 0 || image->width > 8192 || image->height > 8192 || image->stride < image->width*3) return 0;
    out->sws = sws_getCachedContext(out->sws, image->width, image->height, AV_PIX_FMT_RGB24,
        out->frame->width, out->frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
    if (!out->sws || av_frame_make_writable(out->frame) < 0) { out->failed = 1; return 0; }
    /* Host-provided RGB buffers need not have FFmpeg's SIMD padding. */
    if (out->source->width != image->width || out->source->height != image->height) {
        av_frame_unref(out->source);
        out->source->width = image->width; out->source->height = image->height;
        out->source->format = AV_PIX_FMT_RGB24;
        if (av_frame_get_buffer(out->source, 32) < 0) { out->failed = 1; return 0; }
    }
    for (int y = 0; y < image->height; ++y)
        memcpy(out->source->data[0] + (size_t)y*out->source->linesize[0],
               image->pixels + (size_t)y*image->stride, (size_t)image->width*3);
    if (sws_scale(out->sws, (const uint8_t *const *)out->source->data, out->source->linesize,
                  0, image->height, out->frame->data, out->frame->linesize) != out->frame->height) {
        out->failed = 1; return 0;
    }
    out->frame->pts = out->pts++;
    if (avcodec_send_frame(out->encoder, out->frame) < 0 || !send_packets(out)) { out->failed = 1; return 0; }
    avio_flush(out->format->pb);
    return 1;
}
int sf_video_output_close(SfVideoOutput *out) {
    if (!out) return 1;
    int ok = !out->failed;
    if (out->header) {
        if (avcodec_send_frame(out->encoder, NULL) >= 0 && !send_packets(out)) ok = 0;
        if (av_write_trailer(out->format) < 0) ok = 0;
    }
    if (out->format && out->format->pb && avio_closep(&out->format->pb) < 0) ok = 0;
    sws_freeContext(out->sws); av_frame_free(&out->frame); av_frame_free(&out->source); av_packet_free(&out->packet);
    avcodec_free_context(&out->encoder); avformat_free_context(out->format); free(out); return ok;
}
