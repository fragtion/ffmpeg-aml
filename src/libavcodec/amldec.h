#ifndef _AMLDEC_H_
#define _AMLDEC_H_

#include "amlqueue.h"
#include <amcodec/codec.h>
#include <libavutil/buffer.h>
#include <pthread.h>
#include <time.h>

#define EXTERNAL_PTS    1
#define SYNC_OUTSIDE    2

#define PTS_FREQ        90000
#define MAX_HEADER_SIZE 4096

/* Upper bound on packets buffered between the FFmpeg decode callback and the
 * VPU writer thread. With realtime input (-re, or a live source) the queue
 * stays near-empty and nothing is ever dropped. If the queue hits this cap
 * (input faster than the VPU drains, e.g. a file without -re) the oldest
 * packet is dropped rather than blocking the decode callback — blocking there
 * would stall FFmpeg's shared demuxer and starve a concurrent audio output
 * (see ffaml_enqueue()). */
#define MAX_QUEUE_PACKETS 256

typedef struct {
    char data[MAX_HEADER_SIZE];
    int  size;
} AMLHeader;

typedef struct {
    AVClass        *av_class;
    codec_para_t    codec;
    int             first_packet;
    AVBSFContext   *bsf;
    struct buf_status buffer_status;
    int             packets_written;
    int             frame_count;

    /* packet queue + writer thread: the decode callback enqueues and returns
     * immediately (never blocking); the writer thread paces packets into the
     * VPU. Blocking the callback would stall FFmpeg's shared demuxer thread and
     * starve a concurrent audio output, so on overflow we drop instead. */
    PacketQueue     framequeue;
    pthread_t       writer_thread;
    pthread_mutex_t queue_mutex;
    pthread_cond_t  queue_cond;    /* signalled when a packet is enqueued */
    int             writer_run;    /* writer keeps looping while non-zero */
    int             writer_abort;  /* discard queued packets without writing */
    int             writer_started;
    unsigned        dropped_packets; /* video packets dropped on queue overflow */

    /* extradata used to build the prefeed header (from bsf par_out or avctx) */
    uint8_t        *extradata;
    int             extradata_size;

    int             amv_fd;
    double          decoder_buffer_pc;

    /* The VPU renders directly to HDMI and the decoder produces no real frames.
     * But an FFmpeg video output (e.g. -f null) needs at least one frame to
     * initialise its encoder, and the scheduler gates *other* outputs (a
     * concurrent ALSA audio output) on that init. So we hand back a lightweight,
     * refcounted blank frame per packet to keep the video output live and the
     * audio flowing. The picture on screen still comes from the VPU. */
    AVFrame        *blank;
} AMLDecodeContext;

int  ffmal_init_bitstream(AVCodecContext *avctx);
int  ffaml_write_codec_data(AVCodecContext *avctx, char *data, int size);
void ffaml_checkin_packet_pts(AVCodecContext *avctx, AVPacket *avpkt);
void ffaml_create_prefeed_header(AVCodecContext *avctx, AVPacket *pkt,
                                  AMLHeader *header, char *extradata,
                                  int extradatasize);
void ffaml_get_packet_header(AVCodecContext *avctx, AMLHeader *header,
                              AVPacket *pkt);
void ffaml_log_decoder_info(AVCodecContext *avctx);

#endif /* _AMLDEC_H_ */
