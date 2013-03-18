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
#include "mcfourcc.h"
#include "auxinfo.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct VC1Context {
  AVClass        *class;
  vc1venc_tt * v_encoder;
  bufstream_tt * videobs;

} VC1Context;

// implementation structure
struct impl_stream
{
  uint8_t *user_bfr;
  uint32_t user_bfr_size;
  uint32_t user_idx;

  uint8_t *bfr;
  uint32_t idx;       // read-write index
  uint32_t bfr_size;  // allocated size
  uint32_t chunk_size;
};

static uint32_t fw_usable_bytes(bufstream_tt *bs)
{
  struct impl_stream* p = bs->Buf_IO_struct;
  return p->bfr_size - p->idx;
}


// request the buffer with at least numbytes-bytes
static uint8_t *fw_request(bufstream_tt *bs, uint32_t numbytes)
{
  struct impl_stream* p = bs->Buf_IO_struct;

  if(p->idx + numbytes <= p->bfr_size)
    return p->bfr + p->idx;

  if(p->idx)
  {
	if ((p->user_idx + p->idx > p->user_bfr_size) || (p->user_bfr == NULL))
      return NULL;

	memcpy(&p->user_bfr[p->user_idx], p->bfr, p->idx);
	p->user_idx += p->idx;
  }

  p->idx = 0;
  return p->bfr;
}


// confirm numbytes-bytes filled in in requested after last "request"-call
static uint32_t fw_confirm(bufstream_tt *bs, uint32_t numbytes)
{
  bs->Buf_IO_struct->idx += numbytes;
  return numbytes;
}


// put numbytes-bytes into bufsteam
static uint32_t fw_copybytes(bufstream_tt *bs, uint8_t *ptr, uint32_t numbytes)
{
  struct impl_stream* p = bs->Buf_IO_struct;
  uint8_t *pc;

  if ((pc = bs->request(bs, numbytes)) == NULL)
  {
    p->idx = 0;
    return 0;
  }
  memcpy(pc, ptr, numbytes);
  return bs->confirm(bs, numbytes);
}


// maximum chunk-size in buffer-mode (i.e. for "request"-call)
static uint32_t fw_chunksize(bufstream_tt *bs)
{
  return bs->Buf_IO_struct->chunk_size;
}
  

static uint32_t fw_auxinfo(bufstream_tt *bs, uint32_t offs, uint32_t info_ID, void *info_ptr, uint32_t info_size)
{
  uint32_t *ptr;
  struct impl_stream* p = bs->Buf_IO_struct;

  switch (info_ID)
  {
    case BYTECOUNT_INFO:
	  ptr = (uint32_t*)info_ptr;
	  if (ptr && (info_size == sizeof(uint32_t)))
	  {
        if(p->idx)
		{
          if ((p->user_idx + p->idx > p->user_bfr_size) || (p->user_bfr == NULL))
            return 0;

	      memcpy(&p->user_bfr[p->user_idx], p->bfr, p->idx);
	      p->user_idx += p->idx;
          p->idx = 0;
		}
		*ptr = p->user_idx;
	  }
	  break;

  }
  return BS_OK;
}


static uint32_t fw_split(bufstream_tt *bs)
{
  return 0;
}


static void fw_done(bufstream_tt *bs, int32_t Abort)
{
  struct impl_stream* p = bs->Buf_IO_struct;

  if (p->idx)
  {
    if ((p->user_idx + p->idx <= p->user_bfr_size) && (p->user_bfr != NULL))
      memcpy(&p->user_bfr[p->user_idx], p->bfr, p->idx);
  }

  free(p->bfr);
  free(p);
  bs->Buf_IO_struct = NULL;
}


static void fw_free(bufstream_tt *bs)
{
  if (bs->Buf_IO_struct)
    bs->done(bs, 0);

  free(bs);
}


static int32_t init_mem_buf_write(bufstream_tt *bs, uint8_t *buffer, uint32_t bufsize)
{
  bs->Buf_IO_struct = (struct impl_stream*)malloc(sizeof(struct impl_stream));
  if (!(bs->Buf_IO_struct))
  {
    return BS_ERROR;
  }

  bs->Buf_IO_struct->bfr = (uint8_t*)malloc(bufsize);
  if (!(bs->Buf_IO_struct->bfr))
  {
	free(bs->Buf_IO_struct);
    return BS_ERROR;
  }

  bs->Buf_IO_struct->user_bfr      = buffer;
  bs->Buf_IO_struct->user_bfr_size = bufsize;
  bs->Buf_IO_struct->user_idx      = 0;

  bs->Buf_IO_struct->bfr_size      = bufsize;
  bs->Buf_IO_struct->chunk_size    = bufsize;
  bs->Buf_IO_struct->idx           = 0;

  bs->usable_bytes = fw_usable_bytes;
  bs->request      = fw_request;
  bs->confirm      = fw_confirm;
  bs->copybytes    = fw_copybytes;
  bs->split        = fw_split;
  bs->chunksize    = fw_chunksize;
  bs->free         = fw_free;
  bs->auxinfo      = fw_auxinfo;
  bs->done         = fw_done;
  bs->drive_ptr    = NULL;
  bs->drive        = NULL;

  bs->state        = NULL;
  bs->flags        = BS_FLAGS_DST;
  return BS_OK;
}


static bufstream_tt *open_mem_buf_write(uint8_t *buffer, uint32_t bufsize)
{
  bufstream_tt *p;
  p = (bufstream_tt*)malloc(sizeof(bufstream_tt));
  if (p)
  {
    if (init_mem_buf_write(p, buffer, bufsize) != BS_OK)
    {
      free(p);
      p = NULL;
    }
  }
  return p;
}


static void close_mem_buf(bufstream_tt* bs, int32_t Abort)
{
  bs->done(bs, Abort);
  bs->free(bs);
}

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
static void * MC_EXPORT_API get_rc(const char* name)
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

  void * ext_info_stack[16] = {0};
  unsigned int option_flags = 0;
  void ** ext_info = &ext_info_stack[0];

  int plane1_size = frame->linesize[0] * ctx->height;
  int plane2_size = frame->linesize[1] * ctx->height / 2;
  int plane3_size = frame->linesize[2] * ctx->height / 2;
  int s = plane1_size + plane2_size + plane3_size;
  uint8_t *b = malloc(s);

  memcpy(b, frame->data[0], plane1_size);
  memcpy(b + plane1_size, frame->data[1], plane2_size);
  memcpy(b + plane1_size + plane2_size, frame->data[2], plane3_size);

  if (vc1OutVideoPutFrame(context->v_encoder, b, frame->linesize[0], ctx->width, ctx->height, FOURCC_I420, option_flags, ext_info) == VC1ERROR_FAILED)
    {
      printf("It failed\n");
      exit(1);
    }

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
  
  int init_options = 0;
  void * opt_list[10];

  // TODO - get from context
  double frame_rate = 25;
  int interlaced = 1;
  int bufsize = 1000000;
  uint8_t * buffer = malloc(bufsize);

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

  context->videobs = open_mem_buf_write(buffer, bufsize);
  
  if(vc1OutVideoInit(context->v_encoder, context->videobs, init_options, &opt_list[0]))
    {
      printf("vc1OutVideoInit fails.\n");
    }      

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
    .pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE}

};
