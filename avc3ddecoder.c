/*
 * AVC3D 解码器动态库 - FFmpeg 4.4 兼容版本
 */

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/frame.h"
#include "libavutil/pixfmt.h"
#include "libavutil/mem.h"
#include <stdio.h>
#include <time.h>
// #include <android/log.h>
#include "ldecod_api.h"
// 声明第三方解码器接口（已由外部定义）

//#define LOGD(format,...) __android_log_print(ANDROID_LOG_DEBUG, LOGTAG, "%s " format, __func__, __VA_ARGS__)

/* ==== 统一调试日志宏 ==== */
#define OLDAVC3D_TAG "[OLDAVC3D]"
#define LOGI(fmt, ...)  printf(OLDAVC3D_TAG "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...)  printf(OLDAVC3D_TAG "[ERR ] " fmt "\n", ##__VA_ARGS__)

/* ==== 上下文结构体 ==== */
typedef struct Avc3dDecoderContext {
    void *decHandle;          // 第三方解码器句柄
    AVFormatContext *fmt_ctx; // 输入格式上下文
    AVPacket *buffer_pkt;     // 缓存数据包
    AVFrame *buffer_frame;    // 缓存帧
    int yuv_size[3];
    int got_frame;            // 帧可用标志
    int eof;                  // 结束标志
    FILE* fp;
} Avc3dDecoderContext;

static int file_index = 1000;

/* ==== 工具：时间字符串 ==== */
static
char *
unittime_string_1 (double t)
{
    static char  buf[128];

    const char  *unit;
    int         prec;

    /* choose units and scale */
    if (t < 1e-6)
        t *= 1e9, unit = "ns";
    else if (t < 1e-3)
        t *= 1e6, unit = "us";
    else if (t < 1.0)
        t *= 1e3, unit = "ms";
    else
        unit = "s";

    /* want 4 significant figures */
    if (t < 1.0)
        prec = 4;
    else if (t < 10.0)
        prec = 3;
    else if (t < 100.0)
        prec = 2;
    else
        prec = 1;

    sprintf (buf, "%.*f%s", prec, t, unit);
    return buf;
}

/* ==== 计算 YUV 分量大小，并详细打印 ==== */
static int calculate_yuv_component_sizes_2(AVCodecContext *avctx, int yuv_sizes[3])
{
    LOGI("[yuv] calculate_yuv_component_sizes_2 ENTER");

    if (!avctx || !yuv_sizes) {
        LOGE("[yuv] invalid args: avctx=%p yuv_sizes=%p", avctx, yuv_sizes);
        return AVERROR(EINVAL);
    }

    enum AVPixelFormat pix_fmt;
    int width = 0, height = 0;

    /* 获取视频流参数（使用 coded_*） */
    width  = avctx->coded_width;
    height = avctx->coded_height;
    pix_fmt = avctx->pix_fmt;

    LOGI("[yuv] input avctx: width=%d height=%d coded_width=%d coded_height=%d pix_fmt=%d",
         avctx->width, avctx->height, avctx->coded_width, avctx->coded_height, pix_fmt);

    printf("-----calculate_yuv_component_sizes 0------\n");
    printf("pix_fmt = %d\n", pix_fmt);

    // 根据像素格式计算各分量大小
    switch (pix_fmt) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
        yuv_sizes[0] = width * height;         // Y分量
        yuv_sizes[1] = yuv_sizes[0] / 4;       // U分量
        yuv_sizes[2] = yuv_sizes[1];           // V分量
        break;

    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUYV422:
        yuv_sizes[0] = width * height;         // Y分量
        yuv_sizes[1] = yuv_sizes[0] / 2;       // U分量
        yuv_sizes[2] = yuv_sizes[1];           // V分量
        break;

    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
        yuv_sizes[0] = width * height;         // Y分量
        yuv_sizes[1] = yuv_sizes[0];           // U分量
        yuv_sizes[2] = yuv_sizes[1];           // V分量
        break;

    case AV_PIX_FMT_YUVA420P:
        yuv_sizes[0] = width * height;         // Y分量
        yuv_sizes[1] = yuv_sizes[0] / 4;       // U分量
        yuv_sizes[2] = yuv_sizes[1];           // V分量
        // 注意：YUVA420P还有A分量，此处未计算
        break;

    default:
        LOGE("[yuv] unsupported pix_fmt=%d, width=%d height=%d", pix_fmt, width, height);
        return -1;
    }

    LOGI("[yuv] calculated yuv_size: Y=%d U=%d V=%d", yuv_sizes[0], yuv_sizes[1], yuv_sizes[2]);

    return 0;
}
/* ============================================================
 * AVC3D DEBUG TOOL - 帮助分析 AVPacket 的内容结构
 * 你只需要在 decode() 中调用 debug_print_packet_info(pkt)
 * ============================================================ */

