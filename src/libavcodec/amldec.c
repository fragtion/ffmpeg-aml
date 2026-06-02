/*
 * AMLogic Video Decoder
 * Ported to modern FFmpeg API from LongChair/FFmpeg (amlvideo branch)
 * Original Copyright (c) 2016 Lionel Chazallon
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include "avcodec.h"
#include "aml.h"
#include "bsf.h"
#include "codec_internal.h"
#include "decode.h"
#include "hwconfig.h"
#include "amltools.h"
#include "amldec.h"

#include "libavutil/avassert.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "libavutil/log.h"

#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/videodev2.h>

#define AMV_DEVICE_NAME "/dev/video10"
#define UNIT_FREQ       96000
#define TRICKMODE_NONE  0

void ffaml_log_decoder_info(AVCodecContext *avctx)
{
    AMLDecodeContext *s = avctx->priv_data;
    av_log(avctx, AV_LOG_DEBUG,
           "Decoder buffer: filled %d bytes (%.1f%%), read=%d write=%d pkts=%d\n",
           s->buffer_status.data_len,
           (double)(s->buffer_status.data_len * 100) /
           (double)(s->buffer_status.data_len + s->buffer_status.free_len),
           s->buffer_status.read_pointer,
           s->buffer_status.write_pointer,
           s->packets_written);
}

int ffmal_init_bitstream(AVCodecContext *avctx)
{
    AMLDecodeContext *s = avctx->priv_data;
    const AVBitStreamFilter *bsf;
    int ret;

    if (s->bsf)
        return 0;

    switch (avctx->codec_id) {
    case AV_CODEC_ID_H264:
        bsf = av_bsf_get_by_name("h264_mp4toannexb");
        break;
    case AV_CODEC_ID_HEVC:
        bsf = av_bsf_get_by_name("hevc_mp4toannexb");
        break;
    default:
        av_log(avctx, AV_LOG_DEBUG, "No bitstream filter needed\n");
        return 0;
    }

    if (!bsf)
        return AVERROR_BSF_NOT_FOUND;

    av_log(avctx, AV_LOG_DEBUG, "Using bitstream filter: %s\n", bsf->name);

    if ((ret = av_bsf_alloc(bsf, &s->bsf)) < 0)
        return ret;

    if ((ret = avcodec_parameters_from_context(s->bsf->par_in, avctx)) < 0 ||
        (ret = av_bsf_init(s->bsf)) < 0) {
        av_bsf_free(&s->bsf);
        return ret;
    }

    return 0;
}

int ffaml_write_codec_data(AVCodecContext *avctx, char *data, int size)
{
    AMLDecodeContext *s = avctx->priv_data;
    codec_para_t *pcodec = &s->codec;
    int bytesleft = size;
    int written;

    while (bytesleft > 0) {
        written = codec_write(pcodec, data, bytesleft);
        if (written < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "Failed to write to codec (ret=%d)\n", written);
            usleep(10);
        } else {
            data      += written;
            bytesleft -= written;
        }
    }
    return 0;
}

void ffaml_create_prefeed_header(AVCodecContext *avctx, AVPacket *pkt,
                                  AMLHeader *header, char *extradata,
                                  int extradatasize)
{
    header->size = 0;
    if (!extradata || extradatasize <= 0)
        return;
    switch (aml_get_vformat(avctx)) {
    case VFORMAT_VC1:
        memcpy(header->data, extradata + 1, extradatasize - 1);
        header->size = extradatasize - 1;
        break;
    default:
        memcpy(header->data, extradata, extradatasize);
        header->size = extradatasize;
        break;
    }
}

void ffaml_checkin_packet_pts(AVCodecContext *avctx, AVPacket *avpkt)
{
    AMLDecodeContext *s = avctx->priv_data;
    int ret;
    double pts = (double)avpkt->pts * (double)PTS_FREQ * av_q2d(avctx->pkt_timebase);
    av_log(avctx, AV_LOG_DEBUG, "Checking in pts=%f\n", pts);
    if ((ret = codec_checkin_pts(&s->codec, (unsigned long)pts)) < 0)
        av_log(avctx, AV_LOG_ERROR, "Failed to checkin pts (ret=%d)\n", ret);
}

void ffaml_get_packet_header(AVCodecContext *avctx, AMLHeader *header,
                              AVPacket *pkt)
{
    header->size = 0;
    switch (aml_get_vformat(avctx)) {
    case VFORMAT_VC1:
        if (pkt->data[0] == 0x0 && pkt->data[1] == 0x0 &&
            pkt->data[2] == 0x1 &&
            (pkt->data[3] == 0xD || pkt->data[3] == 0xF))
            break; /* header already present */
        header->data[0] = 0x0;
        header->data[1] = 0x0;
        header->data[2] = 0x1;
        header->data[3] = 0xd;
        header->size    = 4;
        break;
    default:
        break;
    }
}

