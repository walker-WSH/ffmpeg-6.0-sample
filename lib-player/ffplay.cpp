/*
 * Copyright (c) 2003 Fabrice Bellard
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
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

 /**
  * @file
  * simple media player based on the FFmpeg libraries
  */

#include "config.h"
#include "config_components.h"

#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>

#include <SDL.h>
#include <SDL_thread.h>

#if defined(__cplusplus)
extern "C" {
#endif
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/fifo.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"
#include "libavutil/error.h"

#if CONFIG_AVFILTER
#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#endif

#include "cmdutils.h"
#if defined(__cplusplus)
};
#endif

#include "ffplay.h"
#include "task-instance.h"
#include "auto-run.hpp"


#pragma warning(disable : 4244)
#pragma warning(disable : 4018)
#pragma warning(disable : 4267)

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_FRAMES 25
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000

#define USE_ONEPASS_SUBTITLE_RENDER 1

typedef struct MyAVPacketList {
        AVPacket* pkt;
        int serial;
} MyAVPacketList;

typedef struct PacketQueue {
        AVFifo* pkt_list;
        int nb_packets;
        int size;
        int64_t duration;
        int abort_request;
        int serial;
        SDL_mutex* mutex;
        SDL_cond* cond;
} PacketQueue;

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

/*
typedef struct AudioParams {
        int freq;
        AVChannelLayout ch_layout;
        enum AVSampleFormat fmt;
        int frame_size;
        int bytes_per_sec;
} AudioParams;
*/

typedef struct Clock {
        double pts;           /* clock base */
        double pts_drift;     /* clock base minus time at which we updated the clock */
        double last_updated;
        double speed;
        int serial;           /* clock is based on a packet with this serial */
        int paused;
        int* queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
        AVFrame* frame;
        AVSubtitle sub;
        int serial;
        double pts;           /* presentation timestamp for the frame */
        double duration;      /* estimated duration of the frame */
        int64_t pos;          /* byte position of the frame in the input file */
        int width;
        int height;
        int format;
        AVRational sar;
        int uploaded;
        int flip_v;
} Frame;

typedef struct FrameQueue {
        Frame queue[FRAME_QUEUE_SIZE];
        int rindex;
        int windex;
        int size;
        int max_size;
        int keep_last;
        int rindex_shown;
        SDL_mutex* mutex;
        SDL_cond* cond;
        PacketQueue* pktq;
} FrameQueue;

enum ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
};

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

typedef struct Decoder {
        AVPacket* pkt;
        PacketQueue* queue;
        AVCodecContext* avctx;
        int pkt_serial;
        int finished;
        int packet_pending;
        SDL_cond* empty_queue_cond;
        int64_t start_pts;
        AVRational start_pts_tb;
        int64_t next_pts;
        AVRational next_pts_tb;
        SDL_Thread* decoder_tid;
} Decoder;

typedef struct VideoState {
        SDL_Thread* read_tid;
        const AVInputFormat* iformat;
        int abort_request;
        int force_refresh;
        int paused;
        int last_paused;
        int queue_attachments_req;
        int seek_req;
        int seek_flags;
        int64_t seek_pos;
        int64_t seek_rel;
        int read_pause_return;
        AVFormatContext* ic;
        int realtime;

        Clock audclk;
        Clock vidclk;
        Clock extclk;

        FrameQueue pictq;
        FrameQueue subpq;
        FrameQueue sampq;

        Decoder auddec;
        Decoder viddec;
        Decoder subdec;

        int audio_stream;

        int av_sync_type;

        double audio_clock;
        int audio_clock_serial;
        double audio_diff_cum; /* used for AV difference average computation */
        double audio_diff_avg_coef;
        double audio_diff_threshold;
        int audio_diff_avg_count;
        AVStream* audio_st;
        PacketQueue audioq;
        int audio_hw_buf_size;
        uint8_t* audio_buf;
        uint8_t* audio_buf1;
        unsigned int audio_buf_size; /* in bytes */
        unsigned int audio_buf1_size;
        int audio_buf_index; /* in bytes */
        int audio_write_buf_size;
        int audio_volume;
        int muted;
        struct AudioParams audio_src;
#if CONFIG_AVFILTER
        struct AudioParams audio_filter_src;
#endif
        struct AudioParams audio_tgt;
        struct SwrContext* swr_ctx;
        int frame_drops_early;
        int frame_drops_late;

        enum ShowMode show_mode;
        int16_t sample_array[SAMPLE_ARRAY_SIZE];
        int sample_array_index;
        int last_i_start;
        RDFTContext* rdft;
        int rdft_bits;
        FFTSample* rdft_data;
        int xpos;
        double last_vis_time;
        SDL_Texture* vis_texture;
        SDL_Texture* sub_texture;
        SDL_Texture* vid_texture;

        int subtitle_stream;
        AVStream* subtitle_st;
        PacketQueue subtitleq;

        double frame_timer;
        double frame_last_returned_time;
        double frame_last_filter_delay;
        int video_stream;
        AVStream* video_st;
        PacketQueue videoq;
        double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
        struct SwsContext* img_convert_ctx;
        struct SwsContext* sub_convert_ctx;
        int eof;

        char* filename;
        int width, height, xleft, ytop;
        int step;

#if CONFIG_AVFILTER
        int vfilter_idx;
        AVFilterContext* in_video_filter;   // the first filter in the video chain
        AVFilterContext* out_video_filter;  // the last filter in the video chain
        AVFilterContext* in_audio_filter;   // the first filter in the audio chain
        AVFilterContext* out_audio_filter;  // the last filter in the audio chain
        AVFilterGraph* agraph;              // audio filter graph
#endif

        int last_video_stream, last_audio_stream, last_subtitle_stream;

        SDL_cond* continue_read_thread;
} VideoState;

unsigned sws_flags = SWS_BICUBIC;

const struct TextureFormatEntry {
        enum AVPixelFormat format;
        int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};


//-------------------------------------------------------------------------------------------------------------------------------------
class ffplayer : public ffplayer_interface {
public:
    explicit ffplayer(std::weak_ptr<ffplayer_event> cb) : event_cb(cb) {
    }

    virtual ~ffplayer() {
        assert(abort_play && event_loop_tid == nullptr);
        do_exit();
    }

private:
        static int s_event_loop(void* arg);
        static int s_read_thread(void* arg);
        static int s_audio_thread(void* arg);
        static int s_video_thread(void* arg);
        static int s_subtitle_thread(void* arg);
        static void s_sdl_audio_callback(void* opaque, Uint8* stream, int len);
        static int s_decode_interrupt_cb(void* ctx);

        VideoState* video_state = nullptr;

        // for hw by walker-WSH
        bool request_hw_decode = false;
        bool hw_decode_used = false;
        AVBufferRef* hw_device_buf = nullptr;
        enum AVPixelFormat hw_format = AVPixelFormat::AV_PIX_FMT_NONE;

        // for send audio by walker-WSH
        std::thread pop_audio_handle;

        // added by walker-WSH
        std::atomic<bool> stream_ready = false;
        std::atomic<bool> video_included = false;
        std::atomic<bool> audio_included = false;
        std::atomic<double> duration_seconds = 0.0;
        const std::weak_ptr<ffplayer_event> event_cb;

        bool abort_play = false;
        SDL_Thread* event_loop_tid = nullptr;

        task_instance task_pool;

        AVDictionary* sws_dict = NULL;
        AVDictionary* swr_opts = NULL;
        AVDictionary* format_opts = NULL;
        AVDictionary* codec_opts = NULL;

        const AVInputFormat* file_iformat = nullptr;
        char* input_filename = nullptr;

        int default_width = 640;
        int default_height = 480;
        int screen_width = 0;
        int screen_height = 0;
        int screen_left = SDL_WINDOWPOS_CENTERED;
        int screen_top = SDL_WINDOWPOS_CENTERED;

        int display_disable = 0;
        int audio_disable = 0;
        int video_disable = 0;
        int subtitle_disable = 1; // disable subtitle

        const char* wanted_stream_spec[AVMEDIA_TYPE_NB] = { 0 };
        int seek_by_bytes = -1;
        float seek_interval = 10;
        int startup_volume = 100;
        int av_sync_type = AV_SYNC_AUDIO_MASTER;
        int64_t start_time = AV_NOPTS_VALUE; // = seconds * 1000000;
        int64_t duration = AV_NOPTS_VALUE; // the duration we want to play
        int fast = 0;
        int genpts = 0;
        int lowres = 0;
        int decoder_reorder_pts = -1;
        int autoexit = 0;
        int loop = 1;
        int framedrop = -1;
        int infinite_buffer = -1;
        enum ShowMode show_mode = SHOW_MODE_NONE;

        const char* audio_codec_name = nullptr;
        const char* subtitle_codec_name = nullptr;
        const char* video_codec_name = nullptr;

        double rdftspeed = 0.02;
        int64_t cursor_last_shown = 0;
        int cursor_hidden = 0;
#if CONFIG_AVFILTER        
        std::string afilters = ""; // such as "atempo=tempo=3,volume=0.3"
        std::string vfilters_list = ""; // such as "setpts=PTS*0.5,vflip"
        int nb_vfilters = 0; // 1 represent video filter is included, no matter how many
#endif
        int autorotate = 1;
        int find_stream_info = 1;
        int filter_nbthreads = 0;

        /* current context */
        int is_full_screen = 0;
        int64_t audio_callback_time = 0;

        SDL_Window* window = nullptr;
        SDL_Renderer* renderer = nullptr;
        SDL_RendererInfo renderer_info = { 0 };
        SDL_AudioDeviceID audio_dev = 0;

        int64_t last_mouse_left_click = 0;

