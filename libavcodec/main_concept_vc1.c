/*
 * H.264 encoding using the x264 library
 * Copyright (C) 2005  Mans Rullgard <mans@mansr.com>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "avcodec.h"
#include "internal.h"
#include "enc_vc1.h"
#include "config_vc1.h"
#include "enc_vc1_def.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VC1Context {
  AVClass        *class;
  vc1venc_tt * v_encoder;
  
} VC1Context;

static void info_printf(const char * fmt, ...)
{
    char lst[1024];
    va_list marker;

    va_start(marker, fmt);
    vsnprintf(lst, sizeof(lst), fmt, marker);
    va_end(marker);

    strncat(lst, "\r\n", sizeof(lst));
    printf("%s\n", lst);
}


static void warn_printf(const char * fmt, ...)
{
    char lst[256];
    va_list marker;

    va_start(marker, fmt);
    vsnprintf(lst, sizeof(lst), fmt, marker);
    va_end(marker);

    printf("%s\n", lst);
}


static void error_printf(const char * fmt, ...)
{
    char lst[256];
    va_list marker;

    va_start(marker,fmt);
    vsnprintf(lst, sizeof(lst), fmt, marker);
    va_end(marker);

    printf("%s\n", lst);
}


static void progress_printf(int32_t percent, const char * fmt, ...)
{
    char lst[256];
    va_list marker;

    va_start(marker,fmt);
    vsnprintf(lst, sizeof(lst), fmt, marker);
    va_end(marker);

    printf(" %d - %s\n", percent, lst);
}


static int32_t yield()
{
return 0;
}


// resource functions dispatcher
void * MC_EXPORT_API get_rc(const char* name)
{
    if (!strcmp(name, "err_printf"))
        return (void*) error_printf;
    else if (!strcmp(name, "prg_printf"))
        return (void*) progress_printf;
    else if (!strcmp(name, "wrn_printf"))
        return (void*) warn_printf;
    else if (!strcmp(name, "inf_printf"))
        return (void*) info_printf;
    else if (!strcmp(name, "yield"))
        return (void*) yield;

    return NULL;
}

static int get_video_type(int width, int height, double frame_rate)
{
    if ((width == 352) && ((height == 240) || (height == 288)))
    {
        return VC1_CIF;
    }
    else if ((width == 480) && ((height == 480) || (height == 576)))
    {
        return VC1_SVCD;
    }
    else if ((width == 720) && ((height == 480) || (height == 576)))
    {
        return VC1_D1;
    }
    else if (width < 288)
    {
        return VC1_BASELINE;
    }
    else if (width >= 1280)
    {
        return VC1_BD;
    }

    return VC1_MAIN;
}

static int VC1_frame(AVCodecContext *ctx, AVPacket *pkt, const AVFrame *frame,
		       int *got_packet)
{
  VC1Context *context = ctx->priv_data;
  int ret;
  
  /*
  if (vc1OutVideoPutFrame(v_encoder, input_video_buffer + img_start, line_size, width, height, fourcc, option_flags, ext_info))
    {
      break;
    }
  */  
  *got_packet = ret;
  return 0;
}

static av_cold int VC1_close(AVCodecContext *avctx)
{
  VC1Context *context = avctx->priv_data;
  
  return 0;
}

static av_cold int VC1_init(AVCodecContext *avctx)
{
  VC1Context *context = avctx->priv_data;
  
  struct vc1_param_set param_set;
  struct vc1_v_settings * v_settings = &param_set.params;

  // TODO - get from context
  double frame_rate = 25;
  int interlaced = 1;

  int video_type = get_video_type(avctx->width, avctx->height, frame_rate);

  vc1OutVideoDefaults(v_settings, video_type, 0);

  v_settings->min_key_frame_interval   = 1;
  
  v_settings->bit_rate                 = avctx->bit_rate >= 0 ? avctx->bit_rate : v_settings->bit_rate;
  v_settings->frame_rate               = frame_rate > 0.0 ? frame_rate : v_settings->frame_rate;
  v_settings->interlace_mode           = interlaced == 0 ? VC1_PROGRESSIVE : interlaced == 1 ? VC1_INTERLACE_MBAFF : v_settings->interlace_mode;
  
  // the encoder can't scale the picture
  v_settings->def_horizontal_size      = avctx->width;
  v_settings->def_vertical_size        = avctx->height;
  
  context->v_encoder = vc1OutVideoNew(get_rc, v_settings, 0, 0xFFFFFFFF, 0, 0);


  return 0;
}

static av_cold void VC1_init_static(AVCodec *codec)
{
}

static const AVOption options[] = {
    { NULL },
};

static const AVClass class = {
    .class_name = "mc_vc1",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault VC1_defaults[] = {
    { NULL },
};

AVCodec ff_mc_vc1_encoder = {
    .name             = "mc_vc1",
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_MAIN_CONCEPT_VC1,
    .priv_data_size   = sizeof(VC1Context),
    .init             = VC1_init,
    .encode2          = VC1_frame,
    .close            = VC1_close,
    .capabilities     = CODEC_CAP_DELAY | CODEC_CAP_AUTO_THREADS,
    .long_name        = NULL_IF_CONFIG_SMALL("Main Concept VC1"),
    .priv_class       = &class,
    .defaults         = VC1_defaults,
    .init_static_data = VC1_init_static,
};