/* ---- writer thread: paces queued packets into the VPU ------------------- *
 *
 * Runs entirely off FFmpeg's decode thread. The decode callback only enqueues
 * (fast); all the blocking work (waiting for VPU input-buffer room, writing
 * the elementary stream) happens here, so a concurrent audio output is never
 * starved by video pacing.
 */
static void *ffaml_writer_thread(void *arg)
{
    AVCodecContext *avctx = arg;
    AMLDecodeContext *s = avctx->priv_data;
    AMLHeader header;

    for (;;) {
        AVPacket *pkt;
        int enough_space = 0;
        int ret;

        pthread_mutex_lock(&s->queue_mutex);
        while (s->writer_run && s->framequeue.size == 0)
            pthread_cond_wait(&s->queue_cond, &s->queue_mutex);
        if (s->framequeue.size == 0 && !s->writer_run) {
            pthread_mutex_unlock(&s->queue_mutex);
            break;
        }
        pkt = ffaml_dequeue_packet(avctx, &s->framequeue);
        pthread_mutex_unlock(&s->queue_mutex);

        if (!pkt)
            continue;

        /* seek/flush in progress: drop without feeding the VPU */
        if (s->writer_abort) {
            av_packet_free(&pkt);
            continue;
        }

        /* block until the VPU input buffer has room (our thread, not ffmpeg's) */
        do {
            ret = codec_get_vbuf_state(&s->codec, &s->buffer_status);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR,
                       "Failed to get vbuf state (ret=%d)\n", ret);
                break;
            }
            s->decoder_buffer_pc = (double)(s->buffer_status.data_len * 100) /
                (double)(s->buffer_status.data_len + s->buffer_status.free_len);
            if (s->buffer_status.free_len > (pkt->size + MAX_HEADER_SIZE))
                enough_space = 1;
            else
                usleep(10000);
        } while (!enough_space && s->writer_run && !s->writer_abort);

        if (s->writer_abort) {
            av_packet_free(&pkt);
            continue;
        }

        /* prefeed header on first packet */
        if (s->first_packet) {
            ffaml_create_prefeed_header(avctx, pkt, &header,
                                        (char *)s->extradata, s->extradata_size);
            if (header.size > 0)
                ffaml_write_codec_data(avctx, header.data, header.size);
            s->first_packet = 0;
        }

        ffaml_checkin_packet_pts(avctx, pkt);

        ffaml_get_packet_header(avctx, &header, pkt);
        if (header.size > 0)
            ffaml_write_codec_data(avctx, header.data, header.size);

        ffaml_write_codec_data(avctx, (char *)pkt->data, pkt->size);
        s->packets_written++;

        av_packet_free(&pkt);
    }
    return NULL;
}

static void ffaml_start_writer(AVCodecContext *avctx)
{
    AMLDecodeContext *s = avctx->priv_data;
    s->writer_run   = 1;
    s->writer_abort = 0;
    if (pthread_create(&s->writer_thread, NULL, ffaml_writer_thread, avctx) == 0) {
        s->writer_started = 1;
    } else {
        s->writer_started = 0;
        av_log(avctx, AV_LOG_ERROR, "Failed to start VPU writer thread\n");
    }
}

/* drain=1: let the writer feed any queued packets to the VPU before stopping
 * (clean EOF). drain=0: discard queued packets (seek/flush). */
static void ffaml_stop_writer(AVCodecContext *avctx, int drain)
{
    AMLDecodeContext *s = avctx->priv_data;
    if (!s->writer_started)
        return;
    pthread_mutex_lock(&s->queue_mutex);
    s->writer_run = 0;
    if (!drain)
        s->writer_abort = 1;
    pthread_cond_broadcast(&s->queue_cond);
    pthread_mutex_unlock(&s->queue_mutex);

    pthread_join(s->writer_thread, NULL);
    s->writer_started = 0;

    /* free anything left (only possible if create failed mid-run) */
    ffaml_queue_clear(avctx, &s->framequeue);
}