        //-------------------------------------------------------------------------------------------------------------------------------------
public:
        int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                enum AVSampleFormat fmt2, int64_t channel_count2)
        {
                /* If channel count == 1, planar and non-planar formats are the same */
                if (channel_count1 == 1 && channel_count2 == 1)
                        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
                else
                        return channel_count1 != channel_count2 || fmt1 != fmt2;
        }

        int packet_queue_put_private(PacketQueue* q, AVPacket* pkt)
        {
                MyAVPacketList pkt1;
                int ret;

                if (q->abort_request)
                        return -1;


                pkt1.pkt = pkt;
                pkt1.serial = q->serial;

                ret = av_fifo_write(q->pkt_list, &pkt1, 1);
                if (ret < 0)
                        return ret;
                q->nb_packets++;
                q->size += pkt1.pkt->size + sizeof(pkt1);
                q->duration += pkt1.pkt->duration;
                /* XXX: should duplicate packet data in DV case */
                SDL_CondSignal(q->cond);
                return 0;
        }

        int packet_queue_put(PacketQueue* q, AVPacket* pkt)
        {
                AVPacket* pkt1;
                int ret;

                pkt1 = av_packet_alloc();
                if (!pkt1) {
                        av_packet_unref(pkt);
                        return -1;
                }
                av_packet_move_ref(pkt1, pkt);

                SDL_LockMutex(q->mutex);
                ret = packet_queue_put_private(q, pkt1);
                SDL_UnlockMutex(q->mutex);

                if (ret < 0)
                        av_packet_free(&pkt1);

                return ret;
        }

        int packet_queue_put_nullpacket(PacketQueue* q, AVPacket* pkt, int stream_index)
        {
                pkt->stream_index = stream_index;
                return packet_queue_put(q, pkt);
        }

        /* packet queue handling */
        int packet_queue_init(PacketQueue* q)
        {
                memset(q, 0, sizeof(PacketQueue));
                q->pkt_list = av_fifo_alloc2(1, sizeof(MyAVPacketList), AV_FIFO_FLAG_AUTO_GROW);
                if (!q->pkt_list)
                        return AVERROR(ENOMEM);
                q->mutex = SDL_CreateMutex();
                if (!q->mutex) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
                        return AVERROR(ENOMEM);
                }
                q->cond = SDL_CreateCond();
                if (!q->cond) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
                        return AVERROR(ENOMEM);
                }
                q->abort_request = 1;
                return 0;
        }

        void packet_queue_flush(PacketQueue* q)
        {
                MyAVPacketList pkt1;

                SDL_LockMutex(q->mutex);
                while (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0)
                        av_packet_free(&pkt1.pkt);
                q->nb_packets = 0;
                q->size = 0;
                q->duration = 0;
                q->serial++;
                SDL_UnlockMutex(q->mutex);
        }

        void packet_queue_destroy(PacketQueue* q)
        {
                packet_queue_flush(q);
                av_fifo_freep2(&q->pkt_list);
                SDL_DestroyMutex(q->mutex);
                SDL_DestroyCond(q->cond);
        }

        void packet_queue_abort(PacketQueue* q)
        {
                SDL_LockMutex(q->mutex);

                q->abort_request = 1;

                SDL_CondSignal(q->cond);

                SDL_UnlockMutex(q->mutex);
        }

        void packet_queue_start(PacketQueue* q)
        {
                SDL_LockMutex(q->mutex);
                q->abort_request = 0;
                q->serial++;
                SDL_UnlockMutex(q->mutex);
        }

        /* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
        int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block, int* serial)
        {
                MyAVPacketList pkt1;
                int ret;

                SDL_LockMutex(q->mutex);

                for (;;) {
                        if (q->abort_request) {
                                ret = -1;
                                break;
                        }

                        if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
                                q->nb_packets--;
                                q->size -= pkt1.pkt->size + sizeof(pkt1);
                                q->duration -= pkt1.pkt->duration;
                                av_packet_move_ref(pkt, pkt1.pkt);
                                if (serial)
                                        *serial = pkt1.serial;
                                av_packet_free(&pkt1.pkt);
                                ret = 1;
                                break;
                        }
                        else if (!block) {
                                ret = 0;
                                break;
                        }
                        else {
                                SDL_CondWait(q->cond, q->mutex);
                        }
                }
                SDL_UnlockMutex(q->mutex);
                return ret;
        }

        int decoder_init(Decoder* d, AVCodecContext* avctx, PacketQueue* queue, SDL_cond* empty_queue_cond) {
                memset(d, 0, sizeof(Decoder));
                d->pkt = av_packet_alloc();
                if (!d->pkt)
                        return AVERROR(ENOMEM);
                d->avctx = avctx;
                d->queue = queue;
                d->empty_queue_cond = empty_queue_cond;
                d->start_pts = AV_NOPTS_VALUE;
                d->pkt_serial = -1;
                return 0;
        }

        int decoder_decode_frame(Decoder* d, AVFrame* frame, AVSubtitle* sub) {
                int ret = AVERROR(EAGAIN);

                for (;;) {
                        if (d->queue->serial == d->pkt_serial) {
                                do {
                                        if (d->queue->abort_request)
                                                return -1;

                                        switch (d->avctx->codec_type) {
                                        case AVMEDIA_TYPE_VIDEO:
                                                ret = avcodec_receive_frame(d->avctx, frame);
                                                if (ret >= 0) {
                                                        if (decoder_reorder_pts == -1) {
                                                                frame->pts = frame->best_effort_timestamp;
                                                        }
                                                        else if (!decoder_reorder_pts) {
                                                                frame->pts = frame->pkt_dts;
                                                        }
                                                }
                                                break;
                                        case AVMEDIA_TYPE_AUDIO:
                                                ret = avcodec_receive_frame(d->avctx, frame);
                                                if (ret >= 0) {
                                                        AVRational tb = AVRational(1, frame->sample_rate);
                                                        if (frame->pts != AV_NOPTS_VALUE)
                                                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                                                        else if (d->next_pts != AV_NOPTS_VALUE)
                                                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                                                        if (frame->pts != AV_NOPTS_VALUE) {
                                                                d->next_pts = frame->pts + frame->nb_samples;
                                                                d->next_pts_tb = tb;
                                                        }
                                                }
                                                break;
                                        }
                                        if (ret == AVERROR_EOF) {
                                                d->finished = d->pkt_serial;
                                                avcodec_flush_buffers(d->avctx);
                                                return 0;
                                        }
                                        if (ret >= 0)
                                                return 1;
                                } while (ret != AVERROR(EAGAIN));
                        }

                        do {
                                if (d->queue->nb_packets == 0)
                                        SDL_CondSignal(d->empty_queue_cond);
                                if (d->packet_pending) {
                                        d->packet_pending = 0;
                                }
                                else {
                                        int old_serial = d->pkt_serial;
                                        if (packet_queue_get(d->queue, d->pkt, 1, &d->pkt_serial) < 0)
                                                return -1;
                                        if (old_serial != d->pkt_serial) {
                                                avcodec_flush_buffers(d->avctx);
                                                d->finished = 0;
                                                d->next_pts = d->start_pts;
                                                d->next_pts_tb = d->start_pts_tb;
                                        }
                                }
                                if (d->queue->serial == d->pkt_serial)
                                        break;
                                av_packet_unref(d->pkt);
                        } while (1);

                        if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                                int got_frame = 0;
                                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, d->pkt);
                                if (ret < 0) {
                                        ret = AVERROR(EAGAIN);
                                }
                                else {
                                        if (got_frame && !d->pkt->data) {
                                                d->packet_pending = 1;
                                        }
                                        ret = got_frame ? 0 : (d->pkt->data ? AVERROR(EAGAIN) : AVERROR_EOF);
                                }
                                av_packet_unref(d->pkt);
                        }
                        else {
                                if (avcodec_send_packet(d->avctx, d->pkt) == AVERROR(EAGAIN)) {
                                        av_log_ffplay(d->avctx, AV_LOG_ERROR, "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                                        d->packet_pending = 1;
                                }
                                else {
                                        av_packet_unref(d->pkt);
                                }
                        }
                }
        }

        void decoder_destroy(Decoder* d) {
                av_packet_free(&d->pkt);
                avcodec_free_context(&d->avctx);
        }

        void frame_queue_unref_item(Frame* vp)
        {
                av_frame_unref(vp->frame);
                avsubtitle_free(&vp->sub);
        }

        int frame_queue_init(FrameQueue* f, PacketQueue* pktq, int max_size, int keep_last)
        {
                int i;
                memset(f, 0, sizeof(FrameQueue));
                if (!(f->mutex = SDL_CreateMutex())) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
                        return AVERROR(ENOMEM);
                }
                if (!(f->cond = SDL_CreateCond())) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
                        return AVERROR(ENOMEM);
                }
                f->pktq = pktq;
                f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
                f->keep_last = !!keep_last;
                for (i = 0; i < f->max_size; i++)
                        if (!(f->queue[i].frame = av_frame_alloc()))
                                return AVERROR(ENOMEM);
                return 0;
        }

        void frame_queue_destory(FrameQueue* f)
        {
                int i;
                for (i = 0; i < f->max_size; i++) {
                        Frame* vp = &f->queue[i];
                        frame_queue_unref_item(vp);
                        av_frame_free(&vp->frame);
                }
                SDL_DestroyMutex(f->mutex);
                SDL_DestroyCond(f->cond);
        }

        void frame_queue_signal(FrameQueue* f)
        {
                SDL_LockMutex(f->mutex);
                SDL_CondSignal(f->cond);
                SDL_UnlockMutex(f->mutex);
        }

        Frame* frame_queue_peek(FrameQueue* f)
        {
                return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
        }

        Frame* frame_queue_peek_next(FrameQueue* f)
        {
                return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
        }

        Frame* frame_queue_peek_last(FrameQueue* f)
        {
                return &f->queue[f->rindex];
        }

        Frame* frame_queue_peek_writable(FrameQueue* f)
        {
                /* wait until we have space to put a new frame */
                SDL_LockMutex(f->mutex);
                while (f->size >= f->max_size &&
                        !f->pktq->abort_request) {
                        SDL_CondWait(f->cond, f->mutex);
                }
                SDL_UnlockMutex(f->mutex);

                if (f->pktq->abort_request)
                        return NULL;

                return &f->queue[f->windex];
        }

        Frame* frame_queue_peek_readable(FrameQueue* f)
        {
                /* wait until we have a readable a new frame */
                SDL_LockMutex(f->mutex);
                while (f->size - f->rindex_shown <= 0 &&
                        !f->pktq->abort_request) {
                        SDL_CondWait(f->cond, f->mutex);
                }
                SDL_UnlockMutex(f->mutex);

                if (f->pktq->abort_request)
                        return NULL;

                return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
        }

        void frame_queue_push(FrameQueue* f)
        {
                if (++f->windex == f->max_size)
                        f->windex = 0;
                SDL_LockMutex(f->mutex);
                f->size++;
                SDL_CondSignal(f->cond);
                SDL_UnlockMutex(f->mutex);
        }

        void frame_queue_next(FrameQueue* f)
        {
                if (f->keep_last && !f->rindex_shown) {
                        f->rindex_shown = 1;
                        return;
                }
                frame_queue_unref_item(&f->queue[f->rindex]);
                if (++f->rindex == f->max_size)
                        f->rindex = 0;
                SDL_LockMutex(f->mutex);
                f->size--;
                SDL_CondSignal(f->cond);
                SDL_UnlockMutex(f->mutex);
        }

        /* return the number of undisplayed frames in the queue */
        int frame_queue_nb_remaining(FrameQueue* f)
        {
                return f->size - f->rindex_shown;
        }

        /* return last shown position */
        int64_t frame_queue_last_pos(FrameQueue* f)
        {
                Frame* fp = &f->queue[f->rindex];
                if (f->rindex_shown && fp->serial == f->pktq->serial)
                        return fp->pos;
                else
                        return -1;
        }

        void decoder_abort(Decoder* d, FrameQueue* fq)
        {
                packet_queue_abort(d->queue);
                frame_queue_signal(fq);
                SDL_WaitThread(d->decoder_tid, NULL);
                d->decoder_tid = NULL;
                packet_queue_flush(d->queue);
        }

        inline void fill_rectangle(int x, int y, int w, int h)
        {
                SDL_Rect rect;
                rect.x = x;
                rect.y = y;
                rect.w = w;
                rect.h = h;
                if (w && h)
                        SDL_RenderFillRect(renderer, &rect);
        }

        int realloc_texture(SDL_Texture** texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode, int init_texture)
        {
                Uint32 format;
                int access, w, h;
                if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h || new_format != format) {
                        void* pixels;
                        int pitch;
                        if (*texture)
                                SDL_DestroyTexture(*texture);
                        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
                                return -1;
                        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
                                return -1;
                        if (init_texture) {
                                if (SDL_LockTexture(*texture, NULL, &pixels, &pitch) < 0)
                                        return -1;
                                memset(pixels, 0, pitch * new_height);
                                SDL_UnlockTexture(*texture);
                        }
                        av_log_ffplay(NULL, AV_LOG_DEBUG, "Created %dx%d texture with %s.\n", new_width, new_height, SDL_GetPixelFormatName(new_format));
                }
                return 0;
        }

        void calculate_display_rect(SDL_Rect* rect,
                int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                int pic_width, int pic_height, AVRational pic_sar)
        {
                AVRational aspect_ratio = pic_sar;
                int64_t width, height, x, y;

                if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
                        aspect_ratio = av_make_q(1, 1);

                aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

                /* XXX: we suppose the screen has a 1.0 pixel ratio */
                height = scr_height;
                width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
                if (width > scr_width) {
                        width = scr_width;
                        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
                }
                x = (scr_width - width) / 2;
                y = (scr_height - height) / 2;
                rect->x = scr_xleft + x;
                rect->y = scr_ytop + y;
                rect->w = FFMAX((int)width, 1);
                rect->h = FFMAX((int)height, 1);
        }

        void get_sdl_pix_fmt_and_blendmode(int format, Uint32* sdl_pix_fmt, SDL_BlendMode* sdl_blendmode)
        {
                int i;
                *sdl_blendmode = SDL_BLENDMODE_NONE;
                *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
                if (format == AV_PIX_FMT_RGB32 ||
                        format == AV_PIX_FMT_RGB32_1 ||
                        format == AV_PIX_FMT_BGR32 ||
                        format == AV_PIX_FMT_BGR32_1)
                        *sdl_blendmode = SDL_BLENDMODE_BLEND;
                for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
                        if (format == sdl_texture_format_map[i].format) {
                                *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
                                return;
                        }
                }
        }

        int upload_texture(SDL_Texture** tex, AVFrame* frame, struct SwsContext** img_convert_ctx) {
                int ret = 0;
                Uint32 sdl_pix_fmt;
                SDL_BlendMode sdl_blendmode;
                get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
                if (realloc_texture(tex, sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt, frame->width, frame->height, sdl_blendmode, 0) < 0)
                        return -1;
                switch (sdl_pix_fmt) {
                case SDL_PIXELFORMAT_UNKNOWN:
                        /* This should only happen if we are not using avfilter... */
                        *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                                frame->width, frame->height, (enum AVPixelFormat)frame->format, frame->width, frame->height,
                                AV_PIX_FMT_BGRA, sws_flags, NULL, NULL, NULL);
                        if (*img_convert_ctx != NULL) {
                                uint8_t* pixels[4];
                                int pitch[4];
                                if (!SDL_LockTexture(*tex, NULL, (void**)pixels, pitch)) {
                                        sws_scale(*img_convert_ctx, (const uint8_t* const*)frame->data, frame->linesize,
                                                0, frame->height, pixels, pitch);
                                        SDL_UnlockTexture(*tex);
                                }
                        }
                        else {
                                av_log_ffplay(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                                ret = -1;
                        }
                        break;
                case SDL_PIXELFORMAT_IYUV:
                        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0], frame->linesize[0],
                                        frame->data[1], frame->linesize[1],
                                        frame->data[2], frame->linesize[2]);
                        }
                        else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                                ret = SDL_UpdateYUVTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0],
                                        frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[1],
                                        frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1), -frame->linesize[2]);
                        }
                        else {
                                av_log_ffplay(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                                return -1;
                        }
                        break;
                default:
                        if (frame->linesize[0] < 0) {
                                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
                        }
                        else {
                                ret = SDL_UpdateTexture(*tex, NULL, frame->data[0], frame->linesize[0]);
                        }
                        break;
                }
                return ret;
        }

        void set_sdl_yuv_conversion_mode(AVFrame* frame)
        {
#if SDL_VERSION_ATLEAST(2,0,8)
                SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
                if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
                        if (frame->color_range == AVCOL_RANGE_JPEG)
                                mode = SDL_YUV_CONVERSION_JPEG;
                        else if (frame->colorspace == AVCOL_SPC_BT709)
                                mode = SDL_YUV_CONVERSION_BT709;
                        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
                                mode = SDL_YUV_CONVERSION_BT601;
                }
                SDL_SetYUVConversionMode(mode); /* FIXME: no support for linear transfer */
#endif
        }

        void video_image_display(VideoState* is)
        {
                Frame* vp;
                Frame* sp = NULL;
                SDL_Rect rect;

                vp = frame_queue_peek_last(&is->pictq);

                if (vp) {
#if defined(DEBUG_SYNC)
                    // pts : 此处已经是经timebase转换过的时间戳，单位s
                    // src_frame: 此处已经是经过同步的视频帧 应该尽快推送渲染
                    av_log_ffplay(NULL, AV_LOG_INFO, "--- pop video, pts=%0.3f type:%c format:%d \n", vp->pts, av_get_picture_type_char(vp->frame->pict_type), vp->frame->format);
#endif
                }

                if (is->subtitle_st) {
                        if (frame_queue_nb_remaining(&is->subpq) > 0) {
                                sp = frame_queue_peek(&is->subpq);

                                if (vp->pts >= sp->pts + ((float)sp->sub.start_display_time / 1000)) {
                                        if (!sp->uploaded) {
                                                uint8_t* pixels[4];
                                                int pitch[4];
                                                int i;
                                                if (!sp->width || !sp->height) {
                                                        sp->width = vp->width;
                                                        sp->height = vp->height;
                                                }
                                                if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height, SDL_BLENDMODE_BLEND, 1) < 0)
                                                        return;

                                                for (i = 0; i < sp->sub.num_rects; i++) {
                                                        AVSubtitleRect* sub_rect = sp->sub.rects[i];

                                                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                                                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                                                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                                                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                                                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                                sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                                sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                                0, NULL, NULL, NULL);
                                                        if (!is->sub_convert_ctx) {
                                                                av_log_ffplay(NULL, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                                                                return;
                                                        }
                                                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)pixels, pitch)) {
                                                                sws_scale(is->sub_convert_ctx, (const uint8_t* const*)sub_rect->data, sub_rect->linesize,
                                                                        0, sub_rect->h, pixels, pitch);
                                                                SDL_UnlockTexture(is->sub_texture);
                                                        }
                                                }
                                                sp->uploaded = 1;
                                        }
                                }
                                else
                                        sp = NULL;
                        }
                }

                calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);
                set_sdl_yuv_conversion_mode(vp->frame);

                if (!vp->uploaded) {
                        if (upload_texture(&is->vid_texture, vp->frame, &is->img_convert_ctx) < 0) {
                                set_sdl_yuv_conversion_mode(NULL);
                                return;
                        }
                        vp->uploaded = 1;
                        vp->flip_v = vp->frame->linesize[0] < 0;
                }

                SDL_RenderCopyEx(renderer, is->vid_texture, NULL, &rect, 0, NULL, vp->flip_v ? SDL_FLIP_VERTICAL : SDL_FLIP_NONE);
                set_sdl_yuv_conversion_mode(NULL);
                if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
                        SDL_RenderCopy(renderer, is->sub_texture, NULL, &rect);
#else
                        int i;
                        double xratio = (double)rect.w / (double)sp->width;
                        double yratio = (double)rect.h / (double)sp->height;
                        for (i = 0; i < sp->sub.num_rects; i++) {
                                SDL_Rect* sub_rect = (SDL_Rect*)sp->sub.rects[i];
                                SDL_Rect target = { .x = rect.x + sub_rect->x * xratio,
                                                   .y = rect.y + sub_rect->y * yratio,
                                                   .w = sub_rect->w * xratio,
                                                   .h = sub_rect->h * yratio };
                                SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
                        }