static void debug_print_packet_info(const AVPacket *pkt)
{
    if (!pkt) {
        LOGI("[debug] pkt == NULL, skip");
        return;
    }

    /* ---- 基础信息 ---- */
    LOGI("[debug] PKT INFO: size=%d pts=%lld dts=%lld flags=0x%x pos=%lld",
         pkt->size,
         (long long)pkt->pts,
         (long long)pkt->dts,
         pkt->flags,
         (long long)pkt->pos);

    /* ---- Dump 前 32 字节 ---- */
    int dump_len = pkt->size < 32 ? pkt->size : 32;
    char dump_buf[256] = {0};
    for (int i = 0; i < dump_len; i++) {
        sprintf(dump_buf + i*3, "%02X ", pkt->data[i]);
    }
    LOGI("[debug] PKT HEAD HEX (%d bytes): %s", dump_len, dump_buf);

    /* ---- 扫描 NALU type ---- */
    int nal_count[256] = {0};
    for (int i = 0; i + 4 < pkt->size; ) {
        if (pkt->data[i] == 0x00 && pkt->data[i+1] == 0x00 &&
            pkt->data[i+2] == 0x01) {
            int nal_unit = pkt->data[i+3] & 0x1F;  // H.264 NAL type
            nal_count[nal_unit]++;
            i += 4;
        } else if (pkt->data[i] == 0x00 && pkt->data[i+1] == 0x00 &&
                   pkt->data[i+2] == 0x00 && pkt->data[i+3] == 0x01) {
            int nal_unit = pkt->data[i+4] & 0x1F;
            nal_count[nal_unit]++;
            i += 5;
        } else {
            i++;
        }
    }

    /* ---- 输出出现过的 NALU 类型 ---- */
    char nal_log[512] = {0};
    char *p = nal_log;
    for (int t = 0; t < 32; t++) {
        if (nal_count[t] > 0) {
            p += sprintf(p, "type%d:%d ", t, nal_count[t]);
        }
    }
    if (p == nal_log)
        sprintf(nal_log, "(no NAL found)");

    LOGI("[debug] NALU TYPES: %s", nal_log);

    /* ---- MVC 扩展特征（NAL type 20） ---- */
    if (nal_count[20] > 0) {
        LOGI("[debug] MVC VIEW-1 NALU DETECTED (type 20) count=%d", nal_count[20]);
    } else {
        LOGI("[debug] no MVC extension NAL (type 20) in this pkt");
    }
}

