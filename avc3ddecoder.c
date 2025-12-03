/************************************************************
 * AVC3D Wrapper - 动态 SPS 初始化版本
 * 完全兼容老版本逻辑（包括 wFrame / MV / FFmpeg 解码）
 * 不写死参数，避免线程池越界、mutex crash
 ************************************************************/

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/mem.h"
#include "libavutil/motion_vector.h"
#include <stdio.h>
#include "ldecod_api.h"

// ============ 重要开关 ============
#define ENABLE_MV_FILE  0

// ========= AVC3D 上下文结构体 ==========
typedef struct Avc3dDecoderContext {
    void *decHandle;          // 第三方解码器句柄
    AVFormatContext *fmt_ctx;

    AVPacket *buffer_pkt;
    AVFrame  *buffer_frame;

    // 第三方解码器尺寸
    int yuv_size[3];
    int inited_3d;            // ⭐ 新增：是否已经初始化第三方解码器

    // FFmpeg H.264 解码器
    AVCodec *h264_codec;
    AVCodecContext *h264_ctx;
    AVFrame *h264_frame;
    AVPacket *h264_pkt;

    // 保存 MV 和参考帧
    DecFrame wFrame;

    // FFmpeg decode() 指针
    int (*h264_decode_func)(AVCodecContext *, void *, int *, AVPacket *);

    int eof;
} Avc3dDecoderContext;


// ========== 计算 YUV 大小 ==========
static int calc_yuv_size(int w, int h, int yuv_size[3]) {
    yuv_size[0] = w * h;
    yuv_size[1] = yuv_size[0] / 4;
    yuv_size[2] = yuv_size[0] / 4;
    return 0;
}


// ========== FFmpeg H.264 解码器初始化 ==========
static int init_ffmpeg_h264(Avc3dDecoderContext *s, AVCodecContext *avctx) {
    int ret;

    s->h264_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!s->h264_codec) return AVERROR_INVALIDDATA;

    s->h264_ctx = avcodec_alloc_context3(s->h264_codec);
    if (!s->h264_ctx) return AVERROR(ENOMEM);

    // 拷贝参数
    s->h264_ctx->width     = avctx->width;
    s->h264_ctx->height    = avctx->height;
    s->h264_ctx->pix_fmt   = avctx->pix_fmt;
    s->h264_ctx->flags2   |= AV_CODEC_FLAG2_EXPORT_MVS;

    s->h264_decode_func = s->h264_codec->decode;

    ret = avcodec_open2(s->h264_ctx, s->h264_codec, NULL);
    if (ret < 0) return ret;

    s->h264_frame = av_frame_alloc();
    s->h264_pkt   = av_packet_alloc();

    return 0;
}


// ========== 初始化第三方解码器（首次 got_frame） ==========
static int init_third_decoder(Avc3dDecoderContext *s, AVCodecContext *avctx) {
    printf("[AVC3D] 初始化第三方解码器：%dx%d, pixfmt=%s\n",
           avctx->coded_width,
           avctx->coded_height,
           av_get_pix_fmt_name(avctx->pix_fmt)
    );

    calc_yuv_size(avctx->coded_width, avctx->coded_height, s->yuv_size);

    s->decHandle = xavc3d_decode_init(s->yuv_size);
    if (!s->decHandle)
        return AVERROR_EXTERNAL;

    // 分配参考帧
    for (int i = 0; i < INPUT_REF_LEN; i++) {
        s->wFrame.inputRef[i].data[0] = malloc(s->yuv_size[0]);
        s->wFrame.inputRef[i].data[1] = malloc(s->yuv_size[1]);
        s->wFrame.inputRef[i].data[2] = malloc(s->yuv_size[2]);
        s->wFrame.inputRef[i].pmv     = malloc(sizeof(RefMv) * (s->yuv_size[0] / 16));
        s->wFrame.inputRef[i].inuse   = 0;
    }

    s->inited_3d = 1;
    return 0;
}


// ========== init() ==========
static av_cold int avc3d_init(AVCodecContext *avctx) {
    Avc3dDecoderContext *s = avctx->priv_data;
    s->inited_3d = 0;

    // 缓存 pkt/frame
    s->buffer_pkt   = av_packet_alloc();
    s->buffer_frame = av_frame_alloc();

    // 初始化 FFmpeg H264（不会初始化第三方解码器）
    return init_ffmpeg_h264(s, avctx);
}