/* Enqueue a packet for the writer thread. This MUST NOT block: it runs (via the
 * decode callback) on FFmpeg's demuxer/decode path, which is shared with every
 * other stream of the same input. If we blocked here while the VPU input buffer
 * is full, the demuxer would stall on video and stop delivering packets to a
 * concurrent audio output (ALSA) — classic head-of-line blocking. That is the
 * real reason single-process A/V used to starve the audio (audio only flushed
 * once the video side was torn down) and why playback previously needed two
 * ffmpeg processes.
 *
 * Instead we drop the oldest queued packet on overflow. With realtime input
 * (-re for files, or an inherently realtime live source) the VPU keeps up, the
 * queue stays near-empty, and nothing is ever dropped; the audio output (which
 * paces itself against the sound card) becomes the master clock for the shared
 * demuxer. */
static int ffaml_enqueue(AVCodecContext *avctx, AVPacket *pkt)
{
    AMLDecodeContext *s = avctx->priv_data;
    int ret;
    pthread_mutex_lock(&s->queue_mutex);
    if (s->framequeue.size >= MAX_QUEUE_PACKETS) {
        AVPacket *old = ffaml_dequeue_packet(avctx, &s->framequeue);
        if (old)
            av_packet_free(&old);
        if (!(s->dropped_packets++ & 0x7f))
            av_log(avctx, AV_LOG_WARNING,
                   "VPU input queue full; dropping video to keep audio flowing "
                   "(%u dropped so far). Use -re for file playback.\n",
                   s->dropped_packets);
    }
    ret = ffaml_queue_packet(avctx, &s->framequeue, pkt);   /* clones pkt */
    pthread_cond_signal(&s->queue_cond);
    pthread_mutex_unlock(&s->queue_mutex);
    return ret;
}