/* ==== 解码器初始化 ==== */
static av_cold int avc3d_init(AVCodecContext *avctx)
{
    struct timespec start_time, end_time;
    double elapsed_time = 0;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    Avc3dDecoderContext *s = avctx->priv_data;

    LOGI("[init] ENTER avc3d_init avctx=%p priv=%p", avctx, s);
    LOGI("[init] avctx BEFORE: width=%d height=%d coded_width=%d coded_height=%d pix_fmt=%d framerate=%d/%d",
         avctx->width, avctx->height,
         avctx->coded_width, avctx->coded_height,
         avctx->pix_fmt,
         avctx->framerate.num, avctx->framerate.den);

    printf(" enter avc3d_init 0000_08080_1\n");
    //__android_log_print(ANDROID_LOG_ERROR, "avc3d_init", "enter avc3d_init 0000__0808\n");

    // 保存格式上下文（需要从外部传入）
    if (avctx->opaque) {
        s->fmt_ctx = (AVFormatContext*)avctx->opaque;
        LOGI("[init] fmt_ctx from avctx->opaque = %p", s->fmt_ctx);
    } else {
        av_log(avctx, AV_LOG_WARNING, "未提供AVFormatContext，可能影响解码效果\n");
        LOGE("[init] avctx->opaque is NULL, fmt_ctx will be NULL");
        s->fmt_ctx = NULL;
    }

    // 先按原有逻辑计算一次 yuv_size（使用 coded_*）
    if (calculate_yuv_component_sizes_2(avctx, s->yuv_size) == 0) {
        LOGI("[init] after calculate_yuv_component_sizes_2 ysize=%d, usize=%d, vsize=%d",
             s->yuv_size[0], s->yuv_size[1], s->yuv_size[2]);
    } else {
        LOGE("[init] calculate_yuv_component_sizes_2 FAILED, pix_fmt=%d", avctx->pix_fmt);
    }

    printf(" enter avc3d_init 0001 ysize=%d,usize=%d,vsize=%d\n",
           s->yuv_size[0], s->yuv_size[1], s->yuv_size[2]);

    // ==== 关键：老版本在这里“写死”尺寸 ====
    s->yuv_size[0] = 1920 * 1080;
    s->yuv_size[1] = 1920 * 1080 / 4;
    s->yuv_size[2] = 1920 * 1080 / 4;
    LOGI("[init] OVERRIDE yuv_size to FIXED: Y=%d U=%d V=%d",
         s->yuv_size[0], s->yuv_size[1], s->yuv_size[2]);

    void *decoder_handle = xavc3d_decode_init(s->yuv_size);
    // s->fp = fopen("./decoder_dump.YUV", "wb");

    printf(" enter avc3d_init 0002\n");
    LOGI("[init] xavc3d_decode_init called with yuv_size: %d,%d,%d handle=%p",
         s->yuv_size[0], s->yuv_size[1], s->yuv_size[2], decoder_handle);

    //printf("pDecoder->yuvsize[0]=%d,pDecoder->yuvsize[1]=%d,pDecoder->yuvsize[2]=%d\n",pDecoder->yuvsize[0],pDecoder->yuvsize[1],pDecoder->yuvsize[2]);
    if (!decoder_handle) {
        av_log(avctx, AV_LOG_ERROR, "第三方解码器初始化失败\n");
        LOGE("[init] xavc3d_decode_init FAILED");
        return AVERROR(ENOMEM);
    }
    s->decHandle = decoder_handle;

    // 分配内部缓存
    s->buffer_pkt = av_packet_alloc();
    s->buffer_frame = av_frame_alloc();
    LOGI("[init] buffer_pkt=%p buffer_frame=%p", s->buffer_pkt, s->buffer_frame);
    if (!s->buffer_pkt || !s->buffer_frame) {
        xavc3d_decode_close(&s->decHandle);
        av_packet_free(&s->buffer_pkt);
        av_frame_free(&s->buffer_frame);
        LOGE("[init] av_packet_alloc/av_frame_alloc failed");
        return AVERROR(ENOMEM);
    }

    // 设置默认像素格式
    if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
        avctx->pix_fmt = AV_PIX_FMT_YUV420P;
        LOGI("[init] avctx->pix_fmt was NONE, set to YUV420P");
    }

    // ==== 老版本写死输出参数 ====
    avctx->width         = 1920;
    avctx->height        = 1080 * 2;
    avctx->coded_width   = 1920;
    avctx->coded_height  = 1088 * 2;
    avctx->framerate.num = 24000;
    avctx->framerate.den = 1001;

    LOGI("[init] avctx AFTER OVERRIDE: width=%d height=%d coded_width=%d coded_height=%d pix_fmt=%d framerate=%d/%d",
         avctx->width, avctx->height,
         avctx->coded_width, avctx->coded_height,
         avctx->pix_fmt,
         avctx->framerate.num, avctx->framerate.den);

    // 计算时间差
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_time = (end_time.tv_sec + end_time.tv_nsec * 1e-9
                    - start_time.tv_sec - start_time.tv_nsec * 1e-9);

    printf("test123 avc3d_init execution time: %s (according to clock_gettime)\n",
           unittime_string_1 (elapsed_time));

    av_log(avctx, AV_LOG_INFO, "AVC3D 解码器初始化成功,  %.f seconds\n", elapsed_time);
    LOGI("[init] LEAVE avc3d_init, elapsed=%s", unittime_string_1 (elapsed_time));

    return 0;
}