// ========== 解码函数 ==========
static int avc3d_decode(AVCodecContext *avctx, void *frame, int *got_frame, AVPacket *pkt)
{
    Avc3dDecoderContext *s = avctx->priv_data;
    int ret;
    int ff_got;

    *got_frame = 0;

    // 复制 pkt
    av_packet_unref(s->h264_pkt);
    ret = av_packet_ref(s->h264_pkt, pkt);
    if (ret < 0) return ret;

    // FFmpeg decode
    ff_got = 0;
    ret = s->h264_decode_func(s->h264_ctx, s->h264_frame, &ff_got, s->h264_pkt);
    if (ret < 0) {
        printf("[AVC3D] FFmpeg decode error: %s\n", av_err2str(ret));
        return ret;
    }

    // ============================
    // ⭐ 第一次真正拿到解码图像
    // ============================
    if (ff_got && !s->inited_3d) {

        printf("[AVC3D] 发现首个有效帧，启动第三方解码器！\n");
        printf("FFmpeg SPS = %dx%d  pixfmt=%s\n",
               s->h264_frame->width,
               s->h264_frame->height,
               av_get_pix_fmt_name(s->h264_frame->format));

        // 设置真实尺寸
        avctx->coded_width  = s->h264_frame->width;
        avctx->coded_height = s->h264_frame->height;
        avctx->pix_fmt      = s->h264_frame->format;

        ret = init_third_decoder(s, avctx);
        if (ret < 0) return ret;
    }

    // ================================
    // 如果第三方还没初始化，直接返回（继续喂数据）
    // ================================
    if (!s->inited_3d) {
        av_frame_unref(s->h264_frame);
        return pkt->size;
    }

    // ========= FFmpeg 解码数据复制进参考帧 ==========
    if (ff_got) {
        int ref_id = selectOneRefBuf(&s->wFrame);
        memcpy(s->wFrame.inputRef[ref_id].data[0], s->h264_frame->data[0], s->yuv_size[0]);
        memcpy(s->wFrame.inputRef[ref_id].data[1], s->h264_frame->data[1], s->yuv_size[1]);
        memcpy(s->wFrame.inputRef[ref_id].data[2], s->h264_frame->data[2], s->yuv_size[2]);
    }

    av_frame_unref(s->h264_frame);

    // ========= 调用第三方解码器 ==========
    DecPacket wPkt = { pkt->data, pkt->size, pkt->pts, pkt->dts };
    DecFrame *wFrame = &s->wFrame;

    AVFrame *tframe = frame;
    tframe->format  = avctx->pix_fmt;
    tframe->width   = avctx->coded_width;
    tframe->height  = avctx->coded_height * 2;

    if (!tframe->data[0]) {
        ret = av_frame_get_buffer(tframe, 0);
        if (ret < 0) return ret;
    }

    wFrame->data[0] = tframe->data[0];
    wFrame->data[1] = tframe->data[1];
    wFrame->data[2] = tframe->data[2];

    ret = xavc3d_decode_pkt(s->decHandle, wFrame, got_frame, &wPkt);
    if (ret < 0) return ret;

    if (*got_frame)
        tframe->pts = wFrame->pts;

    return pkt->size;
}


// ========== 刷新 ==========
static void avc3d_flush(AVCodecContext *avctx) {
    Avc3dDecoderContext *s = avctx->priv_data;

    if (s->h264_ctx) avcodec_flush_buffers(s->h264_ctx);
    if (s->decHandle) xavc3d_decode_flush(s->decHandle);
}


// ========== 关闭 ==========
static av_cold int avc3d_close(AVCodecContext *avctx) {
    Avc3dDecoderContext *s = avctx->priv_data;

    if (s->h264_ctx) {
        avcodec_close(s->h264_ctx);
        avcodec_free_context(&s->h264_ctx);
    }

    av_frame_free(&s->h264_frame);
    av_packet_free(&s->h264_pkt);

    if (s->decHandle)
        xavc3d_decode_close(&s->decHandle);

    av_packet_free(&s->buffer_pkt);
    av_frame_free(&s->buffer_frame);

    return 0;
}


// ========== codec 定义 ==========
AVCodec ff_avc3d_decoder = {
        .name           = "avc3d",
        .long_name      = "AVC3D H.264 Decoder",
        .type           = AVMEDIA_TYPE_VIDEO,
        .id             = AV_CODEC_ID_XAV3,
        .priv_data_size = sizeof(Avc3dDecoderContext),
        .init           = avc3d_init,
        .decode         = avc3d_decode,
        .close          = avc3d_close,
        .flush          = avc3d_flush,
        .capabilities   = AV_CODEC_CAP_DELAY,
};