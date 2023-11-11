#include "ffplay.h"

class my_ffplayer_event : public ffplayer_event
{
public:
    virtual ~my_ffplayer_event() = default;

    virtual void on_stream_ready(bool include_audio, bool include_video, double duration)
    {
        printf("%s audio:%d video:%d duration:%lf \n", __FUNCTION__, include_audio, include_video, duration);
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
    if (!global_init()) {
        return 1;
    }

    {
        ffplay_parameters parameters;
        parameters.file_name = "test.wmv";

        std::shared_ptr<my_ffplayer_event> cb = std::make_shared<my_ffplayer_event>();

        auto player = create_ffplayer(cb);
        player->run_player(parameters);

        printf("click any key to stop ffplay... \n");
        auto ret = getchar();

        cb.reset();
        player->stop_player();
    }

    global_uninit();
    return 0;
}