static av_cold int ffaml_init_decoder(AVCodecContext *avctx)
{
    AMLDecodeContext *s = avctx->priv_data;
    codec_para_t *pcodec = &s->codec;
    int ret;

    s->first_packet    = 1;
    s->bsf             = NULL;
    s->packets_written = 0;
    s->frame_count     = 0;
    s->dropped_packets = 0;
    s->blank           = NULL;

    ffaml_init_queue(&s->framequeue);
    pthread_mutex_init(&s->queue_mutex, NULL);
    pthread_cond_init(&s->queue_cond, NULL);
    s->writer_started = 0;

    /* configure the amlogic vfm pipeline (direct render to amvideo display) */
    amlsysfs_write_string(avctx, "/sys/class/vfm/map", "rm default");
    amlsysfs_write_string(avctx, "/sys/class/vfm/map",
                          "add default decoder ppmgr deinterlace amvideo");
    amlsysfs_write_int(avctx, "/sys/class/video/blackout_policy", 0);
    amlsysfs_write_int(avctx, "/sys/module/amlvideodri/parameters/freerun_mode", 1);
    amlsysfs_write_int(avctx, "/sys/class/tsync/enable", 0);
    amlsysfs_write_int(avctx, "/sys/class/video/disable_video", 0);

    memset(pcodec, 0, sizeof(codec_para_t));
    memset(&s->buffer_status, 0, sizeof(s->buffer_status));

    /* init handles to invalid - critical, Kodi does this */
    pcodec->handle             = -1;
    pcodec->cntl_handle        = -1;
    pcodec->sub_handle         = -1;
    pcodec->audio_utils_handle = -1;

    pcodec->stream_type         = STREAM_TYPE_ES_VIDEO;
    pcodec->has_video           = 1;
    pcodec->noblock             = 0;
    pcodec->video_type          = aml_get_vformat(avctx);
    pcodec->am_sysinfo.format   = aml_get_vdec_type(avctx);
    pcodec->am_sysinfo.width    = avctx->width;
    pcodec->am_sysinfo.height   = avctx->height;

    if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
        pcodec->am_sysinfo.rate = (unsigned int)(0.5 +
            (float)UNIT_FREQ * avctx->framerate.den / avctx->framerate.num);
    else if (avctx->time_base.num > 0)
        pcodec->am_sysinfo.rate = (unsigned int)(0.5 +
            (float)UNIT_FREQ * avctx->time_base.num / avctx->time_base.den);

    pcodec->am_sysinfo.ratio   = 0x100;
    pcodec->am_sysinfo.ratio64 = 0x100;

    /* EXTERNAL_PTS | SYNC_OUTSIDE: we provide PTS and handle sync ourselves
     * (video-only freerun; no audio clock to wait on) */
    pcodec->am_sysinfo.param = (void *)(EXTERNAL_PTS | SYNC_OUTSIDE);

    if (pcodec->video_type == (vformat_t)-1) {
        av_log(avctx, AV_LOG_ERROR,
               "Cannot determine video type for codec_id=%d\n", avctx->codec_id);
        return AVERROR(EINVAL);
    }

    if (pcodec->am_sysinfo.format == (vdec_type_t)-1) {
        av_log(avctx, AV_LOG_ERROR,
               "Cannot determine vdec type for codec_tag=0x%x\n", avctx->codec_tag);
        return AVERROR(EINVAL);
    }

    s->amv_fd = open(AMV_DEVICE_NAME, O_RDWR);
    if (s->amv_fd <= 0) {
        av_log(avctx, AV_LOG_ERROR,
               "Failed to open %s (fd=%d)\n", AMV_DEVICE_NAME, s->amv_fd);
        return AVERROR(EIO);
    }
    av_log(avctx, AV_LOG_DEBUG, "Opened %s fd=%d\n", AMV_DEVICE_NAME, s->amv_fd);

    ret = codec_init(pcodec);
    if (ret != CODEC_ERROR_NONE) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init amcodec (ret=%d)\n", ret);
        return AVERROR_EXTERNAL;
    }

    codec_resume(pcodec);

    codec_set_cntl_mode(pcodec, 0);                 /* TRICKMODE_NONE */
    codec_set_cntl_avthresh(pcodec, PTS_FREQ * 30);
    codec_set_cntl_syncthresh(pcodec, 0);

    ret = ffmal_init_bitstream(avctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to init bitstream filter\n");
        return ret;
    }

    /* extradata for the prefeed header: from the bsf output if present,
     * otherwise straight from the stream */
    if (s->bsf) {
        s->extradata      = s->bsf->par_out->extradata;
        s->extradata_size = s->bsf->par_out->extradata_size;
    } else {
        s->extradata      = avctx->extradata;
        s->extradata_size = avctx->extradata_size;
    }

    /* Pre-allocate the blank placeholder frame handed back from ffaml_decode().
     * Software pixel format so a downstream (null) video output can wrap it
     * without any hwframes context. Content is irrelevant — it is discarded by
     * the output; the real picture is on HDMI via the VPU. */
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    s->blank = av_frame_alloc();
    if (!s->blank)
        return AVERROR(ENOMEM);
    s->blank->format = AV_PIX_FMT_YUV420P;
    s->blank->width  = avctx->width  > 0 ? avctx->width  : 16;
    s->blank->height = avctx->height > 0 ? avctx->height : 16;
    if ((ret = av_frame_get_buffer(s->blank, 0)) < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate placeholder frame\n");
        return ret;
    }
    for (int i = 0; i < AV_NUM_DATA_POINTERS && s->blank->buf[i]; i++)
        memset(s->blank->buf[i]->data, 0, s->blank->buf[i]->size);

    ffaml_start_writer(avctx);

    av_log(avctx, AV_LOG_INFO, "amcodec initialised: format=%s vdec=%s\n",
           aml_get_vformat_name(pcodec->video_type),
           aml_get_vdec_name(pcodec->am_sysinfo.format));

    return 0;
}

static av_cold int ffaml_close_decoder(AVCodecContext *avctx)
{
    AMLDecodeContext *s = avctx->priv_data;

    /* feed remaining queued packets, then stop the writer */
    ffaml_stop_writer(avctx, 1);

    /* Do NOT set blackout_policy=1 - it blacks out before display shows frame */
    usleep(500 * 1000);  /* let display pipeline consume remaining frames */
    codec_close(&s->codec);

    if (s->bsf)
        av_bsf_free(&s->bsf);

    av_frame_free(&s->blank);

    if (s->amv_fd > 0) {
        close(s->amv_fd);
        s->amv_fd = 0;
    }

    pthread_cond_destroy(&s->queue_cond);
    pthread_mutex_destroy(&s->queue_mutex);

    return 0;
}

