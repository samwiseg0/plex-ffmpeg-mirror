/*
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
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavcodec/bsf.h"
#include "libavcodec/bsf_internal.h"
#include "libavcodec/cbs.h"
#include "libavcodec/cbs_bsf.h"
#include "libavcodec/cbs_av1.h"
#include "libavcodec/cbs_h265.h"
#include "libavcodec/dovi_rpu.h"
#include "libavcodec/h2645data.h"
#include "libavcodec/h265_profile_level.h"
#include "libavcodec/itut35.h"
#include "libavcodec/hevc.h"
typedef struct DoviRpuContext {
    CBSBSFContext common;
    DOVIContext dec;
    DOVIContext enc;
    int strip;
    int compression;
} DoviRpuContext;

static int dovi_rpu_update_fragment_hevc(AVBSFContext *bsf, AVPacket *pkt,
                                         CodedBitstreamFragment *au)
{
    DoviRpuContext *s = bsf->priv_data;
    CodedBitstreamUnit *nal = au->nb_units ? &au->units[au->nb_units - 1] : NULL;
    if (!nal || nal->type != HEVC_NAL_UNSPEC62)
        return 0;
    if (s->strip) {
        ff_cbs_delete_unit(au, au->nb_units - 1);
        return 0;
    }
    return 0;
}
static int dovi_rpu_update_fragment_av1(AVBSFContext *bsf, AVPacket *pkt,
                                        CodedBitstreamFragment *frag)
{
    DoviRpuContext *s = bsf->priv_data;
    int provider_code, provider_oriented_code;
    for (int i = 0; i < frag->nb_units; i++) {
        AV1RawOBU *obu = frag->units[i].content;
        AV1RawMetadataITUTT35 *t35 = &obu->obu.metadata.metadata.itut_t35;
        if (frag->units[i].type != AV1_OBU_METADATA ||
            obu->obu.metadata.metadata_type != AV1_METADATA_TYPE_ITUT_T35 ||
            t35->itu_t_t35_country_code != ITU_T_T35_COUNTRY_CODE_US ||
            t35->payload_size < 6)
            continue;
        provider_code = AV_RB16(t35->payload);
        provider_oriented_code = AV_RB32(t35->payload + 2);
        if (provider_code != ITU_T_T35_PROVIDER_CODE_DOLBY ||
            provider_oriented_code != 0x800)
            continue;
        if (s->strip) {
            ff_cbs_delete_unit(frag, i);
            return 0;
        }
        break; /* should be only one RPU per packet */
    }
    return 0;
}
static const CBSBSFType dovi_rpu_hevc_type = {
    .codec_id        = AV_CODEC_ID_HEVC,
    .fragment_name   = "access unit",
    .unit_name       = "NAL unit",
    .update_fragment = &dovi_rpu_update_fragment_hevc,
};
static const CBSBSFType dovi_rpu_av1_type = {
    .codec_id        = AV_CODEC_ID_AV1,
    .fragment_name   = "temporal unit",
    .unit_name       = "OBU",
    .update_fragment = &dovi_rpu_update_fragment_av1,
};
static int dovi_rpu_init(AVBSFContext *bsf)
{
    int ret;
    DoviRpuContext *s = bsf->priv_data;
    s->dec.logctx = s->enc.logctx = bsf;
    if (s->strip) {
        av_packet_side_data_remove(bsf->par_out->coded_side_data,
                                   &bsf->par_out->nb_coded_side_data,
                                   AV_PKT_DATA_DOVI_CONF);
    }
    else {
        av_log(bsf, AV_LOG_ERROR, "Only stripping metadata is currently supported");
        return AVERROR(EINVAL);
    }
    switch (bsf->par_in->codec_id) {
    case AV_CODEC_ID_HEVC:
        return ff_cbs_bsf_generic_init(bsf, &dovi_rpu_hevc_type);
    case AV_CODEC_ID_AV1:
        return ff_cbs_bsf_generic_init(bsf, &dovi_rpu_av1_type);
    default:
        return AVERROR_BUG;
    }
}
static void dovi_rpu_close(AVBSFContext *bsf)
{
    DoviRpuContext *s = bsf->priv_data;
    ff_dovi_ctx_unref(&s->dec);
    ff_dovi_ctx_unref(&s->enc);
    ff_cbs_bsf_generic_close(bsf);
}
#define OFFSET(x) offsetof(DoviRpuContext, x)
#define FLAGS (AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_BSF_PARAM)
static const AVOption dovi_rpu_options[] = {
    { "strip",          "Strip Dolby Vision metadata",  OFFSET(strip),       AV_OPT_TYPE_BOOL,  { .i64 = 0 }, 0, 1, FLAGS },
    { NULL }
};
static const AVClass dovi_rpu_class = {
    .class_name = "dovi_rpu_bsf",
    .item_name  = av_default_item_name,
    .option     = dovi_rpu_options,
    .version    = LIBAVUTIL_VERSION_INT,
};
static const enum AVCodecID dovi_rpu_codec_ids[] = {
    AV_CODEC_ID_HEVC, AV_CODEC_ID_AV1, AV_CODEC_ID_NONE,
};
const FFBitStreamFilter ff_dovi_rpu_bsf = {
    .p.name         = "dovi_rpu",
    .p.codec_ids    = dovi_rpu_codec_ids,
    .p.priv_class   = &dovi_rpu_class,
    .priv_data_size = sizeof(DoviRpuContext),
    .init           = &dovi_rpu_init,
    .close          = &dovi_rpu_close,
    .filter         = &ff_cbs_bsf_generic_filter,
};