#endif
                }
        }

        inline int compute_mod(int a, int b)
        {
                return a < 0 ? a % b + b : a % b;
        }

        void video_audio_display(VideoState* s)
        {
                int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
                int ch, channels, h, h2;
                int64_t time_diff;
                int rdft_bits, nb_freq;

                for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++)
                        ;
                nb_freq = 1 << (rdft_bits - 1);

                /* compute display index : center on currently output samples */
                channels = s->audio_tgt.ch_layout.nb_channels;
                nb_display_channels = channels;
                if (!s->paused) {
                        int data_used = s->show_mode == SHOW_MODE_WAVES ? s->width : (2 * nb_freq);
                        n = 2 * channels;
                        delay = s->audio_write_buf_size;
                        delay /= n;

                        /* to be more precise, we take into account the time spent since
                           the last buffer computation */
                        if (audio_callback_time) {
                                time_diff = av_gettime_relative() - audio_callback_time;
                                delay -= (time_diff * s->audio_tgt.freq) / 1000000;
                        }

                        delay += 2 * data_used;
                        if (delay < data_used)
                                delay = data_used;

                        i_start = x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
                        if (s->show_mode == SHOW_MODE_WAVES) {
                                h = INT_MIN;
                                for (i = 0; i < 1000; i += channels) {
                                        int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                                        int a = s->sample_array[idx];
                                        int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                                        int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                                        int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                                        int score = a - d;
                                        if (h < score && (b ^ c) < 0) {
                                                h = score;
                                                i_start = idx;
                                        }
                                }
                        }

                        s->last_i_start = i_start;
                }
                else {
                        i_start = s->last_i_start;
                }

                if (s->show_mode == SHOW_MODE_WAVES) {
                        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

                        /* total height for one channel */
                        h = s->height / nb_display_channels;
                        /* graph height / 2 */
                        h2 = (h * 9) / 20;
                        for (ch = 0; ch < nb_display_channels; ch++) {
                                i = i_start + ch;
                                y1 = s->ytop + ch * h + (h / 2); /* position of center line */
                                for (x = 0; x < s->width; x++) {
                                        y = (s->sample_array[i] * h2) >> 15;
                                        if (y < 0) {
                                                y = -y;
                                                ys = y1 - y;
                                        }
                                        else {
                                                ys = y1;
                                        }
                                        fill_rectangle(s->xleft + x, ys, 1, y);
                                        i += channels;
                                        if (i >= SAMPLE_ARRAY_SIZE)
                                                i -= SAMPLE_ARRAY_SIZE;
                                }
                        }

                        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

                        for (ch = 1; ch < nb_display_channels; ch++) {
                                y = s->ytop + ch * h;
                                fill_rectangle(s->xleft, y, s->width, 1);
                        }
                }
                else {
                        if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
                                return;

                        if (s->xpos >= s->width)
                                s->xpos = 0;
                        nb_display_channels = FFMIN(nb_display_channels, 2);
                        if (rdft_bits != s->rdft_bits) {
                                av_rdft_end(s->rdft);
                                av_free(s->rdft_data);
                                s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
                                s->rdft_bits = rdft_bits;
                                s->rdft_data = (FFTSample*)av_malloc_array(nb_freq, 4 * sizeof(*s->rdft_data));
                        }
                        if (!s->rdft || !s->rdft_data) {
                                av_log_ffplay(NULL, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
                                s->show_mode = SHOW_MODE_WAVES;
                        }
                        else {
                                FFTSample* data[2];
                                SDL_Rect rect = { .x = s->xpos, .y = 0, .w = 1, .h = s->height };
                                uint32_t* pixels;
                                int pitch;
                                for (ch = 0; ch < nb_display_channels; ch++) {
                                        data[ch] = s->rdft_data + 2 * nb_freq * ch;
                                        i = i_start + ch;
                                        for (x = 0; x < 2 * nb_freq; x++) {
                                                double w = (x - nb_freq) * (1.0 / nb_freq);
                                                data[ch][x] = s->sample_array[i] * (1.0 - w * w);
                                                i += channels;
                                                if (i >= SAMPLE_ARRAY_SIZE)
                                                        i -= SAMPLE_ARRAY_SIZE;
                                        }
                                        av_rdft_calc(s->rdft, data[ch]);
                                }
                                /* Least efficient way to do this, we should of course
                                 * directly access it but it is more than fast enough. */
                                if (!SDL_LockTexture(s->vis_texture, &rect, (void**)&pixels, &pitch)) {
                                        pitch >>= 2;
                                        pixels += pitch * s->height;
                                        for (y = 0; y < s->height; y++) {
                                                double w = 1 / sqrt(nb_freq);
                                                int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] + data[0][2 * y + 1] * data[0][2 * y + 1]));
                                                int b = (nb_display_channels == 2) ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
                                                        : a;
                                                a = FFMIN(a, 255);
                                                b = FFMIN(b, 255);
                                                pixels -= pitch;
                                                *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
                                        }
                                        SDL_UnlockTexture(s->vis_texture);
                                }
                                SDL_RenderCopy(renderer, s->vis_texture, NULL, NULL);
                        }
                        if (!s->paused)
                                s->xpos++;
                }
        }

        void stream_component_close(VideoState* is, int stream_index)
        {
                AVFormatContext* ic = is->ic;
                AVCodecParameters* codecpar;

                if (stream_index < 0 || stream_index >= ic->nb_streams)
                        return;
                codecpar = ic->streams[stream_index]->codecpar;

                switch (codecpar->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                        decoder_abort(&is->auddec, &is->sampq);

                        // for send audio by walker-WSH
                        if (pop_audio_handle.joinable()) {
                            pop_audio_handle.join();
                        }
                        if (audio_dev != 0) {
                            SDL_CloseAudioDevice(audio_dev);
                            audio_dev = 0;
                        }
                        
                        decoder_destroy(&is->auddec);
                        swr_free(&is->swr_ctx);
                        av_freep(&is->audio_buf1);
                        is->audio_buf1_size = 0;
                        is->audio_buf = NULL;

                        if (is->rdft) {
                                av_rdft_end(is->rdft);
                                av_freep(&is->rdft_data);
                                is->rdft = NULL;
                                is->rdft_bits = 0;
                        }
                        break;
                case AVMEDIA_TYPE_VIDEO:
                        decoder_abort(&is->viddec, &is->pictq);
                        decoder_destroy(&is->viddec);

                        // for hw by walker-WSH
                        av_buffer_unref(&hw_device_buf);
                        break;
                case AVMEDIA_TYPE_SUBTITLE:
                        decoder_abort(&is->subdec, &is->subpq);
                        decoder_destroy(&is->subdec);
                        break;
                default:
                        break;
                }

                ic->streams[stream_index]->discard = AVDISCARD_ALL;
                switch (codecpar->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                        is->audio_st = NULL;
                        is->audio_stream = -1;
                        break;
                case AVMEDIA_TYPE_VIDEO:
                        is->video_st = NULL;
                        is->video_stream = -1;
                        break;
                case AVMEDIA_TYPE_SUBTITLE:
                        is->subtitle_st = NULL;
                        is->subtitle_stream = -1;
                        break;
                default:
                        break;
                }
        }

        void stream_close(VideoState* is)
        {
                /* XXX: use a special url_shutdown call to abort parse cleanly */
                is->abort_request = 1;
                SDL_WaitThread(is->read_tid, NULL);

                /* close each stream */
                if (is->audio_stream >= 0)
                        stream_component_close(is, is->audio_stream);
                if (is->video_stream >= 0)
                        stream_component_close(is, is->video_stream);
                if (is->subtitle_stream >= 0)
                        stream_component_close(is, is->subtitle_stream);

                avformat_close_input(&is->ic);

                packet_queue_destroy(&is->videoq);
                packet_queue_destroy(&is->audioq);
                packet_queue_destroy(&is->subtitleq);

                /* free all pictures */
                frame_queue_destory(&is->pictq);
                frame_queue_destory(&is->sampq);
                frame_queue_destory(&is->subpq);
                SDL_DestroyCond(is->continue_read_thread);
                sws_freeContext(is->img_convert_ctx);
                sws_freeContext(is->sub_convert_ctx);
                av_free(is->filename);
                if (is->vis_texture)
                        SDL_DestroyTexture(is->vis_texture);
                if (is->vid_texture)
                        SDL_DestroyTexture(is->vid_texture);
                if (is->sub_texture)
                        SDL_DestroyTexture(is->sub_texture);
                av_free(is);
        }

        void uninit_opts(void)
        {
                av_dict_free(&swr_opts);
                av_dict_free(&sws_dict);
                av_dict_free(&format_opts);
                av_dict_free(&codec_opts);
        }

        void do_exit()
        {
                abort_play = true;

                if (video_state) {
                        stream_close(video_state);
                        video_state = nullptr;
                }

                if (input_filename) {
                        av_freep(&input_filename);
                        input_filename = nullptr;
                }

                if (swr_opts) {
                        av_dict_free(&swr_opts);
                        swr_opts = nullptr;
                }

                if (sws_dict) {
                        av_dict_free(&sws_dict);
                        sws_dict = nullptr;
                }

                if (format_opts) {
                        av_dict_free(&format_opts);
                        format_opts = nullptr;
                }

                if (codec_opts) {
                        av_dict_free(&codec_opts);
                        codec_opts = nullptr;
                }
        }

        void set_default_window_size(int width, int height, AVRational sar)
        {
            /*
                SDL_Rect rect;
                int max_width = screen_width ? screen_width : INT_MAX;
                int max_height = screen_height ? screen_height : INT_MAX;
                if (max_width == INT_MAX && max_height == INT_MAX)
                        max_height = height;
                calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
                default_width = rect.w;
                default_height = rect.h;
            */
        }

        int video_open(VideoState* is)
        {
                int w, h;

                w = screen_width ? screen_width : default_width;
                h = screen_height ? screen_height : default_height;

                SDL_SetWindowSize(window, w, h);
                SDL_SetWindowPosition(window, screen_left, screen_top);
                if (is_full_screen)
                        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                SDL_ShowWindow(window);

                is->width = w;
                is->height = h;

                return 0;
        }

        void send_video_image_to_upper(VideoState* is)
        {
            Frame* vp = frame_queue_peek_last(&is->pictq);
            if (vp) {
#if defined(DEBUG_SYNC)
                // pts : 此处已经是经timebase转换过的时间戳，单位s
                // src_frame: 此处已经是经过同步的视频帧 应该尽快推送渲染
                av_log_ffplay(NULL, AV_LOG_INFO, "---- pop video to upper, pts=%0.3f type:%c format:%d \n", vp->pts, av_get_picture_type_char(vp->frame->pict_type), vp->frame->format);
#endif
                auto cb = event_cb.lock();
                if (cb) {
                    std::shared_ptr<AVFrame> vf(av_frame_alloc(), [](AVFrame* ptr) {av_frame_free(&ptr); });
                    av_frame_move_ref(vf.get(), vp->frame);
                    cb->on_video_frame(vf, vp->pts);
                }
            }
        }

        /* display the current picture, if any */
        void video_display(VideoState* is)
        {
                if (display_disable) 
                {
                    send_video_image_to_upper(is);
                    return;
                }

                if (!is->width)
                        video_open(is);

                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                SDL_RenderClear(renderer);
                if (is->audio_st && is->show_mode != SHOW_MODE_VIDEO)
                        video_audio_display(is);
                else if (is->video_st)
                        video_image_display(is);
                SDL_RenderPresent(renderer);
        }

        double get_clock(Clock* c)
        {
                if (*c->queue_serial != c->serial)
                        return NAN;
                if (c->paused) {
                        return c->pts;
                }
                else {
                        double time = av_gettime_relative() / 1000000.0;
                        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
                }
        }

        void set_clock_at(Clock* c, double pts, int serial, double time)
        {
                c->pts = pts;
                c->last_updated = time;
                c->pts_drift = c->pts - time;
                c->serial = serial;
        }

        void set_clock(Clock* c, double pts, int serial)
        {
                double time = av_gettime_relative() / 1000000.0;
                set_clock_at(c, pts, serial, time);
        }

        void set_clock_speed(Clock* c, double speed)
        {
                set_clock(c, get_clock(c), c->serial);
                c->speed = speed;
        }

        void init_clock(Clock* c, int* queue_serial)
        {
                c->speed = 1.0;
                c->paused = 0;
                c->queue_serial = queue_serial;
                set_clock(c, NAN, -1);
        }

        void sync_clock_to_slave(Clock* c, Clock* slave)
        {
                double clock = get_clock(c);
                double slave_clock = get_clock(slave);
                if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
                        set_clock(c, slave_clock, slave->serial);
        }

        int get_master_sync_type(VideoState* is) {
                if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
                        if (is->video_st)
                                return AV_SYNC_VIDEO_MASTER;
                        else
                                return AV_SYNC_AUDIO_MASTER;
                }
                else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
                        if (is->audio_st)
                                return AV_SYNC_AUDIO_MASTER;
                        else
                                return AV_SYNC_EXTERNAL_CLOCK;
                }
                else {
                        return AV_SYNC_EXTERNAL_CLOCK;
                }
        }

        /* get the current master clock value */
        double get_master_clock(VideoState* is)
        {
                double val;

                switch (get_master_sync_type(is)) {
                case AV_SYNC_VIDEO_MASTER:
                        val = get_clock(&is->vidclk);
                        break;
                case AV_SYNC_AUDIO_MASTER:
                        val = get_clock(&is->audclk);
                        break;
                default:
                        val = get_clock(&is->extclk);
                        break;
                }
                return val;
        }

        void check_external_clock_speed(VideoState* is) {
                if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
                        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
                        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
                }
                else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES) &&
                        (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
                        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
                }
                else {
                        double speed = is->extclk.speed;
                        if (speed != 1.0)
                                set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
                }
        }

        /* seek in the stream */
        void stream_seek(VideoState* is, int64_t pos, int64_t rel, int by_bytes)
        {
                if (!is->seek_req) {
                        is->seek_pos = pos;
                        is->seek_rel = rel;
                        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
                        if (by_bytes)
                                is->seek_flags |= AVSEEK_FLAG_BYTE;
                        is->seek_req = 1;
                        SDL_CondSignal(is->continue_read_thread);
                }
        }

        /* pause or resume the video */
        void stream_toggle_pause(VideoState* is, bool cb_need = false)
        {
                if (is->paused) {
                        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
                        if (is->read_pause_return != AVERROR(ENOSYS)) {
                                is->vidclk.paused = 0;
                        }
                        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
                }
                set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
                is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;

                if (cb_need) {
                    auto cb = event_cb.lock();
                    if (cb) {
                        if (is->paused)
                        {
                            cb->on_player_paused();
                        }
                        else {
                            cb->on_player_resumed();
                        }
                    }
                }
        }

        void toggle_pause(VideoState* is)
        {
                stream_toggle_pause(is, true);
                is->step = 0;
        }

        void toggle_mute(VideoState* is)
        {
                is->muted = !is->muted;
        }

        void update_volume(VideoState* is, int sign, double step)
        {
                double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double)SDL_MIX_MAXVOLUME) / log(10)) : -1000.0;
                int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
                is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0, SDL_MIX_MAXVOLUME);
        }

        void step_to_next_frame(VideoState* is)
        {
                /* if the stream is paused unpause it, then step */
                if (is->paused)
                        stream_toggle_pause(is);
                is->step = 1;
        }

        double compute_target_delay(double delay, VideoState* is)
        {
                double sync_threshold, diff = 0;

                /* update delay to follow master synchronisation source */
                if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
                        /* if video is slave, we try to correct big delays by
                           duplicating or deleting a frame */
                        diff = get_clock(&is->vidclk) - get_master_clock(is);

                        /* skip or repeat frame. We take into account the
                           delay to compute the threshold. I still don't know
                           if it is the best guess */
                        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
                        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
                                if (diff <= -sync_threshold)
                                        delay = FFMAX(0, delay + diff);
                                else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                                        delay = delay + diff;
                                else if (diff >= sync_threshold)
                                        delay = 2 * delay;
                        }
                }

                av_log_ffplay(NULL, AV_LOG_DEBUG, "video: delay=%0.3f A-V=%f\n",
                        delay, -diff);

                return delay;
        }

        double vp_duration(VideoState* is, Frame* vp, Frame* nextvp) {
                if (vp->serial == nextvp->serial) {
                        double duration = nextvp->pts - vp->pts;
                        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
                                return vp->duration;
                        else
                                return duration;
                }
                else {
                        return 0.0;
                }
        }

        void update_video_pts(VideoState* is, double pts, int64_t pos, int serial) {
                /* update current video pts */
                set_clock(&is->vidclk, pts, serial);
                sync_clock_to_slave(&is->extclk, &is->vidclk);
        }

        /* called to display each frame */
        void video_refresh(void* opaque, double* remaining_time)
        {
                VideoState* is = (VideoState*)opaque;
                double time;

                Frame* sp, * sp2;

                if (!is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK && is->realtime)
                        check_external_clock_speed(is);

                if (/*!display_disable && */is->show_mode != SHOW_MODE_VIDEO && is->audio_st) {
                        time = av_gettime_relative() / 1000000.0;
                        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
                                video_display(is);
                                is->last_vis_time = time;
                        }
                        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
                }

                if (is->video_st) {
                retry:
                        if (frame_queue_nb_remaining(&is->pictq) == 0) {
                                // nothing to do, no picture to display in the queue
                        }
                        else {
                                double last_duration, duration, delay;
                                Frame* vp, * lastvp;

                                /* dequeue the picture */
                                lastvp = frame_queue_peek_last(&is->pictq);
                                vp = frame_queue_peek(&is->pictq);

                                if (vp->serial != is->videoq.serial) {
                                        frame_queue_next(&is->pictq);
                                        goto retry;
                                }

                                if (lastvp->serial != vp->serial)
                                        is->frame_timer = av_gettime_relative() / 1000000.0;

                                if (is->paused)
                                        goto display;

                                /* compute nominal last_duration */
                                last_duration = vp_duration(is, lastvp, vp);
                                delay = compute_target_delay(last_duration, is);

                                time = av_gettime_relative() / 1000000.0;
                                if (time < is->frame_timer + delay) {
                                        *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                                        goto display;
                                }

                                is->frame_timer += delay;
                                if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX)
                                        is->frame_timer = time;

                                SDL_LockMutex(is->pictq.mutex);
                                if (!isnan(vp->pts))
                                        update_video_pts(is, vp->pts, vp->pos, vp->serial);
                                SDL_UnlockMutex(is->pictq.mutex);

                                if (frame_queue_nb_remaining(&is->pictq) > 1) {
                                        Frame* nextvp = frame_queue_peek_next(&is->pictq);
                                        duration = vp_duration(is, vp, nextvp);
                                        if (!is->step && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) && time > is->frame_timer + duration) {
                                                is->frame_drops_late++;
                                                frame_queue_next(&is->pictq);
                                                goto retry;
                                        }
                                }

                                if (is->subtitle_st) {
                                        while (frame_queue_nb_remaining(&is->subpq) > 0) {
                                                sp = frame_queue_peek(&is->subpq);

                                                if (frame_queue_nb_remaining(&is->subpq) > 1)
                                                        sp2 = frame_queue_peek_next(&is->subpq);
                                                else
                                                        sp2 = NULL;

                                                if (sp->serial != is->subtitleq.serial
                                                        || (is->vidclk.pts > (sp->pts + ((float)sp->sub.end_display_time / 1000)))
                                                        || (sp2 && is->vidclk.pts > (sp2->pts + ((float)sp2->sub.start_display_time / 1000))))
                                                {
                                                        if (sp->uploaded) {
                                                                int i;
                                                                for (i = 0; i < sp->sub.num_rects; i++) {
                                                                        AVSubtitleRect* sub_rect = sp->sub.rects[i];
                                                                        uint8_t* pixels;
                                                                        int pitch, j;

                                                                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect*)sub_rect, (void**)&pixels, &pitch)) {
                                                                                for (j = 0; j < sub_rect->h; j++, pixels += pitch)
                                                                                        memset(pixels, 0, sub_rect->w << 2);
                                                                                SDL_UnlockTexture(is->sub_texture);
                                                                        }
                                                                }
                                                        }
                                                        frame_queue_next(&is->subpq);
                                                }
                                                else {
                                                        break;
                                                }
                                        }
                                }

                                frame_queue_next(&is->pictq);
                                is->force_refresh = 1;

                                if (is->step && !is->paused)
                                        stream_toggle_pause(is);
                        }
                display:
                        /* display picture */
                        if (/*!display_disable && */ is->force_refresh && is->show_mode == SHOW_MODE_VIDEO && is->pictq.rindex_shown)
                                video_display(is);
                }

                is->force_refresh = 0;
        }

        int queue_picture(VideoState* is, AVFrame* src_frame, double pts, double duration, int64_t pos, int serial)
        {
                Frame* vp;

                if (!(vp = frame_queue_peek_writable(&is->pictq)))
                        return -1;

                vp->sar = src_frame->sample_aspect_ratio;
                vp->uploaded = 0;

                vp->width = src_frame->width;
                vp->height = src_frame->height;
                vp->format = src_frame->format;

                vp->pts = pts;
                vp->duration = duration;
                vp->pos = pos;
                vp->serial = serial;

                set_default_window_size(vp->width, vp->height, vp->sar);

                av_frame_move_ref(vp->frame, src_frame);
                frame_queue_push(&is->pictq);
                return 0;
        }

        int get_video_frame(VideoState* is, AVFrame* frame)
        {
                int got_picture;

                if ((got_picture = decoder_decode_frame(&is->viddec, frame, NULL)) < 0)
                        return -1;

                if (got_picture) {
                        double dpts = NAN;

                        if (frame->pts != AV_NOPTS_VALUE)
                                dpts = av_q2d(is->video_st->time_base) * frame->pts;

                        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

                        if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
                                if (frame->pts != AV_NOPTS_VALUE) {
                                        double diff = dpts - get_master_clock(is);
                                        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                                                diff - is->frame_last_filter_delay < 0 &&
                                                is->viddec.pkt_serial == is->vidclk.serial &&
                                                is->videoq.nb_packets) {
                                                is->frame_drops_early++;
                                                av_frame_unref(frame);
                                                got_picture = 0;
                                        }
                                }
                        }
                }

                return got_picture;
        }