/* ==== 解码函数 ==== */
static int avc3d_decode(AVCodecContext *avctx, void *frame, int *got_frame, AVPacket *pkt)
{
    Avc3dDecoderContext *s = avctx->priv_data;
    int ret = 0;
    static int g_out_no = 0;
    int64_t  pts_tmp = 0;
    struct timespec start_time, end_time;
    double elapsed_time = 0;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    LOGI("[decode] ENTER avc3d_decode avctx=%p ctx_decHandle=%p frame=%p pkt=%p",
         avctx, s ? s->decHandle : NULL, frame, pkt);

    // __android_log_print(ANDROID_LOG_ERROR, "avc3d_decode", "avc3d_decode fun=%s line=%d\n", __FUNCTION__,  __LINE__);
    *got_frame = 0;

    // 处理输入数据包
    if (pkt) {
        LOGI("[decode] input pkt: size=%d pts=%lld dts=%lld flags=0x%x pos=%lld",
             pkt->size, (long long)pkt->pts, (long long)pkt->dts,
             pkt->flags, (long long)pkt->pos);

        // ===== 调试：打印 packet 结构 =====
        debug_print_packet_info(pkt);
        
        ret = av_packet_ref(s->buffer_pkt, pkt);
        if (ret < 0) {
            LOGE("[decode] av_packet_ref FAILED ret=%d", ret);
            return ret;
        }
        LOGI("[decode] buffer_pkt after ref: size=%d pts=%lld dts=%lld",
             s->buffer_pkt->size,
             (long long)s->buffer_pkt->pts,
             (long long)s->buffer_pkt->dts);
    } else {
        s->eof = 1;
        LOGI("[decode] pkt == NULL, set eof=1");
    }

    unsigned char bytes[4];
    bytes[0] = (s->buffer_pkt->size >> 0) & 0xFF;
    bytes[1] = (s->buffer_pkt->size >> 8) & 0xFF;
    bytes[2] = (s->buffer_pkt->size >> 16) & 0xFF;
    bytes[3] = (s->buffer_pkt->size >> 24) & 0xFF;

    //printf(" enter avc3d_decode\n");
    /*
    char strbuff[288];
    memset(strbuff, 0, sizeof(strbuff));
    sprintf(strbuff, "./dump/f_%d.YUV",file_index++);
    s->fp = fopen(strbuff, "wb");

    fwrite(bytes, 1, 4, s->fp);
    fwrite(s->buffer_pkt->data, 1, s->buffer_pkt->size, s->fp);
    fclose(s->fp);
    s->fp = NULL;
    */

    // 调用第三方解码器
    if (s->buffer_pkt->size > 0 || s->eof) {

        //dec proc
        AVFrame *tframe = (AVFrame *)frame;
        DecPacket  wPkt;
        DecFrame   wFrame;

        LOGI("[decode] before setup tframe: avctx width=%d height=%d coded_width=%d coded_height=%d",
             avctx->width, avctx->height, avctx->coded_width, avctx->coded_height);

        // 修改数据前设置正确尺寸（老版本假设输出 height=原始高度的2倍）
        tframe->height = avctx->height;
        tframe->width  = avctx->width;
        tframe->linesize[0] = avctx->coded_width;
        tframe->linesize[1] = tframe->linesize[2] = avctx->coded_width / 2;
        tframe->format = AV_PIX_FMT_YUV420P; // avctx->pix_fmt;
        tframe->pts    = pkt ? pkt->pts : 0;

        printf("aaa tframe->height = %d,tframe->width=%d, tframe->format=%d\n ",
               tframe->height, tframe->width, tframe->format);
        LOGI("[decode] tframe init: w=%d h=%d coded_w=%d coded_h=%d fmt=%d pts=%lld",
             tframe->width, tframe->height,
             avctx->coded_width, avctx->coded_height,
             tframe->format, (long long)tframe->pts);

        // 分配缓冲区（根据width/height/format计算大小）
        if (!tframe->data[0] ||
            tframe->width  != avctx->height ||  /* 注意：这里是老代码的“奇怪条件”，保留 */
            tframe->height != avctx->width)
        {
            LOGI("[decode] av_frame_get_buffer needed: data0=%p, tframe(w=%d,h=%d) vs avctx(w=%d,h=%d)",
                 tframe->data[0],
                 tframe->width, tframe->height,
                 avctx->width, avctx->height);

            ret = av_frame_get_buffer(tframe, 0);  // 0表示按默认对齐方式分配
            printf("bbb tframe->height = %d,tframe->width=%d, tframe->format=%d  ret=%d \n ",
                   tframe->height, tframe->width, tframe->format , ret);

            LOGI("[decode] after av_frame_get_buffer: ret=%d data0=%p linesize[0]=%d [1]=%d [2]=%d",
                 ret,
                 tframe->data[0],
                 tframe->linesize[0], tframe->linesize[1], tframe->linesize[2]);

            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "分配帧缓冲区失败: %d\n", ret);
                LOGE("[decode] av_frame_get_buffer FAILED ret=%d", ret);
                return ret;
            }
            tframe->height = avctx->coded_height;
            tframe->width  = avctx->coded_width;

            LOGI("[decode] tframe size overridden to coded: w=%d h=%d",
                 tframe->width, tframe->height);
        }

        /* 绑定 DecFrame 输出指针（直接指向 tframe->data） */
        {
            int i;
            for (i = 0; i < 3; i++) {
                wFrame.data[i] = tframe->data[i];
            }
        }

        wPkt.size = s->buffer_pkt->size;
        wPkt.data = s->buffer_pkt->data;
        wPkt.pts  = s->buffer_pkt->pts;
        wPkt.dts  = s->buffer_pkt->dts;

        LOGI("[decode] call xavc3d_decode_pkt: in_size=%d pts=%lld dts=%lld tframe(w=%d,h=%d)",
             wPkt.size, (long long)wPkt.pts, (long long)wPkt.dts,
             tframe->width, tframe->height);

        ret = xavc3d_decode_pkt(s->decHandle, &wFrame, got_frame, &wPkt);

        if (pkt != NULL)
            printf("ccc22233  tframe->height = %d,tframe->width=%d, tframe->format=%d, ret=%d, pkt_pts=%lld \n ",
                   tframe->height, tframe->width, tframe->format , ret, pkt->pts);
        else
            printf("ccc22233  tframe->height = %d,tframe->width=%d, tframe->format=%d  ret=%d \n ",
                   tframe->height, tframe->width, tframe->format , ret);

        LOGI("[decode] xavc3d_decode_pkt ret=%d got_frame=%d out_pts=%lld out_w=%d out_h=%d",
             ret, *got_frame, (long long)wFrame.pts, tframe->width, tframe->height);

        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "解码错误: %d\n", ret);
            LOGE("[decode] decode error ret=%d", ret);
            return ret;
        }

        if (*got_frame) {
            // 重置缓冲区
            tframe->pts = wFrame.pts;
            // pts_tmp = ... (原有注释逻辑保留)
            av_packet_unref(s->buffer_pkt);

            printf("xxxx avc3d_decode frame->height = %d,frame->width=%d, got_frame=%d pts=%lld\n ",
                   tframe->height, tframe->width,*got_frame, tframe->pts);

            LOGI("[decode] GOT_FRAME: tframe(w=%d,h=%d) pts=%lld linesize0=%d",
                 tframe->width, tframe->height,
                 (long long)tframe->pts,
                 tframe->linesize[0]);
        } else {
            LOGI("[decode] no frame output this call (got_frame=0)");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_time = (end_time.tv_sec + end_time.tv_nsec * 1e-9
                    - start_time.tv_sec - start_time.tv_nsec * 1e-9);

    printf("test123 decoder execution time: %s (according to clock_gettime)\n",
           unittime_string_1 (elapsed_time));
    LOGI("[decode] LEAVE avc3d_decode elapsed=%s ret=%d final_return=%d",
         unittime_string_1 (elapsed_time),
         ret,
         (ret >= 0 ? (pkt ? pkt->size : 0) : ret));

    return ret >= 0 ? pkt ? pkt->size : 0 : ret;
}

