/*
 * VC1 encoding using the Main Concept library
 * Copyright (C) 2013  id3as-company Ltd
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
#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "internal.h"

#include "enc_vc1.h"
#include "config_vc1.h"
#include "enc_vc1_def.h"
#include "mcfourcc.h"
#include "mcdefs.h"
#include "auxinfo.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VC1Context {
  AVClass        *class;
  struct vc1_param_set param_set;
  struct vc1_v_settings *v_settings;
  vc1venc_tt * v_encoder;
  bufstream_tt * videobs;
  char *video_format;
  int asf_binding_byte;
  int done;

} VC1Context;

#define BUFFER_SIZE 1000000
#define NUM_FRAMES 10
#define ONE_HUNDRED_NANOS (AVRational){1, 10000000}
#define TWENTY_SEVEN_MHZ (AVRational){1, 27000000}


struct encoder_frame {
  uint8_t bfr[BUFFER_SIZE];
  uint32_t bfr_size;
  uint32_t data_size;
  uint16_t flags;
  uint16_t type;
  int64_t original_pts;
  int64_t pts;
  int64_t dts;
  int populated;
};

struct frame_buffer
{
  struct encoder_frame frames[10];
  uint32_t read_idx;
  uint32_t write_idx;
  uint32_t chunk_size;
  AVRational time_base;

  int64_t pts_diff;
};

static struct encoder_frame *read_frame(bufstream_tt *bs)
{
  struct frame_buffer* p = (struct frame_buffer*) bs->Buf_IO_struct;
  uint32_t idx = p->read_idx;

  if (p->frames[idx].populated) {

    p->read_idx = (p->read_idx + 1) % NUM_FRAMES;
    p->frames[idx].populated = 0;
    return &(p->frames[idx]);
  }
  else {
    return NULL;
  }
}

static uint32_t fw_usable_bytes(bufstream_tt *bs)
{
  //struct frame_buffer* p = bs->Buf_IO_struct;

  return BUFFER_SIZE;
}


// request the buffer with at least numbytes-bytes
static uint8_t *fw_request(bufstream_tt *bs, uint32_t numbytes)
{
  struct frame_buffer* p = (struct frame_buffer*) bs->Buf_IO_struct;

  p->write_idx = (p->write_idx + 1) % NUM_FRAMES;
  p->frames[p->write_idx].populated = 1;

  return p->frames[p->write_idx].bfr;
}


// confirm numbytes-bytes filled in in requested after last "request"-call
static uint32_t fw_confirm(bufstream_tt *bs, uint32_t numbytes)
{
  struct frame_buffer* p = (struct frame_buffer*)bs->Buf_IO_struct;

  p->frames[p->write_idx].data_size = numbytes;

  return numbytes;
}


// put numbytes-bytes into bufsteam
static uint32_t fw_copybytes(bufstream_tt *bs, uint8_t *ptr, uint32_t numbytes)
{
  uint8_t *pc;

  if ((pc = bs->request(bs, numbytes)) == NULL)
    {
      return 0;
    }
  memcpy(pc, ptr, numbytes);
  return bs->confirm(bs, numbytes);
}


// maximum chunk-size in buffer-mode (i.e. for "request"-call)
static uint32_t fw_chunksize(bufstream_tt *bs)
{
  struct frame_buffer* p = (struct frame_buffer*) bs->Buf_IO_struct;
  return p->chunk_size;
}


static uint32_t fw_auxinfo(bufstream_tt *bs, uint32_t offs, uint32_t info_ID, void *info_ptr, uint32_t info_size)
{
  struct frame_buffer* p = (struct frame_buffer*) bs->Buf_IO_struct;

  switch (info_ID)
    {
    case BYTECOUNT_INFO:
      {
	uint32_t *ptr = (uint32_t*)info_ptr;
	if (ptr && (info_size == sizeof(uint32_t)))
	  {
	    *ptr = p->frames[p->read_idx].data_size;
	  }
      }
      break;

    case ID_PICTURE_START_CODE:
      {
	__attribute__((__unused__)) struct pic_start_info *pph = (struct pic_start_info*)info_ptr;
      }
      break;

    case STATISTIC_INFO:
      {
        __attribute__((__unused__)) struct encode_stat_struct *stats = (struct encode_stat_struct*)info_ptr;
      }
      break;

    case CPB_FULLNESS:
      {
	__attribute__((__unused__)) struct cpb_fullness_struct *cpb = (struct cpb_fullness_struct*)info_ptr;
      }
      break;

    case TIME_STAMP_INFO:
      {
	__attribute__((__unused__)) struct sample_info_struct *timestamps = (struct sample_info_struct*)info_ptr;

	  p->frames[p->write_idx].original_pts = av_rescale_q(timestamps->rtStart, ONE_HUNDRED_NANOS, p->time_base);
      }
      break;

    case WMV_STREAM_INFO:
      {
	__attribute__((__unused__)) struct wmv_stream_info *wmv = (struct wmv_stream_info*)info_ptr;
      }
      break;

    case STREAM_FORMAT_INFO:
      {
	__attribute__((__unused__)) struct mc_struct_format_t *stream_format = (struct mc_struct_format_t *)info_ptr;
      }
      break;

    case ID_SEQ_START_CODE:
      {
	__attribute__((__unused__)) struct seq_start_info *seq_start = (struct seq_start_info *)info_ptr;
      }
      break;

    case ID_GOP_START_CODE:
      {
	__attribute__((__unused__)) struct gop_start_info *gop_start = (struct gop_start_info *)info_ptr;
      }
      break;

    case VIDEO_AU_CODE:
      {
	struct v_au_struct *au = (struct v_au_struct *) info_ptr;

	int64_t encoder_pts = av_rescale_q(au->PTS, TWENTY_SEVEN_MHZ, p->time_base);
	int64_t encoder_dts = av_rescale_q(au->DTS, TWENTY_SEVEN_MHZ, p->time_base);
        int64_t original_pts = p->frames[p->write_idx].original_pts;

        if (p->pts_diff == -1)
          {
            p->pts_diff = original_pts - encoder_pts;
          }

	p->frames[p->write_idx].flags = au->flags;
	p->frames[p->write_idx].type = au->type;
	p->frames[p->write_idx].pts = original_pts;
	p->frames[p->write_idx].dts = encoder_dts + p->pts_diff;
      }
      break;

    default:
      fprintf(stderr, "unhandled auxinfo %u\n", info_ID);
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
  struct frame_buffer* p = (struct frame_buffer*)bs->Buf_IO_struct;

  free(p);
  bs->Buf_IO_struct = NULL;
}


static void fw_free(bufstream_tt *bs)
{
  if (bs->Buf_IO_struct)
    bs->done(bs, 0);

  free(bs);
}


static int32_t init_mem_buf_write(bufstream_tt *bs, AVRational time_base)
{
  struct frame_buffer *p = malloc(sizeof(struct frame_buffer));

  if (!p)
    {
      return BS_ERROR;
    }

  bs->Buf_IO_struct = (struct impl_stream *)p;

  p->write_idx = -1;
  p->read_idx = 0;
  p->chunk_size = BUFFER_SIZE / 2;
  p->time_base = time_base;
  p->pts_diff = -1;

  for (int i = 0; i < NUM_FRAMES; i++) {
    p->frames[i].populated = 0;
  }

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


static bufstream_tt *open_mem_buf_write(AVRational time_base)
{
  bufstream_tt *p;
  p = (bufstream_tt*)malloc(sizeof(bufstream_tt));
  if (p)
    {
      if (init_mem_buf_write(p, time_base) != BS_OK)
	{
	  free(p);
	  p = NULL;
	}
    }
  return p;
}


static void info_printf(const char * fmt, ...)
{
  char lst[1024];
  va_list marker;

  va_start(marker, fmt);
  vsnprintf(lst, sizeof(lst), fmt, marker);
  va_end(marker);

  strncat(lst, "\r\n", sizeof(lst) - 1);
  fprintf(stderr, "%s\n", lst);
}


static void warn_printf(const char * fmt, ...)
{
  char lst[256];
  va_list marker;

  va_start(marker, fmt);
  vsnprintf(lst, sizeof(lst), fmt, marker);
  va_end(marker);

  fprintf(stderr, "%s\n", lst);
}


static void error_printf(const char * fmt, ...)
{
  char lst[256];
  va_list marker;

  va_start(marker,fmt);
  vsnprintf(lst, sizeof(lst), fmt, marker);
  va_end(marker);

  fprintf(stderr, "%s\n", lst);
}


static void progress_printf(int32_t percent, const char * fmt, ...)
{
  char lst[256];
  va_list marker;

  va_start(marker,fmt);
  vsnprintf(lst, sizeof(lst), fmt, marker);
  va_end(marker);

  fprintf(stderr, " %d - %s\n", percent, lst);
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

  return NULL;
}

static int get_video_type(int width, int height)
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
  struct encoder_frame *encoded_frame;

  if (frame == NULL)
    {
      if (!context->done)
	{
	  vc1OutVideoDone(context->v_encoder, 0);
	  context->done = 1;
	}
    }
  else
    {
      void * ext_info_stack[16] = {0};
      unsigned int option_flags = OPT_EXT_PARAM_TIMESTAMPS;
      void ** ext_info = &ext_info_stack[0];
      int half_width = ctx->width / 2;
      int half_height = ctx->height / 2;
      int y_plane_size = ctx->width * ctx->height;
      int uv_plane_size = half_width * half_height;
      int size = y_plane_size + uv_plane_size + uv_plane_size;
      uint8_t *b = malloc(size);
      struct sample_info_struct si;

      ext_info_stack[0] = &si;
      si.flags = 0;
      si.mode = 0;
      si.rtStart = av_rescale_q(frame->pts, ctx->time_base, ONE_HUNDRED_NANOS);
      si.rtStop = si.rtStart + (10000000 / context->v_settings->frame_rate);

      // Y Plane
      for (int i = 0; i < ctx->height; i++) {
	memcpy(b + (i * ctx->width), frame->data[0] + (frame->linesize[0] * i), ctx->width);
      }

      // U Plane
      for (int i = 0; i < half_height; i++) {
	memcpy(b + y_plane_size + (i * half_width), frame->data[1] + (frame->linesize[1] * i), half_width);
      }

      // V Plane
      for (int i = 0; i < half_height; i++) {
	memcpy(b + y_plane_size + uv_plane_size + (i * half_width), frame->data[2] + (frame->linesize[2] * i), half_width);
      }

      if (vc1OutVideoPutFrame(context->v_encoder, b, ctx->width, ctx->width, ctx->height, FOURCC_I420, option_flags, ext_info) == VC1ERROR_FAILED)
	{
	  exit(1);
	}

      free(b);
    }

  /*
  video_frame_tt v_frame;

  v_frame.four_cc = FOURCC_I420;
  v_frame.src[0].width = ctx->width;
  v_frame.src[0].height = ctx->height;
  v_frame.src[0].stride = frame->linesize[0];
  v_frame.src[0].plane = frame->data[0];

  v_frame.src[1].width = ctx->width >> 1;
  v_frame.src[1].height = ctx->height >> 1;
  v_frame.src[1].stride = frame->linesize[1];
  v_frame.src[1].plane = frame->data[1];

  v_frame.src[2].width = ctx->width >> 1;
  v_frame.src[2].height = ctx->height >> 1;
  v_frame.src[2].stride = frame->linesize[2];
  v_frame.src[2].plane = frame->data[2];

  if (vc1OutVideoPutFrameV(context->v_encoder, &v_frame, option_flags, ext_info) == VC1ERROR_FAILED)
    {
      printf("It failed\n");
      exit(1);
    }
  */

  encoded_frame = read_frame(context->videobs);

  if (encoded_frame) {

    ff_alloc_packet(pkt, encoded_frame->data_size);

    memcpy(pkt->data, encoded_frame->bfr, encoded_frame->data_size);

    pkt->pts = encoded_frame->pts;
    pkt->dts = encoded_frame->dts;
    pkt->duration = 1;

    if (encoded_frame->type == I_TYPE) {
      pkt->flags |= AV_PKT_FLAG_KEY;
    }

    *got_packet = 1;
  }
  else
    {
      *got_packet = 0;
    }
  return 0;
}

 static av_cold int VC1_close(AVCodecContext *avctx)
 {
  //VC1Context *context = avctx->priv_data;

   av_freep(&avctx->extradata);

  return 0;
}

 static av_cold int VC1_init(AVCodecContext *avctx)
 {
  VC1Context *context = avctx->priv_data;

  int init_options = 0;
  void * opt_list[10];
  int video_format;
  int profile;
  uint8_t paramSets[256];
  int32_t paramSetsLen;
  int video_type = get_video_type(avctx->width, avctx->height);

  context->done = 0;

  if (avctx->profile == FF_PROFILE_VC1_SIMPLE) {
    profile = VC1_PROFILE_SIMPLE;
  }
  else if (avctx->profile == FF_PROFILE_VC1_MAIN) {
    profile = VC1_PROFILE_MAIN;
  }
  else if (avctx->profile == FF_PROFILE_VC1_ADVANCED) {
    profile = VC1_PROFILE_ADVANCED;
  }
  else {
    fprintf(stderr, "Invalid profile %d\n", avctx->profile);
    exit(-1);
  }

  if (strcmp(context->video_format, "pal") == 0) {
    video_format = VM_PAL;
  }
  else if (strcmp(context->video_format, "ntsc") == 0) {
    video_format = VM_NTSC;
  }
  else {
    fprintf(stderr, "Invalid video_format %s\n", context->video_format);
    exit(-1);
  }

  context->v_settings = &context->param_set.params;

  vc1OutVideoDefaults(context->v_settings, video_type, video_format);

  context->v_settings->profile_id 		= profile;
  context->v_settings->level_id 		= avctx->level;
  context->v_settings->key_frame_interval       = avctx->gop_size >= 0 ? avctx->gop_size : context->v_settings->key_frame_interval;
  context->v_settings->b_frame_distance         = avctx->max_b_frames;
  context->v_settings->closed_entry             = VC1_CLOSED_ENTRY_OFF;
  context->v_settings->interlace_mode           = avctx->flags & CODEC_FLAG_INTERLACED_DCT ? VC1_INTERLACE_MBAFF : VC1_PROGRESSIVE;
  context->v_settings->def_horizontal_size      = avctx->width;
  context->v_settings->def_vertical_size        = avctx->height;
  context->v_settings->frame_rate               = ((double) avctx->time_base.den) / avctx->time_base.num;
  context->v_settings->bit_rate                 = avctx->bit_rate >= 0 ? avctx->bit_rate : context->v_settings->bit_rate;
  context->v_settings->max_bit_rate             = avctx->rc_max_rate >= 0 ? avctx->rc_max_rate : context->v_settings->bit_rate * 1.1;
  context->v_settings->bit_rate_mode            = VC1_VBR;
  context->v_settings->min_key_frame_interval   = 1;
  context->v_settings->enable_asf_binding       = context->asf_binding_byte ? 1 : 0;
  context->v_settings->num_threads              = avctx->thread_count;
  context->v_settings->sar_width = 1;
  context->v_settings->sar_height = 1;


  context->v_encoder = vc1OutVideoNew(get_rc, context->v_settings, 0, 0xFFFFFFFF, 0, 0);

  context->videobs = open_mem_buf_write(avctx->time_base);

  if(vc1OutVideoInit(context->v_encoder, context->videobs, init_options, &opt_list[0]))
    {
      fprintf(stderr, "vc1OutVideoInit failed\n");
      exit(1);
    }

  if (vc1OutVideoGetParSets(context->v_encoder,
			    context->v_settings,
			    paramSets,
			    &paramSetsLen) != VC1ERROR_NONE) {
    fprintf(stderr, "vc1OutVideoGetParSets failed\n");
    exit(1);
  }

  avctx->extradata = av_malloc(paramSetsLen);
  avctx->extradata_size = paramSetsLen;

  memcpy(avctx->extradata, paramSets, paramSetsLen);

  /*
  for (int i = 0; i < paramSetsLen; i++) {
    fprintf(stderr, "0x%1x ", paramSets[i]);
  }
  fprintf(stderr, "\n");
  */

  if (!avctx->codec_tag)
    avctx->codec_tag = AV_RL32("I420");

  return 0;
}

static av_cold void VC1_init_static(AVCodec *codec)
{
}

#define OFFSET(x) offsetof(VC1Context, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
  { "video_format", "Set the video format (pal | ntsc)", OFFSET(video_format), AV_OPT_TYPE_STRING, { .str = "pal" }, 0, 0, VE},
  { "asf_binding_byte", "Include the ASF binding byte", OFFSET(asf_binding_byte), AV_OPT_TYPE_INT, { .i64 = 1 }, 0, 1, VE},
  { NULL },
};

static const AVClass class = {
  .class_name = "libmcvc1",
  .item_name  = av_default_item_name,
  .option     = options,
  .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault VC1_defaults[] = {
  { NULL },
};

AVCodec ff_libmcvc1_encoder = {
  .name             = "libmcvc1",
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