#if CONFIG_AVFILTER
        int configure_filtergraph(AVFilterGraph* graph, const char* filtergraph,
                AVFilterContext* source_ctx, AVFilterContext* sink_ctx)
        {
                int ret, i;
                int nb_filters = graph->nb_filters;
                AVFilterInOut* outputs = NULL, * inputs = NULL;

                if (filtergraph) {
                        outputs = avfilter_inout_alloc();
                        inputs = avfilter_inout_alloc();
                        if (!outputs || !inputs) {
                                ret = AVERROR(ENOMEM);
                                goto fail;
                        }

                        outputs->name = av_strdup("in");
                        outputs->filter_ctx = source_ctx;
                        outputs->pad_idx = 0;
                        outputs->next = NULL;

                        inputs->name = av_strdup("out");
                        inputs->filter_ctx = sink_ctx;
                        inputs->pad_idx = 0;
                        inputs->next = NULL;

                        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, NULL)) < 0)
                                goto fail;
                }
                else {
                        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
                                goto fail;
                }

                /* Reorder the filters to ensure that inputs of the custom filters are merged first */
                for (i = 0; i < graph->nb_filters - nb_filters; i++)
                        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

                ret = avfilter_graph_config(graph, NULL);
        fail:
                avfilter_inout_free(&outputs);
                avfilter_inout_free(&inputs);
                return ret;
        }

        int configure_video_filters(AVFilterGraph* graph, VideoState* is, const char* vfilters, AVFrame* frame)
        {
                enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
                char sws_flags_str[512] = "";
                char buffersrc_args[256];
                int ret;
                AVFilterContext* filt_src = NULL, * filt_out = NULL, * last_filter = NULL;
                AVCodecParameters* codecpar = is->video_st->codecpar;
                AVRational fr = av_guess_frame_rate(is->ic, is->video_st, NULL);
                const AVDictionaryEntry* e = NULL;
                int nb_pix_fmts = 0;
                int i, j;

                for (i = 0; i < renderer_info.num_texture_formats; i++) {
                        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
                                if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                                        pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                                        break;
                                }
                        }
                }
                pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

                while ((e = av_dict_iterate(sws_dict, e))) {
                        if (!strcmp(e->key, "sws_flags")) {
                                av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
                        }
                        else
                                av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
                }
                if (strlen(sws_flags_str))
                        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

                graph->scale_sws_opts = av_strdup(sws_flags_str);

                snprintf(buffersrc_args, sizeof(buffersrc_args),
                        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                        frame->width, frame->height, frame->format,
                        is->video_st->time_base.num, is->video_st->time_base.den,
                        codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
                if (fr.num && fr.den)
                        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

                if ((ret = avfilter_graph_create_filter(&filt_src,
                        avfilter_get_by_name("buffer"),
                        "ffplay_buffer", buffersrc_args, NULL,
                        graph)) < 0)
                        goto fail;

                ret = avfilter_graph_create_filter(&filt_out,
                        avfilter_get_by_name("buffersink"),
                        "ffplay_buffersink", NULL, NULL, graph);
                if (ret < 0)
                        goto fail;

                if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
                        goto fail;

                last_filter = filt_out;

                /* Note: this macro adds a filter before the lastly added filter, so the
                 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, NULL, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

                if (autorotate) {
                        double theta = 0.0;
                        int32_t* displaymatrix = NULL;
                        AVFrameSideData* sd = av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX);
                        if (sd)
                                displaymatrix = (int32_t*)sd->data;
                        if (!displaymatrix)
                                displaymatrix = (int32_t*)av_stream_get_side_data(is->video_st, AV_PKT_DATA_DISPLAYMATRIX, NULL);
                        theta = get_rotation(displaymatrix);

                        if (fabs(theta - 90) < 1.0) {
                                INSERT_FILT("transpose", "clock");
                        }
                        else if (fabs(theta - 180) < 1.0) {
                                INSERT_FILT("hflip", NULL);
                                INSERT_FILT("vflip", NULL);
                        }
                        else if (fabs(theta - 270) < 1.0) {
                                INSERT_FILT("transpose", "cclock");
                        }
                        else if (fabs(theta) > 1.0) {
                                char rotate_buf[64];
                                snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
                                INSERT_FILT("rotate", rotate_buf);
                        }
                }

                if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
                        goto fail;

                is->in_video_filter = filt_src;
                is->out_video_filter = filt_out;

        fail:
                return ret;
        }

        int configure_audio_filters(VideoState* is, const char* intput_afilters, int force_output_format)
        {
                const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
                int sample_rates[2] = { 0, -1 };
                AVFilterContext* filt_asrc = NULL, * filt_asink = NULL;
                char aresample_swr_opts[512] = "";
                const AVDictionaryEntry* e = NULL;
                AVBPrint bp;
                char asrc_args[256];
                int ret;

                avfilter_graph_free(&is->agraph);
                if (!(is->agraph = avfilter_graph_alloc()))
                        return AVERROR(ENOMEM);
                is->agraph->nb_threads = filter_nbthreads;

                av_bprint_init(&bp, 0, AV_BPRINT_SIZE_AUTOMATIC);

                while ((e = av_dict_iterate(swr_opts, e)))
                        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
                if (strlen(aresample_swr_opts))
                        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
                av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

                av_channel_layout_describe_bprint(&is->audio_filter_src.ch_layout, &bp);

                ret = snprintf(asrc_args, sizeof(asrc_args),
                        "sample_rate=%d:sample_fmt=%s:time_base=%d/%d:channel_layout=%s",
                        is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                        1, is->audio_filter_src.freq, bp.str);

                ret = avfilter_graph_create_filter(&filt_asrc,
                        avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                        asrc_args, NULL, is->agraph);
                if (ret < 0)
                        goto end;


                ret = avfilter_graph_create_filter(&filt_asink,
                        avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                        NULL, NULL, is->agraph);
                if (ret < 0)
                        goto end;

                if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
                        goto end;
                if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
                        goto end;

                if (force_output_format) {
                        sample_rates[0] = is->audio_tgt.freq;
                        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
                                goto end;
                        if ((ret = av_opt_set(filt_asink, "ch_layouts", bp.str, AV_OPT_SEARCH_CHILDREN)) < 0)
                                goto end;
                        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
                                goto end;
                }


                if ((ret = configure_filtergraph(is->agraph, intput_afilters, filt_asrc, filt_asink)) < 0)
                        goto end;

                is->in_audio_filter = filt_asrc;
                is->out_audio_filter = filt_asink;

        end:
                if (ret < 0)
                        avfilter_graph_free(&is->agraph);
                av_bprint_finalize(&bp, NULL);

                return ret;
        }
#endif  /* CONFIG_AVFILTER */

        int audio_thread(void* arg)
        {
                VideoState* is = (VideoState*)arg;
                AVFrame* frame = av_frame_alloc();
                Frame* af;
#if CONFIG_AVFILTER
                int last_serial = -1;
                int reconfigure;
#endif
                int got_frame = 0;
                AVRational tb;
                int ret = 0;

                if (!frame)
                        return AVERROR(ENOMEM);

                do {
                        if ((got_frame = decoder_decode_frame(&is->auddec, frame, NULL)) < 0)
                                goto the_end;

                        if (got_frame) {
                                tb = AVRational(1, frame->sample_rate);

#if CONFIG_AVFILTER
                                reconfigure =
                                        cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.ch_layout.nb_channels,
                                                (enum AVSampleFormat)frame->format, frame->ch_layout.nb_channels) ||
                                        av_channel_layout_compare(&is->audio_filter_src.ch_layout, &frame->ch_layout) ||
                                        is->audio_filter_src.freq != frame->sample_rate ||
                                        is->auddec.pkt_serial != last_serial;

                                if (reconfigure) {
                                        char buf1[1024], buf2[1024];
                                        av_channel_layout_describe(&is->audio_filter_src.ch_layout, buf1, sizeof(buf1));
                                        av_channel_layout_describe(&frame->ch_layout, buf2, sizeof(buf2));
                                        av_log_ffplay(NULL, AV_LOG_DEBUG,
                                                "Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                                                is->audio_filter_src.freq, is->audio_filter_src.ch_layout.nb_channels, av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                                                frame->sample_rate, frame->ch_layout.nb_channels, av_get_sample_fmt_name((enum AVSampleFormat)frame->format), buf2, is->auddec.pkt_serial);

                                        is->audio_filter_src.fmt = (enum AVSampleFormat)frame->format;
                                        ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &frame->ch_layout);
                                        if (ret < 0)
                                                goto the_end;
                                        is->audio_filter_src.freq = frame->sample_rate;
                                        last_serial = is->auddec.pkt_serial;

                                        if ((ret = configure_audio_filters(is, afilters.empty() ? NULL : afilters.c_str(), 1)) < 0)
                                                goto the_end;
                                }

                                if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                                        goto the_end;

                                while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                                        tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                                        if (!(af = frame_queue_peek_writable(&is->sampq)))
                                                goto the_end;

                                        af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                                        af->pos = frame->pkt_pos;
                                        af->serial = is->auddec.pkt_serial;
                                        af->duration = av_q2d(AVRational(frame->nb_samples, frame->sample_rate));


#if defined(DEBUG_SYNC)
                                        av_log_ffplay(NULL, AV_LOG_INFO, "----- push audio , % lf \n", af->pts);
#endif

                                        av_frame_move_ref(af->frame, frame);
                                        frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
                                        if (is->audioq.serial != is->auddec.pkt_serial)
                                                break;
                                }
                                if (ret == AVERROR_EOF)
                                        is->auddec.finished = is->auddec.pkt_serial;
#endif
                        }
                } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
        the_end:
#if CONFIG_AVFILTER
                avfilter_graph_free(&is->agraph);