static int ffaml_decode(AVCodecContext *avctx, AVFrame *frame,
                         int *got_frame, AVPacket *avpkt)
{
    AMLDecodeContext *s = avctx->priv_data;
    int ret;
    int pkt_size;

    *got_frame = 0;

    if (!avpkt || !avpkt->data || avpkt->size == 0)
        return 0;

    pkt_size = avpkt->size;

    /* Enqueue the packet (after annexb conversion for H264/HEVC) and return
     * immediately. The writer thread does the realtime VPU feeding so we never
     * block FFmpeg's thread here. The decoder is direct-render (real picture
     * goes to HDMI via the VPU); we hand back only a blank placeholder frame
     * below. See README for behavior details. */
    if (s->bsf) {
        AVPacket filter_pkt = { 0 };
        AVPacket out_pkt     = { 0 };

        if ((ret = av_packet_ref(&filter_pkt, avpkt)) < 0)
            return ret;
        if ((ret = av_bsf_send_packet(s->bsf, &filter_pkt)) < 0) {
            av_packet_unref(&filter_pkt);
            return ret;
        }
        while ((ret = av_bsf_receive_packet(s->bsf, &out_pkt)) == 0) {
            ffaml_enqueue(avctx, &out_pkt);   /* clones internally */
            av_packet_unref(&out_pkt);
        }
        /* ret == AVERROR(EAGAIN) is the normal "need more input" exit */
    } else {
        ffaml_enqueue(avctx, avpkt);
    }

    /* Hand back a blank placeholder frame so a downstream video output keeps
     * initialising/advancing and does not gate a concurrent audio output. The
     * real picture is rendered by the VPU to HDMI; this frame is discarded. */
    if (s->blank) {
        if ((ret = av_frame_ref(frame, s->blank)) < 0)
            return ret;
        frame->pts     = avpkt->pts;
        frame->pkt_dts = avpkt->dts;
        *got_frame = 1;
    }

    return pkt_size;
}

static void ffaml_flush(AVCodecContext *avctx)
{
    AMLDecodeContext *s = avctx->priv_data;
    int ret;

    av_log(avctx, AV_LOG_DEBUG, "Flushing decoder (codec_reset)\n");

    /* discard queued packets — a seek invalidates them */
    ffaml_stop_writer(avctx, 0);

    ret = codec_reset(&s->codec);
    if (ret != CODEC_ERROR_NONE) {
        av_log(avctx, AV_LOG_WARNING,
               "codec_reset failed (ret=%d), falling back to close+init\n", ret);
        /* close_decoder stops the (already stopped) writer harmlessly */
        ffaml_close_decoder(avctx);
        ffaml_init_decoder(avctx);
        return;
    }

    codec_resume(&s->codec);
    s->first_packet    = 1;
    s->packets_written = 0;

    if (s->bsf)
        av_bsf_flush(s->bsf);

    ffaml_start_writer(avctx);

    av_log(avctx, AV_LOG_DEBUG, "Flush complete\n");
}

static const AVCodecHWConfigInternal *const aml_hw_configs[] = {
    HW_CONFIG_INTERNAL(AML),
    NULL
};

#define FFAML_DEC_CLASS(NAME) \
    static const AVClass ffaml_##NAME##_dec_class = { \
        .class_name = "aml_" #NAME "_dec", \
        .version    = LIBAVUTIL_VERSION_INT, \
    };

#define FFAML_DEC(NAME, ID) \
    FFAML_DEC_CLASS(NAME) \
    const FFCodec ff_##NAME##_aml_decoder = { \
        .p.name         = #NAME "_aml", \
        CODEC_LONG_NAME(#NAME " (aml)"), \
        .p.type         = AVMEDIA_TYPE_VIDEO, \
        .p.id           = ID, \
        .priv_data_size = sizeof(AMLDecodeContext), \
        .init           = ffaml_init_decoder, \
        .close          = ffaml_close_decoder, \
        FF_CODEC_DECODE_CB(ffaml_decode), \
        .flush          = ffaml_flush, \
        .p.priv_class   = &ffaml_##NAME##_dec_class, \
        .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_HARDWARE, \
        .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE | \
                          FF_CODEC_CAP_SETS_PKT_DTS, \
        .hw_configs     = aml_hw_configs, \
        .p.wrapper_name = "aml", \
    };

FFAML_DEC(h264,      AV_CODEC_ID_H264)
FFAML_DEC(hevc,      AV_CODEC_ID_HEVC)
FFAML_DEC(mpeg2,     AV_CODEC_ID_MPEG2VIDEO)
FFAML_DEC(mpeg4,     AV_CODEC_ID_MPEG4)
FFAML_DEC(msmpeg4v1, AV_CODEC_ID_MSMPEG4V1)
FFAML_DEC(msmpeg4v2, AV_CODEC_ID_MSMPEG4V2)
FFAML_DEC(msmpeg4v3, AV_CODEC_ID_MSMPEG4V3)
FFAML_DEC(vc1,       AV_CODEC_ID_VC1)
FFAML_DEC(mjpeg,     AV_CODEC_ID_MJPEG)
FFAML_DEC(rv30,      AV_CODEC_ID_RV30)
FFAML_DEC(rv40,      AV_CODEC_ID_RV40)