/* ==== 刷新解码器 ==== */
static void avc3d_flush(AVCodecContext *avctx)
{
    struct timespec start_time, end_time;
    double elapsed_time = 0;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    Avc3dDecoderContext *s = avctx->priv_data;

    LOGI("[flush] ENTER avc3d_flush avctx=%p ctx_decHandle=%p", avctx, s ? s->decHandle : NULL);

    // 清除内部缓存
    av_packet_unref(s->buffer_pkt);
    av_frame_unref(s->buffer_frame);
    s->got_frame = 0;
    s->eof = 0;

    LOGI("[flush] buffers cleared: buffer_pkt=%p buffer_frame=%p got_frame=%d eof=%d",
         s->buffer_pkt, s->buffer_frame, s->got_frame, s->eof);

    // 刷新第三方解码器
    if (s->decHandle) {
        xavc3d_decode_flush(s->decHandle);
        LOGI("[flush] xavc3d_decode_flush called");
    } else {
        LOGI("[flush] decHandle is NULL, skip third-party flush");
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_time = (end_time.tv_sec + end_time.tv_nsec * 1e-9
                    - start_time.tv_sec - start_time.tv_nsec * 1e-9);

    //printf("avc3d_flush execution time: %.f seconds\n", elapsed_time);
    printf("test123 avc3d_flush execution time: %s (according to clock_gettime)\n",
           unittime_string_1 (elapsed_time));
    LOGI("[flush] LEAVE avc3d_flush elapsed=%s",
         unittime_string_1 (elapsed_time));
}

/* ==== 关闭解码器 ==== */
static av_cold int avc3d_close(AVCodecContext *avctx)
{
    Avc3dDecoderContext *s = avctx->priv_data;
    struct timespec start_time, end_time;
    double elapsed_time = 0;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    LOGI("[close] ENTER avc3d_close avctx=%p ctx_decHandle=%p", avctx, s ? s->decHandle : NULL);

    // 关闭第三方解码器
    if (s->decHandle) {
        xavc3d_decode_close(&s->decHandle);
        LOGI("[close] xavc3d_decode_close done, decHandle now=%p", s->decHandle);
        s->decHandle = NULL;
    } else {
        LOGI("[close] decHandle already NULL");
    }

    // 释放内部资源
    av_packet_free(&s->buffer_pkt);
    av_frame_free(&s->buffer_frame);
    LOGI("[close] buffer_pkt/frame freed: buffer_pkt=%p buffer_frame=%p",
         s->buffer_pkt, s->buffer_frame);
    // fclose(s->fp);

    av_log(avctx, AV_LOG_INFO, "AVC3D 解码器关闭\n");

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed_time = (end_time.tv_sec + end_time.tv_nsec * 1e-9
                    - start_time.tv_sec - start_time.tv_nsec * 1e-9);

    printf("test123 avc3d_close execution time: %s (according to clock_gettime)\n",
           unittime_string_1 (elapsed_time));
    LOGI("[close] LEAVE avc3d_close elapsed=%s",
         unittime_string_1 (elapsed_time));

    return 0;
}

/* ==== 解码器定义 ==== */
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