#endif
                av_frame_free(&frame);
                return ret;
        }

        int decoder_start(Decoder* d, int (*fn)(void*), const char* thread_name, void* arg)
        {
                packet_queue_start(d->queue);
                d->decoder_tid = SDL_CreateThread(fn, thread_name, arg);
                if (!d->decoder_tid) {
                        av_log_ffplay(NULL, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
                        return AVERROR(ENOMEM);
                }
                return 0;
        }

        int video_thread(void* arg)
        {
                // for hw by walker-WSH
                AVFrame* sw_frame = av_frame_alloc();
                AVFrame* hw_frame = av_frame_alloc();
                RUN_WHEN_SECTION_END([=]() {
                    av_frame_free((AVFrame**)&sw_frame);
                    av_frame_free((AVFrame**)&hw_frame);
                    }
                );

                VideoState* is = (VideoState*)arg;
                //AVFrame* frame = av_frame_alloc();
                double pts;
                double duration;
                int ret;
                AVRational tb = is->video_st->time_base;
                AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, NULL);

#if CONFIG_AVFILTER
                AVFilterGraph* graph = NULL;
                AVFilterContext* filt_out = NULL, * filt_in = NULL;
                int last_w = 0;
                int last_h = 0;
                enum AVPixelFormat last_format = AV_PIX_FMT_NONE;
                int last_serial = -1;
                int last_vfilter_idx = 0;
#endif

                //if (!frame)
                //        return AVERROR(ENOMEM);

                for (;;) {
                        // for hw by walker-WSH
                        AVFrame* decode_frame = hw_decode_used ? hw_frame : sw_frame;
                        ret = get_video_frame(is, decode_frame);
                        if (ret < 0)
                                goto the_end;
                        if (!ret)
                                continue;

                        // for hw by walker-WSH ------------------------- start
                        AVFrame* frame = sw_frame;

                        RUN_WHEN_SECTION_END([=]() {
                            av_frame_unref(sw_frame);
                            av_frame_unref(hw_frame); });

                        if (hw_decode_used)
                        {
                            if (hw_frame->format == hw_format) {
                                auto err = av_hwframe_transfer_data(sw_frame, hw_frame, 0);
                                if (err != 0) {
                                    continue;
                                }

                                av_frame_copy_props(sw_frame, hw_frame);
                            }
                            else {
                                frame = hw_frame;
                            }
                        }
                        // for hw by walker-WSH ------------------------- end

#if CONFIG_AVFILTER
                        if (last_w != frame->width
                                || last_h != frame->height
                                || last_format != frame->format
                                || last_serial != is->viddec.pkt_serial
                                || last_vfilter_idx != is->vfilter_idx) {
                                av_log_ffplay(NULL, AV_LOG_DEBUG,
                                        "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                                        last_w, last_h,
                                        (const char*)av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                                        frame->width, frame->height,
                                        (const char*)av_x_if_null(av_get_pix_fmt_name((enum AVPixelFormat)frame->format), "none"), is->viddec.pkt_serial);
                                avfilter_graph_free(&graph);
                                graph = avfilter_graph_alloc();
                                if (!graph) {
                                        ret = AVERROR(ENOMEM);
                                        goto the_end;
                                }
                                graph->nb_threads = filter_nbthreads;
                                if ((ret = configure_video_filters(graph, is, !vfilters_list.empty() ? vfilters_list.c_str() : NULL, frame)) < 0) {
                                        abort_play = true;
                                        goto the_end;
                                }
                                filt_in = is->in_video_filter;
                                filt_out = is->out_video_filter;
                                last_w = frame->width;
                                last_h = frame->height;
                                last_format = (enum AVPixelFormat)frame->format;
                                last_serial = is->viddec.pkt_serial;
                                last_vfilter_idx = is->vfilter_idx;
                                frame_rate = av_buffersink_get_frame_rate(filt_out);
                        }

                        ret = av_buffersrc_add_frame(filt_in, frame);
                        if (ret < 0)
                                goto the_end;

                        while (ret >= 0) {
                                is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

                                ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
                                if (ret < 0) {
                                        if (ret == AVERROR_EOF)
                                                is->viddec.finished = is->viddec.pkt_serial;
                                        ret = 0;
                                        break;
                                }

                                is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
                                if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                                        is->frame_last_filter_delay = 0;
                                tb = av_buffersink_get_time_base(filt_out);
#endif
                                duration = (frame_rate.num && frame_rate.den ? av_q2d(AVRational(frame_rate.den, frame_rate.num)) : 0);
                                pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                                ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
                                av_frame_unref(frame);
#if CONFIG_AVFILTER
                                if (is->videoq.serial != is->viddec.pkt_serial)
                                        break;
                        }
#endif

                        if (ret < 0)
                                goto the_end;
                }
        the_end:
#if CONFIG_AVFILTER
                avfilter_graph_free(&graph);
#endif
                //av_frame_free(&frame);
                return 0;
        }

        int subtitle_thread(void* arg)
        {
                VideoState* is = (VideoState*)arg;
                Frame* sp;
                int got_subtitle;
                double pts;

                for (;;) {
                        if (!(sp = frame_queue_peek_writable(&is->subpq)))
                                return 0;

                        if ((got_subtitle = decoder_decode_frame(&is->subdec, NULL, &sp->sub)) < 0)
                                break;

                        pts = 0;

                        if (got_subtitle && sp->sub.format == 0) {
                                if (sp->sub.pts != AV_NOPTS_VALUE)
                                        pts = sp->sub.pts / (double)AV_TIME_BASE;
                                sp->pts = pts;
                                sp->serial = is->subdec.pkt_serial;
                                sp->width = is->subdec.avctx->width;
                                sp->height = is->subdec.avctx->height;
                                sp->uploaded = 0;

                                /* now we can update the picture count */
                                frame_queue_push(&is->subpq);
                        }
                        else if (got_subtitle) {
                                avsubtitle_free(&sp->sub);
                        }
                }
                return 0;
        }

        /* copy samples for viewing in editor window */
        void update_sample_display(VideoState* is, short* samples, int samples_size)
        {
                int size, len;

                size = samples_size / sizeof(short);
                while (size > 0) {
                        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
                        if (len > size)
                                len = size;
                        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
                        samples += len;
                        is->sample_array_index += len;
                        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
                                is->sample_array_index = 0;
                        size -= len;
                }
        }

        /* return the wanted number of samples to get better sync if sync_type is video
         * or external master clock */
        int synchronize_audio(VideoState* is, int nb_samples)
        {
                int wanted_nb_samples = nb_samples;

                /* if not master, then we try to remove or add samples to correct the clock */
                if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
                        double diff, avg_diff;
                        int min_nb_samples, max_nb_samples;

                        diff = get_clock(&is->audclk) - get_master_clock(is);

                        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
                                is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
                                if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                                        /* not enough measures to have a correct estimate */
                                        is->audio_diff_avg_count++;
                                }
                                else {
                                        /* estimate the A-V difference */
                                        avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                                        if (fabs(avg_diff) >= is->audio_diff_threshold) {
                                                wanted_nb_samples = nb_samples + (int)(diff * is->audio_src.freq);
                                                min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                                                max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                                                wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                                        }
                                        av_log_ffplay(NULL, AV_LOG_DEBUG, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                                                diff, avg_diff, wanted_nb_samples - nb_samples,
                                                is->audio_clock, is->audio_diff_threshold);
                                }
                        }
                        else {
                                /* too big difference : may be initial PTS errors, so
                                   reset A-V filter */
                                is->audio_diff_avg_count = 0;
                                is->audio_diff_cum = 0;
                        }
                }

                return wanted_nb_samples;
        }

        /**
         * Decode one audio frame and return its uncompressed size.
         *
         * The processed audio frame is decoded, converted if required, and
         * stored in is->audio_buf, with size in bytes given by the return
         * value.
         */
        int audio_decode_frame(VideoState* is)
        {
                int data_size, resampled_data_size;
                av_unused double audio_clock0;
                int wanted_nb_samples;
                Frame* af;

                if (is->paused)
                        return -1;

                do {
#if defined(_WIN32)
                        while (frame_queue_nb_remaining(&is->sampq) == 0) {
                                if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                                        return -1;
                                av_usleep(1000);
                        }
#endif
                        if (!(af = frame_queue_peek_readable(&is->sampq)))
                                return -1;
                        frame_queue_next(&is->sampq);
                } while (af->serial != is->audioq.serial);

                data_size = av_samples_get_buffer_size(NULL, af->frame->ch_layout.nb_channels,
                        af->frame->nb_samples,
                        (enum AVSampleFormat)af->frame->format, 1);

                wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

                if (af->frame->format != is->audio_src.fmt ||
                        av_channel_layout_compare(&af->frame->ch_layout, &is->audio_src.ch_layout) ||
                        af->frame->sample_rate != is->audio_src.freq ||
                        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
                        swr_free(&is->swr_ctx);
                        swr_alloc_set_opts2(&is->swr_ctx,
                                &is->audio_tgt.ch_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                &af->frame->ch_layout, (enum AVSampleFormat)af->frame->format, af->frame->sample_rate,
                                0, NULL);
                        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
                                av_log_ffplay(NULL, AV_LOG_ERROR,
                                        "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                                        af->frame->sample_rate, av_get_sample_fmt_name((enum AVSampleFormat)af->frame->format), af->frame->ch_layout.nb_channels,
                                        is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.ch_layout.nb_channels);
                                swr_free(&is->swr_ctx);
                                return -1;
                        }
                        if (av_channel_layout_copy(&is->audio_src.ch_layout, &af->frame->ch_layout) < 0)
                                return -1;
                        is->audio_src.freq = af->frame->sample_rate;
                        is->audio_src.fmt = (enum AVSampleFormat)af->frame->format;
                }

                if (is->swr_ctx) {
                        const uint8_t** in = (const uint8_t**)af->frame->extended_data;
                        uint8_t** out = &is->audio_buf1;
                        int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
                        int out_size = av_samples_get_buffer_size(NULL, is->audio_tgt.ch_layout.nb_channels, out_count, is->audio_tgt.fmt, 0);
                        int len2;
                        if (out_size < 0) {
                                av_log_ffplay(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
                                return -1;
                        }
                        if (wanted_nb_samples != af->frame->nb_samples) {
                                if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq / af->frame->sample_rate,
                                        wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                                        av_log_ffplay(NULL, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                                        return -1;
                                }
                        }
                        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
                        if (!is->audio_buf1)
                                return AVERROR(ENOMEM);
                        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
                        if (len2 < 0) {
                                av_log_ffplay(NULL, AV_LOG_ERROR, "swr_convert() failed\n");
                                return -1;
                        }
                        if (len2 == out_count) {
                                av_log_ffplay(NULL, AV_LOG_WARNING, "audio buffer is probably too small\n");
                                if (swr_init(is->swr_ctx) < 0)
                                        swr_free(&is->swr_ctx);
                        }
                        is->audio_buf = is->audio_buf1;
                        resampled_data_size = len2 * is->audio_tgt.ch_layout.nb_channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
                }
                else {
                        is->audio_buf = af->frame->data[0];
                        resampled_data_size = data_size;
                }

                audio_clock0 = is->audio_clock;
                /* update the audio clock with the pts */
                if (!isnan(af->pts))
                        is->audio_clock = af->pts + (double)af->frame->nb_samples / af->frame->sample_rate;
                else
                        is->audio_clock = NAN;

                // for send audio by walker-WSH
                if (!isnan(af->pts)) {
                    frame_pts_begin = pts_pos = af->pts;
                    frame_pts_end = is->audio_clock;
                }
                else {
                    frame_pts_begin = pts_pos = frame_pts_end = NAN;
                }

#if defined(DEBUG_SYNC)
                av_log_ffplay(NULL, AV_LOG_INFO, "----- pop audio , % lf \n", af->pts);
#endif

                is->audio_clock_serial = af->serial;
                return resampled_data_size;
        }

        // for send audio by walker-WSH
        double frame_pts_begin = NAN;
        double frame_pts_end = NAN;
        double pts_pos = NAN;

        // for send audio by walker-WSH
        void pop_audio_thread(VideoState* is)
        {
            auto start_systime = av_gettime_relative();
            int64_t next_system = 0;
            double start_data = NAN;

            double pre_pts = NAN;
            double expect_next_pts = NAN;
            const double ts_jumped = 0.1; // 100ms
            
            std::shared_ptr<uint8_t> buffer = nullptr;
            const int buffer_size = is->audio_hw_buf_size;

            const auto one_ms = 1000.0;

            while (!is->abort_request && !abort_play) {
                if (!buffer) {
                    buffer = std::shared_ptr<uint8_t>(new uint8_t[buffer_size]);
                }

                if (is->paused) {
                    pre_pts = expect_next_pts = NAN;
                }

                auto crt_systime = av_gettime_relative();
                if (crt_systime < next_system)
                {
                    auto sleep_us = next_system - crt_systime;
                    if (sleep_us > 100.0 * one_ms)
                        sleep_us = 100.0 * one_ms;

                    if (sleep_us >= 5 * one_ms)
                        av_usleep(sleep_us);
                }

                double crt_pts = NAN;
                int got_bytes = 0;
                auto data_got = pop_audio_callback(is, buffer.get(), buffer_size, crt_pts, got_bytes);
                if (!data_got) {
                    av_usleep(10 * one_ms);
                    continue;
                }

                auto cb = event_cb.lock();
                if (!cb)
                    break;

#if defined(DEBUG_SYNC)
                av_log_ffplay(NULL, AV_LOG_INFO, "----- pop audio to upper, % lf \n", crt_pts);
#endif

                int samples_per_chn = got_bytes / is->audio_tgt.frame_size;
                cb->on_audio_frame(buffer, is->audio_tgt, samples_per_chn, crt_pts);
                buffer.reset();

                if (isnan(pre_pts) || isnan(crt_pts) || crt_pts <= pre_pts || fabs(expect_next_pts - crt_pts) >= ts_jumped) {
                    av_log_ffplay(NULL, AV_LOG_INFO, "audio ts jumped %lf(ms), near-expected %lf(ms) \n", 
                        (crt_pts - pre_pts) * 1000.0, 
                        (expect_next_pts - crt_pts) * 1000.0);

                    start_data = crt_pts;
                    start_systime = av_gettime_relative();
                }

                auto one_seconds = 1000000.0;
                auto data_duration_s = double(samples_per_chn) / double(is->audio_tgt.freq);
                auto data_duration_us = data_duration_s * one_seconds;
                auto total_duration = (crt_pts - start_data) * one_seconds + data_duration_us;
                next_system = start_systime + total_duration;

                pre_pts = crt_pts;
                expect_next_pts = crt_pts + data_duration_s;
            }
        }

        // for send audio by walker-WSH
        bool pop_audio_callback(VideoState* is, Uint8* stream, int len, double &ret_pts, int &got_bytes)
        {
            got_bytes = 0;
            ret_pts = NAN;
            bool data_got = false;

            int audio_size, len1;
   
            audio_callback_time = av_gettime_relative();

            while (len > 0) {
                if (is->audio_buf_index >= is->audio_buf_size) {
                    audio_size = audio_decode_frame(is);
                    if (audio_size < 0) {
                        /* if error, just output silence */
                        is->audio_buf = NULL;
                        is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
                    }
                    else {
                        if (is->show_mode != SHOW_MODE_VIDEO)
                            update_sample_display(is, (int16_t*)is->audio_buf, audio_size);
                        is->audio_buf_size = audio_size;
                    }
                    is->audio_buf_index = 0;
                }

                len1 = is->audio_buf_size - is->audio_buf_index;
                if (len1 > len)
                    len1 = len;

                if (is->audio_buf) {
                    data_got = true;
                    got_bytes += len1;
                    memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
                }
                else {
                    memset(stream, 0, len1);
                }

                len -= len1;
                stream += len1;
                is->audio_buf_index += len1;

                if (isnan(ret_pts)) {
                    ret_pts = pts_pos;
                }
                if (!isnan(pts_pos))
                {
                    auto samples_per_chn = len1 / is->audio_tgt.frame_size;
                    pts_pos += double(samples_per_chn) / double(is->audio_tgt.freq);
                }
            }

            is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
            /* Let's assume the audio driver that is used by SDL has two periods. */
            if (!isnan(is->audio_clock)) {
                set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
                sync_clock_to_slave(&is->extclk, &is->audclk);
            }

            return data_got;
        }

        /* prepare a new audio buffer */
        void sdl_audio_callback(void* opaque, Uint8* stream, int len)
        {
                VideoState* is = (VideoState*)opaque;
                int audio_size, len1;

                audio_callback_time = av_gettime_relative();

                while (len > 0) {
                        if (is->audio_buf_index >= is->audio_buf_size) {
                                audio_size = audio_decode_frame(is);
                                if (audio_size < 0) {
                                        /* if error, just output silence */
                                        is->audio_buf = NULL;
                                        is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
                                }
                                else {
                                        if (is->show_mode != SHOW_MODE_VIDEO)
                                                update_sample_display(is, (int16_t*)is->audio_buf, audio_size);
                                        is->audio_buf_size = audio_size;
                                }
                                is->audio_buf_index = 0;
                        }
                        len1 = is->audio_buf_size - is->audio_buf_index;
                        if (len1 > len)
                                len1 = len;
                        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
                                memcpy(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, len1);
                        else {
                                memset(stream, 0, len1);
                                if (!is->muted && is->audio_buf)
                                        SDL_MixAudioFormat(stream, (uint8_t*)is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1, is->audio_volume);
                        }
                        len -= len1;
                        stream += len1;
                        is->audio_buf_index += len1;
                }
                is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
                /* Let's assume the audio driver that is used by SDL has two periods. */
                if (!isnan(is->audio_clock)) {
                        set_clock_at(&is->audclk, is->audio_clock - (double)(2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec, is->audio_clock_serial, audio_callback_time / 1000000.0);
                        sync_clock_to_slave(&is->extclk, &is->audclk);
                }
        }

        int audio_open(void* opaque, AVChannelLayout* wanted_channel_layout, int wanted_sample_rate, struct AudioParams* audio_hw_params)
        {
                SDL_AudioSpec wanted_spec, spec;
                const char* env;
                const int next_nb_channels[] = { 0, 0, 1, 6, 2, 6, 4, 6 };
                const int next_sample_rates[] = { 0, 44100, 48000, 96000, 192000 };
                int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;
                int wanted_nb_channels = wanted_channel_layout->nb_channels;

                env = SDL_getenv("SDL_AUDIO_CHANNELS");
                if (env) {
                        wanted_nb_channels = atoi(env);
                        av_channel_layout_uninit(wanted_channel_layout);
                        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
                }
                if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
                        av_channel_layout_uninit(wanted_channel_layout);
                        av_channel_layout_default(wanted_channel_layout, wanted_nb_channels);
                }
                wanted_nb_channels = wanted_channel_layout->nb_channels;
                wanted_spec.channels = wanted_nb_channels;
                wanted_spec.freq = wanted_sample_rate;
                if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
                        av_log_ffplay(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
                        return -1;
                }
                while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
                        next_sample_rate_idx--;
                wanted_spec.format = AUDIO_S16SYS;
                wanted_spec.silence = 0;
                wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
                wanted_spec.callback = s_sdl_audio_callback;
                wanted_spec.userdata = this;

                // for send audio by walker-WSH
                if (display_disable) {
                    spec = wanted_spec;

                    spec.channels = 2;
                    spec.size = wanted_spec.samples * spec.channels * 2; // 2 : sizeof AUDIO_S16SYS
                }
                else {
                    while (!(audio_dev = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
                        av_log_ffplay(NULL, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
                            wanted_spec.channels, wanted_spec.freq, SDL_GetError());
                        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
                        if (!wanted_spec.channels) {
                            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
                            wanted_spec.channels = wanted_nb_channels;
                            if (!wanted_spec.freq) {
                                av_log_ffplay(NULL, AV_LOG_ERROR,
                                    "No more combinations to try, audio open failed\n");
                                return -1;
                            }
                        }
                        av_channel_layout_default(wanted_channel_layout, wanted_spec.channels);
                    }
                }

                if (spec.format != AUDIO_S16SYS) {
                        av_log_ffplay(NULL, AV_LOG_ERROR,
                                "SDL advised audio format %d is not supported!\n", spec.format);
                        return -1;
                }
                if (spec.channels != wanted_spec.channels) {
                        av_channel_layout_uninit(wanted_channel_layout);
                        av_channel_layout_default(wanted_channel_layout, spec.channels);
                        if (wanted_channel_layout->order != AV_CHANNEL_ORDER_NATIVE) {
                                av_log_ffplay(NULL, AV_LOG_ERROR,
                                        "SDL advised channel count %d is not supported!\n", spec.channels);
                                return -1;
                        }
                }

                audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
                audio_hw_params->freq = spec.freq;
                if (av_channel_layout_copy(&audio_hw_params->ch_layout, wanted_channel_layout) < 0)
                        return -1;
                audio_hw_params->frame_size = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, 1, audio_hw_params->fmt, 1);
                audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(NULL, audio_hw_params->ch_layout.nb_channels, audio_hw_params->freq, audio_hw_params->fmt, 1);
                if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
                        av_log_ffplay(NULL, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
                        return -1;
                }
                return spec.size;
        }

        // for hw by walker-WSH
        bool has_hw_type(const AVCodec* c, enum AVHWDeviceType type, enum AVPixelFormat* output_hw_format)
        {
            for (int i = 0;; i++) {
                const AVCodecHWConfig* config = avcodec_get_hw_config(c, i);
                if (!config) {
                    break;
                }

                if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                    config->device_type == type) {
                    *output_hw_format = config->pix_fmt;
                    return true;
                }
            }

            return false;
        }

        // for hw by walker-WSH
        AVBufferRef* init_hw_decoder(Decoder* d, AVCodecContext* c, const AVCodec* codec)
        {
            enum AVHWDeviceType hw_priority[] = {
                AV_HWDEVICE_TYPE_D3D11VA,      AV_HWDEVICE_TYPE_DXVA2,
                AV_HWDEVICE_TYPE_CUDA,         AV_HWDEVICE_TYPE_VAAPI,
                AV_HWDEVICE_TYPE_VDPAU,        AV_HWDEVICE_TYPE_QSV,
                AV_HWDEVICE_TYPE_VIDEOTOOLBOX, AV_HWDEVICE_TYPE_NONE,
            };

            enum AVHWDeviceType* priority = hw_priority;
            enum AVPixelFormat temp_format = AVPixelFormat::AV_PIX_FMT_NONE;
            AVBufferRef* hw_ctx = NULL;
            
            while (*priority != AV_HWDEVICE_TYPE_NONE) {
                if (has_hw_type(codec, *priority, &temp_format)) {
                    int ret = av_hwdevice_ctx_create(&hw_ctx, *priority, NULL, NULL, 0);
                    if (ret == 0)
                        break;
                }

                priority++;
            }

            if (hw_ctx) {
                // this ref will be decreased by ffmpeg
                c->hw_device_ctx = av_buffer_ref(hw_ctx);
                hw_format = temp_format;
            }

            return hw_ctx;
        }

        /* open a given stream. Return 0 if OK */
        int stream_component_open(VideoState* is, int stream_index)
        {
                // for hw by walker-WSH
                AVBufferRef* temp_hw_device_ctx = nullptr;

                AVFormatContext* ic = is->ic;
                AVCodecContext* avctx;
                const AVCodec* codec;
                const char* forced_codec_name = NULL;
                AVDictionary* opts = NULL;
                const AVDictionaryEntry* t = NULL;
                int sample_rate;
                AVChannelLayout ch_layout = { 0 };
                int ret = 0;
                int stream_lowres = lowres;

                if (stream_index < 0 || stream_index >= ic->nb_streams)
                        return -1;

                avctx = avcodec_alloc_context3(NULL);
                if (!avctx)
                        return AVERROR(ENOMEM);

                ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
                if (ret < 0)
                        goto fail;
                avctx->pkt_timebase = ic->streams[stream_index]->time_base;

                codec = avcodec_find_decoder(avctx->codec_id);

                switch (avctx->codec_type) {
                case AVMEDIA_TYPE_AUDIO: is->last_audio_stream = stream_index; forced_codec_name = audio_codec_name; break;
                case AVMEDIA_TYPE_SUBTITLE: is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
                case AVMEDIA_TYPE_VIDEO: is->last_video_stream = stream_index; forced_codec_name = video_codec_name; break;
                }
                if (forced_codec_name)
                        codec = avcodec_find_decoder_by_name(forced_codec_name);
                if (!codec) {
                        if (forced_codec_name) av_log_ffplay(NULL, AV_LOG_WARNING,
                                "No codec could be found with name '%s'\n", forced_codec_name);
                        else                   av_log_ffplay(NULL, AV_LOG_WARNING,
                                "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
                        ret = AVERROR(EINVAL);
                        goto fail;
                }

                avctx->codec_id = codec->id;
                if (stream_lowres > codec->max_lowres) {
                        av_log_ffplay(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
                                codec->max_lowres);
                        stream_lowres = codec->max_lowres;
                }
                avctx->lowres = stream_lowres;

                if (fast)
                        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

                // for hw by walker-WSH
                if (request_hw_decode && avctx->codec_type == AVMediaType::AVMEDIA_TYPE_VIDEO) {
                    if ((avctx->width * avctx->height) >= min_hw_image_size) {
                        temp_hw_device_ctx = init_hw_decoder(&is->viddec, avctx, codec);
                    }
                    else {
                        av_log_ffplay(NULL, AV_LOG_INFO, "ignore hw decode since low resolution. %dx%d \n", avctx->width, avctx->height);
                    }
                }

                opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
                if (!av_dict_get(opts, "threads", NULL, 0))
                        av_dict_set(&opts, "threads", "auto", 0);
                if (stream_lowres)
                        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
                if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
                        goto fail;
                }
                if ((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
                        av_log_ffplay(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
                        ret = AVERROR_OPTION_NOT_FOUND;
                        goto fail;
                }

                is->eof = 0;
                ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
                switch (avctx->codec_type) {
                case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
                {
                        AVFilterContext* sink;

                        is->audio_filter_src.freq = avctx->sample_rate;
                        ret = av_channel_layout_copy(&is->audio_filter_src.ch_layout, &avctx->ch_layout);
                        if (ret < 0)
                                goto fail;
                        is->audio_filter_src.fmt = avctx->sample_fmt;
                        if ((ret = configure_audio_filters(is, afilters.empty() ? NULL : afilters.c_str(), 0)) < 0)
                                goto fail;
                        sink = is->out_audio_filter;
                        sample_rate = av_buffersink_get_sample_rate(sink);
                        ret = av_buffersink_get_ch_layout(sink, &ch_layout);
                        if (ret < 0)
                                goto fail;
                }
#else
                        sample_rate = avctx->sample_rate;
                        ret = av_channel_layout_copy(&ch_layout, &avctx->ch_layout);
                        if (ret < 0)
                                goto fail;
#endif

                        /* prepare audio output */
                        if ((ret = audio_open(is, &ch_layout, sample_rate, &is->audio_tgt)) < 0)
                                goto fail;
                        is->audio_hw_buf_size = ret;
                        is->audio_src = is->audio_tgt;
                        is->audio_buf_size = 0;
                        is->audio_buf_index = 0;

                        /* init averaging filter */
                        is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
                        is->audio_diff_avg_count = 0;
                        /* since we do not have a precise anough audio FIFO fullness,
                           we correct audio sync only if larger than this threshold */
                        is->audio_diff_threshold = (double)(is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

                        is->audio_stream = stream_index;
                        is->audio_st = ic->streams[stream_index];

                        if ((ret = decoder_init(&is->auddec, avctx, &is->audioq, is->continue_read_thread)) < 0)
                                goto fail;
                        if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) && !is->ic->iformat->read_seek) {
                                is->auddec.start_pts = is->audio_st->start_time;
                                is->auddec.start_pts_tb = is->audio_st->time_base;
                        }
                        if ((ret = decoder_start(&is->auddec, s_audio_thread, "audio_decoder", this)) < 0)
                                goto out;

                        // for send audio by walker-WSH
                        if (display_disable) 
                        {
                            pop_audio_handle = std::thread(&ffplayer::pop_audio_thread, this, is);
                        }
                        else {
                            SDL_PauseAudioDevice(audio_dev, 0);
                        }
                        
                        break;
                case AVMEDIA_TYPE_VIDEO:
                        is->video_stream = stream_index;
                        is->video_st = ic->streams[stream_index];

                        if ((ret = decoder_init(&is->viddec, avctx, &is->videoq, is->continue_read_thread)) < 0)
                                goto fail;
                        if ((ret = decoder_start(&is->viddec, s_video_thread, "video_decoder", this)) < 0)
                                goto out;
                        is->queue_attachments_req = 1;

                        // for hw by walker-WSH
                        hw_device_buf = temp_hw_device_ctx;
                        hw_decode_used = !!temp_hw_device_ctx;
                        temp_hw_device_ctx = nullptr;

                        av_log_ffplay(NULL, AV_LOG_INFO, "request hardware decode:%d, result:%d \n", request_hw_decode, hw_decode_used);
                        break;
                case AVMEDIA_TYPE_SUBTITLE:
                        is->subtitle_stream = stream_index;
                        is->subtitle_st = ic->streams[stream_index];

                        if ((ret = decoder_init(&is->subdec, avctx, &is->subtitleq, is->continue_read_thread)) < 0)
                                goto fail;
                        if ((ret = decoder_start(&is->subdec, s_subtitle_thread, "subtitle_decoder", this)) < 0)
                                goto out;
                        break;
                default:
                        break;
                }
                goto out;

            fail:
                avcodec_free_context(&avctx);

                // for send audio by walker-WSH
                if (audio_dev != 0) {
                    SDL_CloseAudioDevice(audio_dev);
                    audio_dev = 0;
                }

            out:
                // for hw by walker-WSH
                av_buffer_unref(&temp_hw_device_ctx);

                av_channel_layout_uninit(&ch_layout);
                av_dict_free(&opts);

                return ret;
        }

        int stream_has_enough_packets(AVStream* st, int stream_id, PacketQueue* queue) {
                return stream_id < 0 ||
                        queue->abort_request ||
                        (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
                        queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);
        }

        int is_realtime(AVFormatContext* s)
        {
                if (!strcmp(s->iformat->name, "rtp")
                        || !strcmp(s->iformat->name, "rtsp")
                        || !strcmp(s->iformat->name, "sdp")
                        )
                        return 1;

                if (s->pb && (!strncmp(s->url, "rtp:", 4)
                        || !strncmp(s->url, "udp:", 4)
                        )
                        )
                        return 1;
                return 0;
        }

        double get_duration_seconds(VideoState* is)
        {
            auto ctx_dur = is->ic->duration; // micro seconds
            if (ctx_dur != AV_NOPTS_VALUE)
                return double(ctx_dur) / AV_TIME_BASE;

            double audio_dur = 0.0;
            if (is->audio_st) {
                audio_dur = av_q2d(is->audio_st->time_base) * is->audio_st->duration;
            }

            double video_dur = 0.0;
            if (is->video_st) {
                video_dur = av_q2d(is->video_st->time_base) * is->video_st->duration;
            }

            return std::max(audio_dur, video_dur);
        }

        /* this thread gets the stream from the disk or the network */
        int read_thread(void* arg)
        {
                bool eof_happened = false;

                VideoState* is = (VideoState*)arg;
                AVFormatContext* ic = NULL;
                int err, i, ret;
                int st_index[AVMEDIA_TYPE_NB];
                AVPacket* pkt = NULL;
                int64_t stream_start_time;
                int pkt_in_play_range = 0;
                const AVDictionaryEntry* t;
                SDL_mutex* wait_mutex = SDL_CreateMutex();
                int scan_all_pmts_set = 0;
                int64_t pkt_ts;

                if (!wait_mutex) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateMutex(): %s\n", SDL_GetError());
                        ret = AVERROR(ENOMEM);
                        goto fail;
                }

                memset(st_index, -1, sizeof(st_index));
                is->eof = 0;

                pkt = av_packet_alloc();
                if (!pkt) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "Could not allocate packet.\n");
                        ret = AVERROR(ENOMEM);
                        goto fail;
                }
                ic = avformat_alloc_context();
                if (!ic) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
                        ret = AVERROR(ENOMEM);
                        goto fail;
                }
                ic->interrupt_callback.callback = s_decode_interrupt_cb;
                ic->interrupt_callback.opaque = this;
                if (!av_dict_get(format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE)) {
                        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
                        scan_all_pmts_set = 1;
                }
                err = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
                if (err < 0) {
                        av_log_ffplay(NULL, AV_LOG_ERROR, "avformat_open_input failed: %s\n", get_ffmpeg_error(err).c_str());
                        assert(false);
                        ret = -1;
                        goto fail;
                }
                if (scan_all_pmts_set)
                        av_dict_set(&format_opts, "scan_all_pmts", NULL, AV_DICT_MATCH_CASE);

                if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
                        av_log_ffplay(NULL, AV_LOG_ERROR, "Option %s not found.\n", t->key);
                        ret = AVERROR_OPTION_NOT_FOUND;
                        goto fail;
                }
                is->ic = ic;

                if (genpts)
                        ic->flags |= AVFMT_FLAG_GENPTS;

                av_format_inject_global_side_data(ic);

                if (find_stream_info) {
                        AVDictionary** opts = setup_find_stream_info_opts(ic, codec_opts);
                        int orig_nb_streams = ic->nb_streams;

                        err = avformat_find_stream_info(ic, opts);

                        for (i = 0; i < orig_nb_streams; i++)
                                av_dict_free(&opts[i]);
                        av_freep(&opts);

                        if (err < 0) {
                                av_log_ffplay(NULL, AV_LOG_WARNING,
                                        "%s: could not find codec parameters\n", is->filename);
                                ret = -1;
                                goto fail;
                        }
                }

                if (ic->pb)
                        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

                if (seek_by_bytes < 0)
                        seek_by_bytes = !(ic->iformat->flags & AVFMT_NO_BYTE_SEEK) &&
                        !!(ic->iformat->flags & AVFMT_TS_DISCONT) &&
                        strcmp("ogg", ic->iformat->name);

                is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;

                /* if seeking requested, we execute it */
                if (start_time != AV_NOPTS_VALUE) {
                        int64_t timestamp;

                        timestamp = start_time;
                        /* add the stream start time */
                        if (ic->start_time != AV_NOPTS_VALUE)
                                timestamp += ic->start_time;
                        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
                        if (ret < 0) {
                                av_log_ffplay(NULL, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                                        is->filename, (double)timestamp / AV_TIME_BASE);
                        }
                }

                is->realtime = is_realtime(ic);

                for (i = 0; i < ic->nb_streams; i++) {
                        AVStream* st = ic->streams[i];
                        enum AVMediaType type = st->codecpar->codec_type;
                        st->discard = AVDISCARD_ALL;
                        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
                                if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                                        st_index[type] = i;
                }
                for (i = 0; i < AVMEDIA_TYPE_NB; i++) {
                        if (wanted_stream_spec[i] && st_index[i] == -1) {
                                av_log_ffplay(NULL, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n", wanted_stream_spec[i], av_get_media_type_string((enum AVMediaType)i));
                                st_index[i] = INT_MAX;
                        }
                }

                if (!video_disable)
                        st_index[AVMEDIA_TYPE_VIDEO] =
                        av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                st_index[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
                if (!audio_disable)
                        st_index[AVMEDIA_TYPE_AUDIO] =
                        av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
                if (!video_disable && !subtitle_disable)
                        st_index[AVMEDIA_TYPE_SUBTITLE] =
                        av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ?
                                        st_index[AVMEDIA_TYPE_AUDIO] :
                                        st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);

                is->show_mode = show_mode;
                if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
                        AVStream* st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
                        AVCodecParameters* codecpar = st->codecpar;
                        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
                        if (codecpar->width)
                                set_default_window_size(codecpar->width, codecpar->height, sar);
                }

                /* open the streams */
                if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
                        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
                }

                ret = -1;
                if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
                        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
                }
                if (is->show_mode == SHOW_MODE_NONE)
                        is->show_mode = ret >= 0 ? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;

                if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {
                        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
                }

                if (is->video_stream < 0 && is->audio_stream < 0) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n",
                                is->filename);
                        ret = -1;
                        goto fail;
                }

                if (infinite_buffer < 0 && is->realtime)
                        infinite_buffer = 1;

                stream_ready = true;
                video_included = is->video_stream >= 0;
                audio_included = is->audio_stream >= 0;
                duration_seconds = get_duration_seconds(is);
                {
                    auto cb = event_cb.lock();
                    if (cb) {
                        ffplay_file_info info;
                        info.include_audio = audio_included.load();
                        info.include_video = video_included.load();
                        info.hw_decode_used = hw_decode_used;
                        info.duration_seconds = duration_seconds.load();

                        if (video_included) {
                            info.width = is->viddec.avctx->width;
                            info.height = is->viddec.avctx->height;
                        }

                        cb->on_stream_ready(info);
                    }
                }

                for (;;) {
                        if (is->abort_request)
                                break;
                        if (is->paused != is->last_paused) {
                                is->last_paused = is->paused;
                                if (is->paused)
                                        is->read_pause_return = av_read_pause(ic);
                                else
                                        av_read_play(ic);
                        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
                        if (is->paused &&
                                (!strcmp(ic->iformat->name, "rtsp") ||
                                        (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
                                /* wait 10 ms to avoid trying to get another packet */
                                /* XXX: horrible */
                                SDL_Delay(10);
                                continue;
                        }
#endif
                        if (is->seek_req) {
                                int64_t seek_target = is->seek_pos;
                                int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
                                int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
                                // FIXME the +-2 is due to rounding being not done in the correct direction in generation
                                //      of the seek_pos/seek_rel variables

                                ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
                                if (ret < 0) {
                                        assert(false);
                                        av_log_ffplay(NULL, AV_LOG_ERROR,
                                                "%s: error while seeking\n", is->ic->url);
                                }
                                else {
                                        eof_happened = false;

                                        if (is->audio_stream >= 0)
                                                packet_queue_flush(&is->audioq);
                                        if (is->subtitle_stream >= 0)
                                                packet_queue_flush(&is->subtitleq);
                                        if (is->video_stream >= 0)
                                                packet_queue_flush(&is->videoq);
                                        if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                                                set_clock(&is->extclk, NAN, 0);
                                        }
                                        else {
                                                set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                                        }
                                }
                                is->seek_req = 0;
                                is->queue_attachments_req = 1;
                                is->eof = 0;
                                if (is->paused)
                                        step_to_next_frame(is);
                        }
                        if (is->queue_attachments_req) {
                                if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                                        if ((ret = av_packet_ref(pkt, &is->video_st->attached_pic)) < 0)
                                                goto fail;
                                        packet_queue_put(&is->videoq, pkt);
                                        packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                                }
                                is->queue_attachments_req = 0;
                        }

                        /* if the queue are full, no need to read more */
                        if (infinite_buffer < 1 &&
                                (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE
                                        || (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                                                stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                                                stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
                                /* wait 10 ms */
                                SDL_LockMutex(wait_mutex);
                                SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
                                SDL_UnlockMutex(wait_mutex);
                                continue;
                        }
                        if (!is->paused &&
                                (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0)) &&
                                (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0))) {
                                if (loop != 1 && (!loop || --loop)) {
                                        auto cb = event_cb.lock();
                                        if (cb) {
                                            cb->on_player_restart();
                                        }
                                        stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
                                }
                                else if (autoexit) {
                                        auto cb = event_cb.lock();
                                        if (cb) {
                                            cb->on_player_auto_exit();
                                        }
                                        ret = AVERROR_EOF;
                                        goto fail;
                                }
                                else {
                                    if (!eof_happened) {
                                        eof_happened = true;
                                        auto cb = event_cb.lock();
                                        if (cb) {
                                            cb->on_stream_eof();
                                        }
                                    }
                                }
                        }
                        ret = av_read_frame(ic, pkt);
                        if (ret < 0) {
                                if ((ret == AVERROR_EOF || avio_feof(ic->pb)) && !is->eof) {
                                        if (is->video_stream >= 0)
                                                packet_queue_put_nullpacket(&is->videoq, pkt, is->video_stream);
                                        if (is->audio_stream >= 0)
                                                packet_queue_put_nullpacket(&is->audioq, pkt, is->audio_stream);
                                        if (is->subtitle_stream >= 0)
                                                packet_queue_put_nullpacket(&is->subtitleq, pkt, is->subtitle_stream);
                                        is->eof = 1;
                                }
                                if (ic->pb && ic->pb->error) {
                                    if (autoexit) {
                                        auto cb = event_cb.lock();
                                        if (cb) {
                                            cb->on_stream_error("av_read_frame failed");
                                            cb->on_player_auto_exit();
                                        }
                                        goto fail;
                                    }
                                    else
                                        break;
                                }
                                SDL_LockMutex(wait_mutex);
                                SDL_CondWaitTimeout(is->continue_read_thread, wait_mutex, 10);
                                SDL_UnlockMutex(wait_mutex);
                                continue;
                        }
                        else {
                                is->eof = 0;
                        }
                        /* check if packet is in play range specified by user, then queue, otherwise discard */
                        stream_start_time = ic->streams[pkt->stream_index]->start_time;
                        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
                        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
                                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                                av_q2d(ic->streams[pkt->stream_index]->time_base) -
                                (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                                <= ((double)duration / 1000000);
                        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
                                packet_queue_put(&is->audioq, pkt);
                        }
                        else if (pkt->stream_index == is->video_stream && pkt_in_play_range
                                && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                                packet_queue_put(&is->videoq, pkt);
                        }
                        else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
                                packet_queue_put(&is->subtitleq, pkt);
                        }
                        else {
                                av_packet_unref(pkt);
                        }
                }

                ret = 0;
        fail:
                if (ic && !is->ic)
                        avformat_close_input(&ic);

                av_packet_free(&pkt);
                if (ret != 0) {
                        abort_play = true;
                }
                SDL_DestroyMutex(wait_mutex);

                if (!stream_ready.load()) {
                    auto cb = event_cb.lock();
                    if (cb) {
                        cb->on_stream_error("failed to prepare media");
                    }
                }

                stream_ready = false;
                return 0;
        }

        VideoState* stream_open(const char* filename,
                const AVInputFormat* iformat)
        {
                VideoState* is = (VideoState*)av_mallocz(sizeof(VideoState));
                if (!is)
                        return NULL;

                video_state = is;

                is->last_video_stream = is->video_stream = -1;
                is->last_audio_stream = is->audio_stream = -1;
                is->last_subtitle_stream = is->subtitle_stream = -1;
                is->filename = av_strdup(filename);
                if (!is->filename)
                        goto fail;
                is->iformat = iformat;
                is->ytop = 0;
                is->xleft = 0;

                /* start video display */
                if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0)
                        goto fail;
                if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
                        goto fail;
                if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
                        goto fail;

                if (packet_queue_init(&is->videoq) < 0 ||
                        packet_queue_init(&is->audioq) < 0 ||
                        packet_queue_init(&is->subtitleq) < 0)
                        goto fail;

                if (!(is->continue_read_thread = SDL_CreateCond())) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
                        goto fail;
                }

                init_clock(&is->vidclk, &is->videoq.serial);
                init_clock(&is->audclk, &is->audioq.serial);
                init_clock(&is->extclk, &is->extclk.serial);
                is->audio_clock_serial = -1;
                if (startup_volume < 0)
                        av_log_ffplay(NULL, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
                if (startup_volume > 100)
                        av_log_ffplay(NULL, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
                startup_volume = av_clip(startup_volume, 0, 100);
                startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
                is->audio_volume = startup_volume;
                is->muted = 0;
                is->av_sync_type = av_sync_type;
                is->read_tid = SDL_CreateThread(s_read_thread, "read_thread", this);
                if (!is->read_tid) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
                fail:
                        stream_close(is);
                        video_state = NULL;
                        return NULL;
                }
                return is;
        }

        void stream_cycle_channel(VideoState* is, int codec_type)
        {
                AVFormatContext* ic = is->ic;
                int start_index, stream_index;
                int old_index;
                AVStream* st;
                AVProgram* p = NULL;
                int nb_streams = is->ic->nb_streams;

                if (codec_type == AVMEDIA_TYPE_VIDEO) {
                        start_index = is->last_video_stream;
                        old_index = is->video_stream;
                }
                else if (codec_type == AVMEDIA_TYPE_AUDIO) {
                        start_index = is->last_audio_stream;
                        old_index = is->audio_stream;
                }
                else {
                        start_index = is->last_subtitle_stream;
                        old_index = is->subtitle_stream;
                }
                stream_index = start_index;

                if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
                        p = av_find_program_from_stream(ic, NULL, is->video_stream);
                        if (p) {
                                nb_streams = p->nb_stream_indexes;
                                for (start_index = 0; start_index < nb_streams; start_index++)
                                        if (p->stream_index[start_index] == stream_index)
                                                break;
                                if (start_index == nb_streams)
                                        start_index = -1;
                                stream_index = start_index;
                        }
                }

                for (;;) {
                        if (++stream_index >= nb_streams)
                        {
                                if (codec_type == AVMEDIA_TYPE_SUBTITLE)
                                {
                                        stream_index = -1;
                                        is->last_subtitle_stream = -1;
                                        goto the_end;
                                }
                                if (start_index == -1)
                                        return;
                                stream_index = 0;
                        }
                        if (stream_index == start_index)
                                return;
                        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
                        if (st->codecpar->codec_type == codec_type) {
                                /* check that parameters are OK */
                                switch (codec_type) {
                                case AVMEDIA_TYPE_AUDIO:
                                        if (st->codecpar->sample_rate != 0 &&
                                                st->codecpar->ch_layout.nb_channels != 0)
                                                goto the_end;
                                        break;
                                case AVMEDIA_TYPE_VIDEO:
                                case AVMEDIA_TYPE_SUBTITLE:
                                        goto the_end;
                                default:
                                        break;
                                }
                        }
                }
        the_end:
                if (p && stream_index != -1)
                        stream_index = p->stream_index[stream_index];
                av_log_ffplay(NULL, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
                        av_get_media_type_string((enum AVMediaType)codec_type),
                        old_index,
                        stream_index);

                stream_component_close(is, old_index);
                stream_component_open(is, stream_index);
        }


        void toggle_full_screen(VideoState* is)
        {
                is_full_screen = !is_full_screen;
                SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        }

        void toggle_audio_display(VideoState* is)
        {
                int next = is->show_mode;
                do {
                        next = (next + 1) % SHOW_MODE_NB;
                } while (next != is->show_mode && (next == SHOW_MODE_VIDEO && !is->video_st || next != SHOW_MODE_VIDEO && !is->audio_st));
                if (is->show_mode != next) {
                        is->force_refresh = 1;
                        is->show_mode = (enum ShowMode)next;
                }
        }

        bool refresh_loop_wait_event(VideoState* is, SDL_Event* event) {
                double remaining_time = 0.0; // in seconds
                SDL_PumpEvents();

                while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
                        if (abort_play || !task_pool.TaskEmpty())
                                return false;

                        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
                                SDL_ShowCursor(0);
                                cursor_hidden = 1;
                        }

                        if (remaining_time > 0.0)
                                av_usleep((int64_t)(remaining_time * 1000000.0));

                        remaining_time = REFRESH_RATE;
                        if (is->show_mode != SHOW_MODE_NONE && (!is->paused || is->force_refresh))
                                video_refresh(is, &remaining_time);

                        SDL_PumpEvents();
                }

                return true;
        }

        void seek_chapter(VideoState* is, int incr)
        {
                AVRational temp(1, AV_TIME_BASE);

                int64_t pos = get_master_clock(is) * AV_TIME_BASE;
                int i;

                if (!is->ic->nb_chapters)
                        return;

                /* find the current chapter */
                for (i = 0; i < is->ic->nb_chapters; i++) {
                        AVChapter* ch = is->ic->chapters[i];
                        if (av_compare_ts(pos, temp, ch->start, ch->time_base) < 0) {
                                i--;
                                break;
                        }
                }

                i += incr;
                i = FFMAX(i, 0);
                if (i >= is->ic->nb_chapters)
                        return;

                av_log_ffplay(NULL, AV_LOG_DEBUG, "Seeking to chapter %d.\n", i);
                stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
                        temp), 0, 0);
        }

        void event_loop(VideoState* cur_stream)
        {
                if (!init_SDL_video()) {
                        assert(false);
                        do_exit();
                        return;
                }

                SDL_Event event;
                double incr, pos, frac;
                double x;

                while (!abort_play) {
                        task_pool.RunAllTask();

                        if (!refresh_loop_wait_event(cur_stream, &event))
                                continue;

                        switch (event.type) {
                        case SDL_KEYDOWN:
                                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q) {
                                        abort_play = true;
                                        break;
                                }

                                // If we don't yet have a window, skip all key events, because read_thread might still be initializing...
                                if (!cur_stream->width)
                                        continue;
                                switch (event.key.keysym.sym) {
                                case SDLK_f: // enter/leave fullscreen
                                        toggle_full_screen(cur_stream);
                                        cur_stream->force_refresh = 1;
                                        break;
                                case SDLK_p:
                                case SDLK_SPACE: // pause/resume
                                        toggle_pause(cur_stream);
                                        break;
                                case SDLK_m: // mute/unmute audio
                                        toggle_mute(cur_stream);
                                        break;
                                case SDLK_KP_MULTIPLY:
                                case SDLK_0: // increase volume 
                                        update_volume(cur_stream, 1, SDL_VOLUME_STEP);
                                        break;
                                case SDLK_KP_DIVIDE:
                                case SDLK_9:  // reduce volume
                                        update_volume(cur_stream, -1, SDL_VOLUME_STEP);
                                        break;
                                case SDLK_s: // S: Step to next frame
                                        step_to_next_frame(cur_stream);
                                        break;
                                case SDLK_a:
                                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                                        break;
                                case SDLK_v:
                                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                                        break;
                                case SDLK_c:
                                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_VIDEO);
                                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_AUDIO);
                                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                                        break;
                                case SDLK_t:
                                        stream_cycle_channel(cur_stream, AVMEDIA_TYPE_SUBTITLE);
                                        break;
                                case SDLK_w:
#if CONFIG_AVFILTER
                                        if (cur_stream->show_mode == SHOW_MODE_VIDEO && cur_stream->vfilter_idx < nb_vfilters - 1) {
                                                if (++cur_stream->vfilter_idx >= nb_vfilters)
                                                        cur_stream->vfilter_idx = 0;
                                        }
                                        else {
                                                cur_stream->vfilter_idx = 0;
                                                toggle_audio_display(cur_stream);
                                        }
#else
                                        toggle_audio_display(cur_stream);
#endif
                                        break;
                                case SDLK_PAGEUP:
                                        if (cur_stream->ic->nb_chapters <= 1) {
                                                incr = 600.0;
                                                goto do_seek;
                                        }
                                        seek_chapter(cur_stream, 1);
                                        break;
                                case SDLK_PAGEDOWN:
                                        if (cur_stream->ic->nb_chapters <= 1) {
                                                incr = -600.0;
                                                goto do_seek;
                                        }
                                        seek_chapter(cur_stream, -1);
                                        break;
                                case SDLK_LEFT:
                                        incr = seek_interval ? -seek_interval : -10.0;
                                        goto do_seek;
                                case SDLK_RIGHT:
                                        incr = seek_interval ? seek_interval : 10.0;
                                        goto do_seek;
                                case SDLK_UP:
                                        incr = 60.0;
                                        goto do_seek;
                                case SDLK_DOWN:
                                        incr = -60.0;
                                do_seek:
                                        if (seek_by_bytes) {
                                                pos = -1;
                                                if (pos < 0 && cur_stream->video_stream >= 0)
                                                        pos = frame_queue_last_pos(&cur_stream->pictq);
                                                if (pos < 0 && cur_stream->audio_stream >= 0)
                                                        pos = frame_queue_last_pos(&cur_stream->sampq);
                                                if (pos < 0)
                                                        pos = avio_tell(cur_stream->ic->pb);
                                                if (cur_stream->ic->bit_rate)
                                                        incr *= cur_stream->ic->bit_rate / 8.0;
                                                else
                                                        incr *= 180000.0;
                                                pos += incr;
                                                stream_seek(cur_stream, pos, incr, 1);
                                        }
                                        else {
                                                pos = get_master_clock(cur_stream);
                                                if (isnan(pos))
                                                        pos = (double)cur_stream->seek_pos / AV_TIME_BASE;
                                                pos += incr;
                                                if (cur_stream->ic->start_time != AV_NOPTS_VALUE && pos < cur_stream->ic->start_time / (double)AV_TIME_BASE)
                                                        pos = cur_stream->ic->start_time / (double)AV_TIME_BASE;
                                                stream_seek(cur_stream, (int64_t)(pos * AV_TIME_BASE), (int64_t)(incr * AV_TIME_BASE), 0);
                                        }
                                        break;
                                default:
                                        break;
                                }
                                break;
                        case SDL_MOUSEBUTTONDOWN:
                                if (event.button.button == SDL_BUTTON_LEFT) {
                                        if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                                                toggle_full_screen(cur_stream); // double click to enter/leave fullscreen
                                                cur_stream->force_refresh = 1;
                                                last_mouse_left_click = 0;
                                        }
                                        else {
                                                last_mouse_left_click = av_gettime_relative();
                                        }
                                }
                        case SDL_MOUSEMOTION:
                                if (cursor_hidden) {
                                        SDL_ShowCursor(1);
                                        cursor_hidden = 0;
                                }
                                cursor_last_shown = av_gettime_relative();
                                if (event.type == SDL_MOUSEBUTTONDOWN) {
                                        if (event.button.button != SDL_BUTTON_RIGHT)
                                                break;
                                        x = event.button.x;
                                }
                                else {
                                        if (!(event.motion.state & SDL_BUTTON_RMASK))
                                                break;
                                        x = event.motion.x;
                                }
                                if (seek_by_bytes || cur_stream->ic->duration <= 0) {
                                        uint64_t size = avio_size(cur_stream->ic->pb);
                                        stream_seek(cur_stream, size * x / cur_stream->width, 0, 1);
                                }
                                else {
                                        // seek file
                                        int64_t ts;
                                        int ns, hh, mm, ss;
                                        int tns, thh, tmm, tss;
                                        tns = cur_stream->ic->duration / 1000000LL;
                                        thh = tns / 3600;
                                        tmm = (tns % 3600) / 60;
                                        tss = (tns % 60);
                                        frac = x / cur_stream->width;
                                        ns = frac * tns;
                                        hh = ns / 3600;
                                        mm = (ns % 3600) / 60;
                                        ss = (ns % 60);
                                        av_log_ffplay(NULL, AV_LOG_INFO,
                                                "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                                                hh, mm, ss, thh, tmm, tss);
                                        ts = frac * cur_stream->ic->duration; // frac : percent position
                                        if (cur_stream->ic->start_time != AV_NOPTS_VALUE)
                                                ts += cur_stream->ic->start_time;
                                        stream_seek(cur_stream, ts, 0, 0);
                                }
                                break;
                        case SDL_WINDOWEVENT:
                                switch (event.window.event) {
                                case SDL_WINDOWEVENT_SIZE_CHANGED:
                                        screen_width = cur_stream->width = event.window.data1;
                                        screen_height = cur_stream->height = event.window.data2;
                                        if (cur_stream->vis_texture) {
                                                SDL_DestroyTexture(cur_stream->vis_texture);
                                                cur_stream->vis_texture = NULL;
                                        }
                                case SDL_WINDOWEVENT_EXPOSED:
                                        cur_stream->force_refresh = 1;
                                }
                                break;
                        case SDL_QUIT: // user clicked "X" button
                                abort_play = true;
                                break;
                        default:
                                break;
                        }
                }

                do_exit();

                uninit_SDL_video();

                task_pool.ClearAllTask();
        }

        char* copy_string(const  std::string& src) {
                assert(!src.empty());
                int len = src.length() + 1;
                auto dest = (char*)av_malloc(len);
                memmove(dest, src.c_str(), len);

                return dest;
        }

        bool apply_play_parameters(const ffplay_parameters& parameters)
        {
                if (parameters.file_name.empty()) {
                        return false;
                }

                input_filename = copy_string(parameters.file_name);

                if (!parameters.audio_filter.empty()) {
                        afilters = parameters.audio_filter;
                }

                if (!parameters.video_filter.empty()) {
                        nb_vfilters = 1;
                        vfilters_list = parameters.video_filter;
                }

                av_sync_type = AV_SYNC_AUDIO_MASTER;

                start_time = parameters.start_time;
                duration = parameters.duration;
                autoexit = parameters.auto_exit_when_eof ? 1 : 0;
                loop = parameters.loop;
                autorotate = parameters.auto_rotate ? 1 : 0;
                filter_nbthreads = parameters.filter_nbthreads;
                framedrop = parameters.framedrop;
                infinite_buffer = parameters.infinite_buffer;

                display_disable = parameters.disable_debug_render ? 1 : 0;
                audio_disable = parameters.disable_audio ? 1 : 0;
                video_disable = parameters.disable_video ? 1 : 0;
                subtitle_disable = parameters.disable_subtitle ? 1 : 0;

                // for hw by walker-WSH
                request_hw_decode = parameters.hw_decode;
                hw_decode_used = false;

                return true;
        }

        bool init_SDL_video() {
                SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
                SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

                if (!display_disable) {
                        int flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR
                        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
                        window = SDL_CreateWindow("Demo", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width, default_height, flags);
                        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
                        if (window) {
                                renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                                if (!renderer) {
                                        av_log_ffplay(NULL, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n", SDL_GetError());
                                        renderer = SDL_CreateRenderer(window, -1, 0);
                                }
                                if (renderer) {
                                        if (!SDL_GetRendererInfo(renderer, &renderer_info))
                                                av_log_ffplay(NULL, AV_LOG_DEBUG, "Initialized %s renderer.\n", renderer_info.name);
                                }
                        }
                        if (!window || !renderer || !renderer_info.num_texture_formats) {
                                av_log_ffplay(NULL, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
                                do_exit();
                                assert(false);
                                return false;
                        }
                }

                return true;
        }

        void uninit_SDL_video() {
                if (renderer) {
                        SDL_DestroyRenderer(renderer);
                        renderer = nullptr;
                }

                if (window) {
                        SDL_DestroyWindow(window);
                        window = nullptr;
                }
        }

        int run_player(const ffplay_parameters& parameters) override {
                if (!apply_play_parameters(parameters)) {
                        assert(false);
                        return FFPLAY_ERROR_INVALID_PARAM;
                }

                video_state = stream_open(input_filename, file_iformat);
                if (!video_state) {
                        av_log_ffplay(NULL, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
                        do_exit();
                        assert(false);
                        return FFPLAY_ERROR_OPEN_FAILED;
                }

                event_loop_tid = SDL_CreateThread(s_event_loop, "s_event_loop", this);
                return FFPLAY_ERROR_OK;
        }

        bool is_stream_ready() override {
            return stream_ready.load();
        }

        void request_seek_file(double percent) override {
            task_pool.PushAsyncTask([this, percent]()
                {
                    if (is_stream_ready() && !abort_play && video_state) {
                        auto temp = std::min(percent, 1.0);
                        temp = std::max(temp, 0.0);

                        auto ts = int64_t(temp * video_state->ic->duration);
                        if (video_state->ic->start_time != AV_NOPTS_VALUE)
                            ts += video_state->ic->start_time;

                        stream_seek(video_state, ts, 0, 0);
                    }
                    else {
                        assert(false);
                    }
                });
        }

        void request_toggle_pause_resume() override {
            task_pool.PushAsyncTask([this]()
                {
                    if (is_stream_ready() && !abort_play && video_state) {
                        toggle_pause(video_state);
                    }
                    else {
                        assert(false);
                    }
                });
        }

        void request_step_to_next_frame() override {
            task_pool.PushAsyncTask([this]()
                {
                    if (is_stream_ready() && !abort_play && video_state) {
                        step_to_next_frame(video_state);
                    }
                    else {
                        assert(false);
                    }
                });
        }

        void stop_player() override {
                abort_play = true;
                if (event_loop_tid) {
                        SDL_WaitThread(event_loop_tid, NULL);
                        event_loop_tid = nullptr;
                }
        }

 private:
     std::string get_ffmpeg_error(int err)
     {
         char errbuf[AV_ERROR_MAX_STRING_SIZE] = { 0 };
         av_strerror(err, errbuf, AV_ERROR_MAX_STRING_SIZE);
         return errbuf;
     }

     void av_log_ffplay(void* avcl, int level, const char* fmt, ...)
     {
         auto cb = event_cb.lock();
         if (!cb)
             return;

         char buf[4096];

         va_list args;
         va_start(args, fmt);
         vsnprintf_s(buf, 4096, fmt, args);
         va_end(args);

         cb->on_player_log(this, level, buf);
     }

}; // class ffplayer

int ffplayer::s_event_loop(void* arg) {
        auto self = (ffplayer*)arg;
        self->event_loop(self->video_state);
        return 0;
}

int ffplayer::s_read_thread(void* arg) {
        auto self = (ffplayer*)arg;
        self->read_thread(self->video_state);
        return 0;
}

int ffplayer::s_audio_thread(void* arg) {
        auto self = (ffplayer*)arg;
        self->audio_thread(self->video_state);
        return 0;
}

int ffplayer::s_video_thread(void* arg) {
        auto self = (ffplayer*)arg;
        self->video_thread(self->video_state);
        return 0;
}

int ffplayer::s_subtitle_thread(void* arg) {
        auto self = (ffplayer*)arg;
        self->subtitle_thread(self->video_state);
        return 0;
}

void ffplayer::s_sdl_audio_callback(void* arg, Uint8* stream, int len) {
        auto self = (ffplayer*)arg;
        self->sdl_audio_callback(self->video_state, stream, len);
}

int ffplayer::s_decode_interrupt_cb(void* ctx)
{
        auto self = (ffplayer*)ctx;
        return self->video_state->abort_request;
}

//-----------------------------------------------------------------------------------------------------------------------------
bool global_init() {
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();

    auto flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (SDL_Init(flags)) {
        return false;
    }

    return true;
}

void global_uninit() {
    avformat_network_deinit();

    SDL_Quit();
}

std::shared_ptr<ffplayer_interface> create_ffplayer(std::weak_ptr<ffplayer_event> cb)
{
    return std::make_shared<ffplayer>(cb);
}