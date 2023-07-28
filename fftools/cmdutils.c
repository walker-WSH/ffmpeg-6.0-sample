/*
 * Various utilities for command line tools
 * Copyright (c) 2000-2003 Fabrice Bellard
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

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>

 /* Include only the enabled headers since some compilers (namely, Sun
    Studio) will not omit unused inline functions and create undefined
    references to libraries that are not being built. */

#include "config.h"
#include "compat/va_copy.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswscale/version.h"
#include "libswresample/swresample.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/display.h"
#include "libavutil/getenv_utf8.h"
#include "libavutil/mathematics.h"
#include "libavutil/imgutils.h"
#include "libavutil/libm.h"
#include "libavutil/parseutils.h"
#include "libavutil/eval.h"
#include "libavutil/dict.h"
#include "libavutil/opt.h"
#include "cmdutils.h"
#include "fopen_utf8.h"
#ifdef _WIN32
#include <windows.h>
#include "compat/w32dlfcn.h"
#endif

const OptionDef* find_option(const OptionDef* po, const char* name)
{
        while (po->name) {
                const char* end;
                if (av_strstart(name, po->name, &end) && (!*end || *end == ':'))
                        break;
                po++;
        }
        return po;
}

void print_error(const char* filename, int err)
{
        av_log(NULL, AV_LOG_ERROR, "%s: %s\n", filename, av_err2str(err));
}

int check_stream_specifier(AVFormatContext* s, AVStream* st, const char* spec)
{
        int ret = avformat_match_stream_specifier(s, st, spec);
        if (ret < 0)
                av_log(s, AV_LOG_ERROR, "Invalid stream specifier: %s.\n", spec);
        return ret;
}

AVDictionary* filter_codec_opts(AVDictionary* opts, enum AVCodecID codec_id,
        AVFormatContext* s, AVStream* st, const AVCodec* codec)
{
        AVDictionary* ret = NULL;
        const AVDictionaryEntry* t = NULL;
        int            flags = s->oformat ? AV_OPT_FLAG_ENCODING_PARAM
                : AV_OPT_FLAG_DECODING_PARAM;
        char          prefix = 0;
        const AVClass* cc = avcodec_get_class();

        if (!codec)
                codec = s->oformat ? avcodec_find_encoder(codec_id)
                : avcodec_find_decoder(codec_id);

        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
                prefix = 'v';
                flags |= AV_OPT_FLAG_VIDEO_PARAM;
                break;
        case AVMEDIA_TYPE_AUDIO:
                prefix = 'a';
                flags |= AV_OPT_FLAG_AUDIO_PARAM;
                break;
        case AVMEDIA_TYPE_SUBTITLE:
                prefix = 's';
                flags |= AV_OPT_FLAG_SUBTITLE_PARAM;
                break;
        }

        while (t = av_dict_iterate(opts, t)) {
                const AVClass* priv_class;
                char* p = strchr(t->key, ':');

                /* check stream specification in opt name */
                if (p)
                        switch (check_stream_specifier(s, st, p + 1)) {
                        case  1: *p = 0; break;
                        case  0:         continue;
                        default:        assert(0); break; // on error
                        }

                if (av_opt_find(&cc, t->key, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ||
                        !codec ||
                        ((priv_class = codec->priv_class) &&
                                av_opt_find(&priv_class, t->key, NULL, flags,
                                        AV_OPT_SEARCH_FAKE_OBJ)))
                        av_dict_set(&ret, t->key, t->value, 0);
                else if (t->key[0] == prefix &&
                        av_opt_find(&cc, t->key + 1, NULL, flags,
                                AV_OPT_SEARCH_FAKE_OBJ))
                        av_dict_set(&ret, t->key + 1, t->value, 0);

                if (p)
                        *p = ':';
        }
        return ret;
}

AVDictionary** setup_find_stream_info_opts(AVFormatContext* s,
        AVDictionary* codec_opts)
{
        int i;
        AVDictionary** opts;

        if (!s->nb_streams)
                return NULL;

        opts = av_calloc(s->nb_streams, sizeof(*opts));
        if (!opts)
                return NULL;

        for (i = 0; i < s->nb_streams; i++)
                opts[i] = filter_codec_opts(codec_opts, s->streams[i]->codecpar->codec_id,
                        s, s->streams[i], NULL);
        return opts;
}

double get_rotation(int32_t* displaymatrix)
{
        double theta = 0;
        if (displaymatrix)
                theta = -round(av_display_rotation_get((int32_t*)displaymatrix));

        theta -= 360 * floor(theta / 360 + 0.9 / 360);

        if (fabs(theta - 90 * round(theta / 90)) > 2)
                av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
                        "If you want to help, upload a sample "
                        "of this file to https://streams.videolan.org/upload/ "
                        "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");

        return theta;
}
