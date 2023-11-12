#include "ffplay.h"
#include <chrono>


class my_ffplayer_event : public ffplayer_event
{
public:
    virtual ~my_ffplayer_event() = default;

    virtual void on_player_log(void* player, int level, const char* text)
    {
        switch (level)
        {
        case AV_LOG_PANIC:
        case AV_LOG_FATAL:
        case AV_LOG_ERROR:
        case AV_LOG_WARNING:
        case AV_LOG_INFO:
            printf(text);
            break;

        default:
#if defined(DEBUG_SYNC)
            printf(text);
#endif
            break;
        }
    }

    virtual void on_stream_ready(const ffplay_file_info& info)
    {
        printf("%s audio:%d video:%d duration:%lf hw:%d %dx%d \n", __FUNCTION__,
            info.include_audio, info.include_video, info.duration_seconds, info.hw_decode_used,
            info.width, info.height);
    }

    virtual void on_stream_error(const std::string& error)
    {
        printf("%s \n", __FUNCTION__);
    }

    virtual void on_player_paused()
    {
        printf("%s \n", __FUNCTION__);
    }

    virtual void on_player_resumed()
    {
        printf("%s \n", __FUNCTION__);
    }

    virtual void on_stream_eof()
    {
        printf("%s \n", __FUNCTION__);
    }

    virtual void on_player_restart()
    {
        printf("%s \n", __FUNCTION__);
    }

    virtual void on_player_auto_exit()
    {
        printf("%s \n", __FUNCTION__);
    }
};

int main(int argc, char** argv)
{
    printf("to init \n");
    std::chrono::steady_clock::time_point pos1 = std::chrono::steady_clock::now();

    if (!global_init()) {
        return 1;
    }

    std::chrono::steady_clock::time_point pos2 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::duration dur = pos2 - pos1;
    auto ms = duration_cast<std::chrono::milliseconds>(dur).count();
    printf("take %llu ms to init \n", ms);

    {
        ffplay_parameters parameters;
        parameters.file_name = "test.wmv";
        parameters.loop = 0;
        parameters.hw_decode = true;

        std::shared_ptr<my_ffplayer_event> cb = std::make_shared<my_ffplayer_event>();

        auto player = create_ffplayer(cb);
        player->run_player(parameters);

        auto ms = duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - pos2).count();
        printf("take %llu ms to run_player \n", ms);

        printf("click any key to stop ffplay... \n\n");
        auto ret = getchar();

        cb.reset();
        player->stop_player();
    }

    global_uninit();
    return 0;
}