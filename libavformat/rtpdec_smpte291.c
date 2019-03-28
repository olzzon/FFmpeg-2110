/*
 * RTP Depacketization of raw ancillary data
 * Copyright (c) 2019 CBC/Radio-Canada
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

#include "libavcodec/get_bits.h"
#include "rtpdec_formats.h"
#include "libavutil/avstring.h"

#define EIA708_PKT_MAX 512
/*
 * https://tools.ietf.org/id/draft-ietf-payload-rtp-ancillary-10.xml
 */

struct PayloadContext {
    char *sampling;
    int interlaced;
    int field;
    uint32_t timestamp;
    uint8_t *frame;
    uint8_t *frame_p;
    uint8_t data_count;
};

static int smpte291_parse_format(AVStream *stream, PayloadContext *data)
{
    return 0;
}

static int smpte291_parse_fmtp(AVFormatContext *s, AVStream *stream,
                              PayloadContext *data, const char *attr,
                              const char *value)
{
    return 0;
}

static int smpte291_parse_sdp_line(AVFormatContext *s, int st_index,
                                  PayloadContext *data, const char *line)
{
    const char *p;

    if (st_index < 0)
        return AVERROR(EAGAIN);

    if (av_strstart(line, "fmtp:", &p)) {
        AVStream *stream = s->streams[st_index];
        int ret = ff_parse_fmtp(s, stream, data, p, smpte291_parse_fmtp);

        if (ret < 0)
            return ret;

        ret = smpte291_parse_format(stream, data);

        return ret;
    }

    return 0;
}

static int smpte291_finalize_packet(PayloadContext *data, AVPacket *pkt,
                                   int stream_index)
{
    int ret;
    int i;

    if (data->data_count) {
//         printf("Finalize packet: %d Bytes\n", data->data_count);

        pkt->stream_index = stream_index;
        ret = av_packet_from_data(pkt, data->frame, data->data_count);
        if (ret < 0) {
            av_freep(&data->frame);
        }

        printf("cc: ");
        for (i=0; i<16; i++) { printf("%X ", data->frame[i]); }
        printf("\n");
    }

    data->frame = NULL;
    data->frame_p = NULL;
    data->data_count = 0;

    return ret;
}

static int smpte291_handle_packet(AVFormatContext *ctx, PayloadContext *data,
                                 AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                                 const uint8_t * buf, int len,
                                 uint16_t seq, int flags)
{
    int length, line, offset, cont, field;
    const uint8_t *headers = buf + 2; /* skip extended seqnum */
    const uint8_t *payload = buf + 12;
    int payload_len = len - 12;
    uint8_t anc_count;
    uint16_t did, sdid, data_count;
    int ret, i,j;
    int missed_last_packet = 0;
    uint8_t cc_count;
    GetBitContext bc;

    anc_count = headers[2];
    av_log(ctx, AV_LOG_TRACE, "Anc: length=%d,%d count=%d, marker=%d\n",
            (headers[0] << 8) + headers[1], payload_len, anc_count,
            (flags & RTP_FLAG_MARKER));

    if (*timestamp != data->timestamp) {
        if (data->frame) {
            /*
             * if we're here, it means that two RTP packets didn't have the
             * same timestamp, which is a sign that they were packets from two
             * different frames, but we didn't get the flag RTP_FLAG_MARKER on
             * the first one of these frames (last packet of a frame).
             * Finalize the previous frame anyway by filling the AVPacket.
             */
            av_log(ctx, AV_LOG_ERROR, "Missed previous RTP Marker\n");
            missed_last_packet = 1;
            smpte291_finalize_packet(data, pkt, st->index);
        }

        data->frame = av_malloc(EIA708_PKT_MAX);
        data->frame_p = data->frame;

        data->timestamp = *timestamp;

        if (!data->frame) {
            av_log(ctx, AV_LOG_ERROR, "Out of memory.\n");
            return AVERROR(ENOMEM);
        }
    }

    if (anc_count) {
        ret = init_get_bits(&bc, payload, payload_len * 8);
        if (ret){
            av_log(ctx, AV_LOG_ERROR, "No get bit context.\n");
            return ret;
        }

        for (i=0; i<anc_count; i++) {
            did = (uint8_t)(get_bits(&bc, 10) & 0xFF);
            sdid = (uint8_t)(get_bits(&bc, 10) & 0xFF);
            data_count = (uint8_t)(get_bits(&bc, 10) & 0xFF );
            av_log(ctx, AV_LOG_TRACE, "     did,sdid=%X,%X data_count=%d\n", did, sdid, data_count);

            if ((did == 0x61) && (sdid == 0x01)) {
                get_bits(&bc, 10); // cpd id 0
                get_bits(&bc, 10); // cpd id 1 0x9669
                get_bits(&bc, 10); // cpd len
                get_bits(&bc, 10); // cpd frame rate
                get_bits(&bc, 10); // cpd flags
                get_bits(&bc, 20); // cpd hdr ...
                get_bits(&bc, 10); // cpd ccdata_id 0x72
                cc_count = get_bits(&bc, 10) & 0x1F; // cpd count

                for (j=0; j<cc_count*3; j++) {
                    *data->frame_p++ = (uint8_t)(get_bits(&bc, 10) & 0xFF);
                    data->data_count++;
                }
                /*
                if (cdp_length - 9 - 4 <  cc_count * 3) {
                    av_log(s, AV_LOG_ERROR, "wrong cdp size %d cc count %d\n", cdp_length, cc_count);
                    return AVERROR_INVALIDDATA;
                }
                avio_skip(s->pb, data_length - 9 - 4 - cc_count * 3);
                cdp_footer_id = avio_r8(s->pb);
                if (cdp_footer_id != 0x74) {
                    av_log(s, AV_LOG_ERROR, "wrong cdp footer section %x\n", cdp_footer_id);
                    return AVERROR_INVALIDDATA;
                }
                avio_rb16(s->pb); // cdp_ftr_sequence_cntr
                avio_r8(s->pb); // packet_checksum
                */
            }

            /* Next is 10-bit checksum and padding % 32bits*/
        }
    }

    if ((flags & RTP_FLAG_MARKER)) {
        return smpte291_finalize_packet(data, pkt, st->index);
    } else if (missed_last_packet) {
        return 0;
    }

    return AVERROR(EAGAIN);
}

const RTPDynamicProtocolHandler ff_smpte291_rtp_handler = {
    .enc_name           = "smpte291",
    .codec_type         = AVMEDIA_TYPE_SUBTITLE,
    .codec_id           = AV_CODEC_ID_EIA_608,
    .priv_data_size     = sizeof(PayloadContext),
    .parse_sdp_a_line   = smpte291_parse_sdp_line,
    .parse_packet       = smpte291_handle_packet,
};
