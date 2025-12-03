/*
 * AVC3D 解码器动态库 - FFmpeg H264 延迟初始化版本
 *（最小改动版，只把 FFmpeg 初始化移动到 decode()）
 */

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/mem.h"
#include <stdio.h>
#include "ldecod_api.h"

#define VLC_CHECK 0

int g_num = 0;

// 解码器上下文
typedef struct Avc3dDecoderContext {

    void *decHandle;
    AVFormatContext *fmt_ctx;

    // packet/frame 缓冲
    AVPacket *buffer_pkt;
    AVFrame  *buffer_frame;

    // 第三方
    int yuv_size[3];
    DecFrame wFrame;
    int eof;

    // FFmpeg h264（延迟初始化）
    AVCodec *h264_codec;
    AVCodecContext *h264_ctx;
    AVFrame *h264_frame;
    AVPacket *h264_pkt;
    int (*h264_decode_func)(AVCodecContext *, void *, int *, AVPacket *);

    int ffmpeg_inited;   // **新增：标志 FFmpeg 是否初始化**
} Avc3dDecoderContext;



/* ----------------- FFmpeg H264 延迟初始化 ----------------- */
static int init_ffmpeg_if_needed(Avc3dDecoderContext *s, AVCodecContext *avctx)
{
    if (s->ffmpeg_inited)
        return 0;

    printf("[wrapper] 初始化 FFmpeg H264 解码器（延迟初始化）\n");

    s->h264_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!s->h264_codec) {
        av_log(avctx, AV_LOG_ERROR, "找不到 FFmpeg H264 解码器\n");
        return AVERROR(ENOMEM);
    }

    s->h264_ctx = avcodec_alloc_context3(s->h264_codec);
    if (!s->h264_ctx) return AVERROR(ENOMEM);

    /* 注意：不写死参数，交给 FFmpeg 从 SPS 自行解析 */
    s->h264_ctx->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS;  // 导出 MV

    s->h264_decode_func = s->h264_codec->decode;

    if (avcodec_open2(s->h264_ctx, s->h264_codec, NULL) < 0) {
        av_log(avctx, AV_LOG_ERROR, "FFmpeg H264 open2 失败\n");
        return AVERROR(ENOMEM);
    }

    s->h264_frame = av_frame_alloc();
    s->h264_pkt   = av_packet_alloc();

    s->ffmpeg_inited = 1;
    return 0;
}




/* ----------------- 初始化（第三方保持写死逻辑不动） ----------------- */
static av_cold int avc3d_init(AVCodecContext *avctx)
{
    Avc3dDecoderContext *s = avctx->priv_data;
    memset(s, 0, sizeof(*s));

    s->buffer_pkt   = av_packet_alloc();
    s->buffer_frame = av_frame_alloc();

    /* 你当前能跑的固定参数保持不动 */
    avctx->width  = 1920;
    avctx->height = 1080 * 2;
    avctx->coded_width  = 1920;
    avctx->coded_height = 1088 * 2;
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    s->yuv_size[0] = 1920 * 1088;
    s->yuv_size[1] = s->yuv_size[0] / 4;
    s->yuv_size[2] = s->yuv_size[0] / 4;

    /* 初始化第三方解码器（保持你旧逻辑） */
    s->decHandle = xavc3d_decode_init(s->yuv_size);

    s->ffmpeg_inited = 0;   // **关键：一开始不初始化 FFmpeg**

    printf("avc3d_init: 第三方解码器初始化完成，等待 FFmpeg 延迟初始化\n");
    return 0;
}



/* ----------------- 解码函数（FFmpeg 延迟初始化） ----------------- */
static int avc3d_decode(AVCodecContext *avctx, void *frame, int *got_frame, AVPacket *pkt)
{
    Avc3dDecoderContext *s = avctx->priv_data;
    *got_frame = 0;

    if (pkt) {
        av_packet_unref(s->buffer_pkt);
        av_packet_ref(s->buffer_pkt, pkt);
    }

    /* -------- 1) FFmpeg H264 延迟初始化 -------- */
    if (!s->ffmpeg_inited) {
        if (init_ffmpeg_if_needed(s, avctx) < 0) {
            printf("[wrapper] FFmpeg 初始化失败，跳过 FFmpeg 环节\n");
        }
    }

    /* -------- 2) FFmpeg H264 解码流程 -------- */
    if (s->ffmpeg_inited && s->buffer_pkt->size > 0) {

        int ff_got = 0;

        av_packet_unref(s->h264_pkt);
        av_packet_ref(s->h264_pkt, s->buffer_pkt);

        s->h264_decode_func(s->h264_ctx, s->h264_frame, &ff_got, s->h264_pkt);

        printf("FFmpeg 解码: got=%d width=%d height=%d\n",
               ff_got, s->h264_frame->width, s->h264_frame->height);

        av_frame_unref(s->h264_frame);
    }


    /* -------- 3) 第三方解码器（保持你之前的逻辑不动） -------- */
    if (s->buffer_pkt->size > 0) {

        DecFrame *wFrame = &s->wFrame;
        DecPacket wPkt;

        AVFrame *tframe = (AVFrame*)frame;
        tframe->width  = 1920;
        tframe->height = 2160;
        tframe->format = AV_PIX_FMT_YUV420P;

        if (!tframe->data[0]) {
            av_frame_get_buffer(tframe, 0);
        }

        wPkt.data = s->buffer_pkt->data;
        wPkt.size = s->buffer_pkt->size;
        wPkt.pts  = s->buffer_pkt->pts;
        wPkt.dts  = s->buffer_pkt->dts;

        wFrame->data[0] = tframe->data[0];
        wFrame->data[1] = tframe->data[1];
        wFrame->data[2] = tframe->data[2];

        int ret = xavc3d_decode_pkt(s->decHandle, wFrame, got_frame, &wPkt);

        if (*got_frame)
            tframe->pts = wFrame->pts;

        av_packet_unref(s->buffer_pkt);

        return pkt ? pkt->size : 0;
    }

    return 0;
}



/* ----------------- 清理函数 ----------------- */
static av_cold int avc3d_close(AVCodecContext *avctx)
{
    Avc3dDecoderContext *s = avctx->priv_data;

    if (s->h264_ctx) avcodec_free_context(&s->h264_ctx);
    av_frame_free(&s->h264_frame);
    av_packet_free(&s->h264_pkt);

    if (s->decHandle)
        xavc3d_decode_close(&s->decHandle);

    av_packet_free(&s->buffer_pkt);
    av_frame_free(&s->buffer_frame);

    return 0;
}


AVCodec ff_avc3d_decoder = {
        .name = "avc3d",
        .long_name = "AVC3D H.264 Decoder (delay-init)",
        .type = AVMEDIA_TYPE_VIDEO,
        .id = AV_CODEC_ID_XAV3,
        .priv_data_size = sizeof(Avc3dDecoderContext),
        .init  = avc3d_init,
        .decode = avc3d_decode,
        .close = avc3d_close,
        .capabilities = AV_CODEC_CAP_DELAY,
};