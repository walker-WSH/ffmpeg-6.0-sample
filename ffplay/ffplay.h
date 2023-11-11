#pragma once
#include <string>
#include <atomic>
#include <memory>
#include <assert.h>

#if defined(__cplusplus)
extern "C" {
#endif
#include "libavutil/avutil.h"
#include "libavutil/frame.h"
#if defined(__cplusplus)
};
#endif

#ifdef _DEBUG
//#define DEBUG_SYNC
#endif

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

enum {
    FFPLAY_ERROR_OK = 0,
    FFPLAY_ERROR_INVALID_PARAM,
    FFPLAY_ERROR_OPEN_FAILED,
    FFPLAY_ERROR_UNKNOWN,
};

struct ffplay_parameters
{
    std::string file_name = "";

    // we can control the play speed by av filters
    std::string audio_filter = ""; // "atempo=tempo=2"
    std::string video_filter = ""; // "setpts=PTS*0.5"

    int64_t start_time = AV_NOPTS_VALUE; // in microseconds, seconds * 1000000
    int64_t duration = AV_NOPTS_VALUE; // seconds * 1000000

    bool disable_debug_render = false;
    bool disable_video = false;
    bool disable_audio = false;
    bool disable_subtitle = true; // disable subtitle

    int av_sync_type = AV_SYNC_AUDIO_MASTER;

    int loop = 1; // 0 means loop forever
    bool auto_exit_when_eof = false; // it will be ignore if loop is 0

    bool auto_rotate = true;
    int filter_nbthreads = 0;

    int framedrop = -1; // "drop frames when cpu is too slow"
    int infinite_buffer = -1; // don't limit the input buffer size (useful with realtime streams)
};

class ffplayer_event
{
public:
    virtual ~ffplayer_event() = default;

    // duration_seconds : in seconds
    virtual void on_stream_ready(bool include_audio, bool include_video, double duration_seconds) {}
    virtual void on_stream_error(const std::string& error) {}

    virtual void on_player_paused() {}
    virtual void on_player_resumed() {}

    // pts_seconds : in seconds
    virtual void on_video_frame(std::shared_ptr<AVFrame> frame, double pts_seconds) {}
    virtual void on_audio_frame(std::shared_ptr<AVFrame> frame, double pts_seconds) {}

    virtual void on_player_restart() {} // retart play since your loop settings
    virtual void on_stream_eof() {} // player stay on eof, and you can seek
    virtual void on_player_auto_exit() {} // player exit and you should no longer use it
};

class ffplayer_interface
{
public:
    virtual ~ffplayer_interface() = default;

    // return FFPLAY_ERROR_OK if there is no error
    virtual int run_player(const ffplay_parameters& parameters) = 0;

    virtual bool is_stream_ready() = 0;

    // percent : [0, 1.0]
    virtual void request_seek_file(double percent) = 0;

    virtual void request_toggle_pause_resume() = 0;

    virtual void request_step_to_next_frame() = 0;

    virtual void stop_player() = 0;
};

bool global_init();
void global_uninit();
std::shared_ptr<ffplayer_interface> create_ffplayer(std::weak_ptr<ffplayer_event> cb);

/*
TODO:
》不支持视频硬件解码
》audio: 设置支持的audio format
》audio: 应删除SDL audio/video的初始化，删除SDL Audio后，需要自己负责定时回调sdl_audio_callback取数据.注意：电脑没扬声器设备时 SDL_OpenAudioDevice会卡住
*/