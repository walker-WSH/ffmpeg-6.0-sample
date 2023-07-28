/*
 * Various utilities for command line tools
 * copyright (c) 2003 Fabrice Bellard
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

#ifndef FFTOOLS_CMDUTILS_H
#define FFTOOLS_CMDUTILS_H

#include <stdint.h>
#include <assert.h>

#include "config.h"
#include "libavcodec/avcodec.h"
#include "libavfilter/avfilter.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"

#ifdef _WIN32
#undef main /* We don't want SDL to override our main() */
#endif

typedef struct SpecifierOpt {
        char* specifier;    /**< stream/chapter/program/... specifier */
        union {
                uint8_t* str;
                int        i;
                int64_t  i64;
                uint64_t ui64;
                float      f;
                double   dbl;
        } u;
} SpecifierOpt;

typedef struct OptionDef {
        const char* name;
        int flags;
#define HAS_ARG    0x0001
#define OPT_BOOL   0x0002
#define OPT_EXPERT 0x0004
#define OPT_STRING 0x0008
#define OPT_VIDEO  0x0010
#define OPT_AUDIO  0x0020
#define OPT_INT    0x0080
#define OPT_FLOAT  0x0100
#define OPT_SUBTITLE 0x0200
#define OPT_INT64  0x0400
#define OPT_EXIT   0x0800
#define OPT_DATA   0x1000
#define OPT_PERFILE  0x2000     /* the option is per-file (currently ffmpeg-only).
                                   implied by OPT_OFFSET or OPT_SPEC */
#define OPT_OFFSET 0x4000       /* option is specified as an offset in a passed optctx */
#define OPT_SPEC   0x8000       /* option is to be stored in an array of SpecifierOpt.
                                   Implies OPT_OFFSET. Next element after the offset is
                                   an int containing element count in the array. */
#define OPT_TIME  0x10000
#define OPT_DOUBLE 0x20000
#define OPT_INPUT  0x40000
#define OPT_OUTPUT 0x80000
        union {
                void* dst_ptr;
                int (*func_arg)(void*, const char*, const char*);
                size_t off;
        } u;
        const char* help;
        const char* argname;
} OptionDef;

/**
 * An option extracted from the commandline.
 * Cannot use AVDictionary because of options like -map which can be
 * used multiple times.
 */
typedef struct Option {
        const OptionDef* opt;
        const char* key;
        const char* val;
} Option;

typedef struct OptionGroupDef {
        /**< group name */
        const char* name;
        /**
         * Option to be used as group separator. Can be NULL for groups which
         * are terminated by a non-option argument (e.g. ffmpeg output files)
         */
        const char* sep;
        /**
         * Option flags that must be set on each option that is
         * applied to this group
         */
        int flags;
} OptionGroupDef;

typedef struct OptionGroup {
        const OptionGroupDef* group_def;
        const char* arg;

        Option* opts;
        int  nb_opts;

        AVDictionary* codec_opts;
        AVDictionary* format_opts;
        AVDictionary* sws_dict;
        AVDictionary* swr_opts;
} OptionGroup;

/**
 * A list of option groups that all have the same group type
 * (e.g. input files or output files)
 */
typedef struct OptionGroupList {
        const OptionGroupDef* group_def;

        OptionGroup* groups;
        int       nb_groups;
} OptionGroupList;

typedef struct OptionParseContext {
        OptionGroup global_opts;

        OptionGroupList* groups;
        int           nb_groups;

        /* parsing state */
        OptionGroup cur_group;
} OptionParseContext;

/**
 * Check if the given stream matches a stream specifier.
 *
 * @param s  Corresponding format context.
 * @param st Stream from s to be checked.
 * @param spec A stream specifier of the [v|a|s|d]:[\<stream index\>] form.
 *
 * @return 1 if the stream matches, 0 if it doesn't, <0 on error
 */
int check_stream_specifier(AVFormatContext* s, AVStream* st, const char* spec);

/**
 * Filter out options for given codec.
 *
 * Create a new options dictionary containing only the options from
 * opts which apply to the codec with ID codec_id.
 *
 * @param opts     dictionary to place options in
 * @param codec_id ID of the codec that should be filtered for
 * @param s Corresponding format context.
 * @param st A stream from s for which the options should be filtered.
 * @param codec The particular codec for which the options should be filtered.
 *              If null, the default one is looked up according to the codec id.
 * @return a pointer to the created dictionary
 */
AVDictionary* filter_codec_opts(AVDictionary* opts, enum AVCodecID codec_id,
        AVFormatContext* s, AVStream* st, const AVCodec* codec);

/**
 * Setup AVCodecContext options for avformat_find_stream_info().
 *
 * Create an array of dictionaries, one dictionary for each stream
 * contained in s.
 * Each dictionary will contain the options from codec_opts which can
 * be applied to the corresponding stream codec context.
 *
 * @return pointer to the created array of dictionaries.
 */
AVDictionary** setup_find_stream_info_opts(AVFormatContext* s,
        AVDictionary* codec_opts);

/**
 * Print an error message to stderr, indicating filename and a human
 * readable description of the error code err.
 *
 * If strerror_r() is not available the use of this function in a
 * multithreaded application may be unsafe.
 *
 * @see av_strerror()
 */
void print_error(const char* filename, int err);

double get_rotation(int32_t* displaymatrix);

#endif /* FFTOOLS_CMDUTILS_H */
