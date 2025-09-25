/*
 * Copyright (c) 2025 Plex, Inc.
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

#ifndef FFTOOLS_PLEX_H
#define FFTOOLS_PLEX_H

#include "cmdutils.h"
#include "ffmpeg.h"
#include "libavfilter/avfilter.h"

typedef struct
{
    int file_index;
    int stream_index;
    AVFilterContext *ctx;
    int width, height;
} InlineAssContext;

typedef struct
{
    char* progress_url;
    int throttle_delay;
    
    int nb_inlineass_ctxs;
    InlineAssContext *inlineass_ctxs;
} PlexContext;

extern PlexContext plexContext;

typedef enum LogLevel
{
   LOG_LEVEL_ERROR,
   LOG_LEVEL_WARNING,
   LOG_LEVEL_INFO,
   LOG_LEVEL_DEBUG,
   LOG_LEVEL_VERBOSE,
} LogLevel;

char* PMS_IssueHttpRequest(const char* url, const char* verb);
void PMS_Log(LogLevel level, const char* format, ...);

void plex_init(int argc, char **argv, const OptionDef *options);
int av_log_get_level_plex(void);
void av_log_set_level_plex(int);

void plex_report_stream(const AVStream *st);
void plex_report_stream_detail(AVStream *st);

int plex_opt_progress_url(void *optctx, const char *opt, const char *arg);
int plex_opt_loglevel(void *o, const char *opt, const char *arg);

void plex_feedback(const AVFormatContext *ic);

int plex_opt_subtitle_stream(void *optctx, const char *opt, const char *arg);

void plex_prepare_setup_streams_for_input_stream(InputStream* ist);
void plex_link_subtitles_to_graph(AVFilterGraph* graph);
int plex_process_subtitles(const InputStream *ist, AVSubtitle *sub);
void plex_link_input_stream(const InputStream *ist);

#endif