/*
 * Unpack bit-packed streams to formats supported by FFmpeg
 * Copyright (c) 2017 Savoir-faire Linux, Inc
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

/* Development sponsored by CBC/Radio-Canada */

/**
 * @file
 * Bitpacked
 */

#include "avcodec.h"
#include "internal.h"
#include "get_bits.h"
#include "libavutil/imgutils.h"

struct BitpackedContext {
    int (*decode)(AVCodecContext *avctx, AVFrame *frame,
                  AVPacket *pkt, int top_field);
    AVPacket *first_field_pkt;
};

/* For this format, it's a simple passthrough */
static int bitpacked_decode_uyvy422(AVCodecContext *avctx, AVFrame *frame,
                                    AVPacket *avpkt, int top_field)
{
    int ret;

    if (frame->interlaced_frame)
        return AVERROR_PATCHWELCOME;

    /* there is no need to copy as the data already match
     * a known pixel format */
    frame->buf[0] = av_buffer_ref(avpkt->buf);
    ret = av_image_fill_arrays(frame->data, frame->linesize, avpkt->data,
                               avctx->pix_fmt, avctx->width, avctx->height, 1);
    if (ret < 0) {
        av_buffer_unref(&frame->buf[0]);
        return ret;
    }

    return 0;
}

static int bitpacked_decode_yuv422p10(AVCodecContext *avctx, AVFrame *frame,
                                      AVPacket *avpkt, int top_field)
{
    uint64_t frame_size = (uint64_t)avctx->width * (uint64_t)avctx->height * 20;
    uint64_t packet_size = (uint64_t)avpkt->size * 8;
    int interlaced = frame->interlaced_frame;
    GetBitContext bc;
    uint16_t *y, *u, *v;
    int ret, i, j;

    /* check packet size depending on the interlaced/progressive format */
    if (interlaced) {
        if ((frame_size / 2) > packet_size)
            return AVERROR_INVALIDDATA;
    } else if (frame_size > packet_size) {
        return AVERROR_INVALIDDATA;
    }

    if (avctx->width % 2)
        return AVERROR_PATCHWELCOME;

    ret = init_get_bits(&bc, avpkt->data, avctx->width * avctx->height * 20);
    if (ret)
        return ret;

    /*
     * if the frame is interlaced, the avpkt we are getting is either the top
     * or the bottom field. If it's the bottom field, it contains all the odd
     * lines of the recomposed frame, so we start at offset 1.
     */
    i = (interlaced && !top_field) ? 1 : 0;

    /*
     * Packets from interlaced frames contain either even lines, or odd
     * lines, so increment by two in that case.
     */
    for (; i < avctx->height; interlaced ? i += 2 : i++) {
        y = (uint16_t*)(frame->data[0] + i * frame->linesize[0]);
        u = (uint16_t*)(frame->data[1] + i * frame->linesize[1]);
        v = (uint16_t*)(frame->data[2] + i * frame->linesize[2]);

        for (j = 0; j < avctx->width; j += 2) {
            *u++ = get_bits(&bc, 10);
            *y++ = get_bits(&bc, 10);
            *v++ = get_bits(&bc, 10);
            *y++ = get_bits(&bc, 10);
        }
    }

    return 0;
}

static av_cold int bitpacked_init_decoder(AVCodecContext *avctx)
{
    struct BitpackedContext *bc = avctx->priv_data;

    if (!avctx->codec_tag || !avctx->width || !avctx->height)
        return AVERROR_INVALIDDATA;

    if (avctx->codec_tag == MKTAG('U', 'Y', 'V', 'Y')) {
        if (avctx->bits_per_coded_sample == 16 &&
            avctx->pix_fmt == AV_PIX_FMT_UYVY422) {

            if (avctx->field_order > AV_FIELD_PROGRESSIVE) {
                av_log(avctx, AV_LOG_ERROR, "interlaced not yet supported for 8-bit\n");
                return AVERROR_PATCHWELCOME;
            }

            bc->decode = bitpacked_decode_uyvy422;
        } else if (avctx->bits_per_coded_sample == 20 &&
                 avctx->pix_fmt == AV_PIX_FMT_YUV422P10) {
            bc->decode = bitpacked_decode_yuv422p10;
        } else {
            return AVERROR_INVALIDDATA;
        }
    } else {
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static int bitpacked_decode(AVCodecContext *avctx, void *data, int *got_frame,
                            AVPacket *avpkt)
{
    struct BitpackedContext *bc = avctx->priv_data;
    int buf_size = avpkt->size;
    AVFrame *frame = data;
    int top_field = 0;
    int res;

    frame->pict_type = AV_PICTURE_TYPE_I;
    frame->key_frame = 1;

    if (avctx->field_order != AV_FIELD_PROGRESSIVE) {
        top_field = avpkt->flags & AV_PKT_FLAG_TOP_FIELD;
        frame->interlaced_frame = 1;
        frame->top_field_first = 1;
    }

    if (avctx->pix_fmt == AV_PIX_FMT_YUV422P10) {
        res = ff_get_buffer(avctx, frame, 0);
        if (res < 0)
            return res;
    }

    if (frame->interlaced_frame) {

        if (top_field) {
            bc->first_field_pkt = av_packet_clone(avpkt);
            return 0;

        } else if (bc->first_field_pkt) {
            /* Combine the 2 fields in a single frame.
             * N fields/s give N/2 frames/s. */
            res = bc->decode(avctx, frame, bc->first_field_pkt, 1);
            res += bc->decode(avctx, frame, avpkt, 0);

            av_packet_free(&bc->first_field_pkt);
        } else {
            return 0;
        }
    } else {
        res = bc->decode(avctx, frame, avpkt, 0);
    }

    if (res)
        return res;

    *got_frame = 1;
    return buf_size;
}

AVCodec ff_bitpacked_decoder = {
    .name   = "bitpacked",
    .long_name = NULL_IF_CONFIG_SMALL("Bitpacked"),
    .type = AVMEDIA_TYPE_VIDEO,
    .id = AV_CODEC_ID_BITPACKED,
    .priv_data_size        = sizeof(struct BitpackedContext),
    .init = bitpacked_init_decoder,
    .decode = bitpacked_decode,
    .capabilities = AV_CODEC_CAP_EXPERIMENTAL,
};
