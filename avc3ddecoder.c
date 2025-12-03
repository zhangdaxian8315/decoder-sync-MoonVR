/*
 * AVC3D 解码器动态库 - FFmpeg 4.4 兼容版本
 */

 #include "libavcodec/avcodec.h"
 #include "libavformat/avformat.h"
 #include "libavutil/frame.h"
 #include "libavutil/pixfmt.h"
 #include "libavutil/pixdesc.h"  // 用于 av_get_pix_fmt_name
 #include "libavutil/mem.h"
 #include <stdio.h>
 #include <libavutil/motion_vector.h>
 #include "ldecod_api.h"

 
 
 #define VLC_CHECK  1

 int g_num = 0;


 // 解码器上下文结构体
 typedef struct Avc3dDecoderContext {
     void *decHandle;          // 第三方解码器句柄
     AVFormatContext *fmt_ctx; // 输入格式上下文
     AVPacket *buffer_pkt;     // 缓存数据包
     AVFrame *buffer_frame;    // 缓存帧
     DecFrame wFrame; //worked frame for buf copy and 


     int yuv_size[3];
     int got_frame;            // 帧可用标志
     int eof;                  // 结束标志
 
     // H264 解码器相关（FFmpeg默认软件解码器）
     AVCodec *h264_codec;      // FFmpeg的H.264解码器
     AVCodecContext *h264_ctx; // FFmpeg解码器上下文
     AVFrame *h264_frame;      // FFmpeg解码后的帧
     AVPacket *h264_pkt;       // 给FFmpeg的数据包
     AVFrame *cached_frame;    // 缓存FFmpeg解码后的帧（供第三方解码器使用）

     // ===== 新增：用来缓存左眼 Packet =====
     AVPacket *left_pkt;     // 缓存左眼
     int       has_left_pkt; // 是否已有左眼

    // 关键：H264 解码函数指针（替代直接调用 h264_decode_frame）
    int (*h264_decode_func)(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt);
 
 } Avc3dDecoderContext;
 
 
 static int calculate_yuv_component_sizes_2(AVCodecContext *avctx, int yuv_sizes[3])
 {
     printf("-----calculate_yuv_component_sizes 0------\n");
     if (!avctx || !yuv_sizes) {
         return AVERROR(EINVAL);
     }
 
     enum AVPixelFormat pix_fmt;
     int width, height;
 
     // 获取视频流参数
     width = avctx->coded_width;
     height = avctx->coded_height;
#if VLC_CHECK
     height = avctx->height/2;
 #endif
     pix_fmt = avctx->pix_fmt;
 
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
             break;
             
         default:
             return -1;
     }
     
     return 0;
 }

// 判断是否包含 NAL type 20（MVC 视图扩展，右眼）
static int packet_is_right_eye(const AVPacket *pkt)
{
    const uint8_t *p = pkt->data;
    int size = pkt->size;

    for (int i = 0; i < size - 4; i++) {
        // 查找起始码
        if (p[i] == 0 && p[i+1] == 0 &&
            (p[i+2] == 1 || (p[i+2] == 0 && p[i+3] == 1))) {

            // 找到 NAL 头
            int offset = (p[i+2] == 1) ? 3 : 4;
            uint8_t nal_type = p[i + offset] & 0x1F;

            if (nal_type == 20)
                return 1; // Right eye
        }
    }
    return 0; // Not right eye
}
 

 // 初始化FFmpeg默认H.264解码器
 static int init_ffmpeg_h264_decoder(Avc3dDecoderContext *s, AVCodecContext *avctx)
 {
     int ret;
 
     // 查找FFmpeg默认H.264软件解码器
     s->h264_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
     if (!s->h264_codec) {
         av_log(avctx, AV_LOG_ERROR, "找不到FFmpeg H.264解码器\n");
         return AVERROR(ENOMEM);
     }
 
     // 初始化解码器上下文
     s->h264_ctx = avcodec_alloc_context3(s->h264_codec);
     if (!s->h264_ctx) {
         av_log(avctx, AV_LOG_ERROR, "无法分配FFmpeg解码器上下文\n");
         return AVERROR(ENOMEM);
     }
 
     // 复制当前解码器参数到FFmpeg解码器
     s->h264_ctx->width = avctx->width;
     s->h264_ctx->height = avctx->height;
#if VLC_CHECK
     s->h264_ctx->height = avctx->height/2;
#endif
     s->h264_ctx->coded_width = avctx->coded_width;
     s->h264_ctx->coded_height = avctx->coded_height;
     s->h264_ctx->pix_fmt = avctx->pix_fmt;
     s->h264_ctx->framerate = avctx->framerate;

    // 关键：获取解码函数指针（替代直接调用 h264_decode_frame）
    s->h264_decode_func = s->h264_codec->decode;
     //s->h264_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟模式，减少缓存
     //s->h264_ctx->max_b_frames = 0; // 禁用B帧，避免重排序

     s->h264_ctx->flags2 |= AV_CODEC_FLAG2_EXPORT_MVS; //导出mv
 
     // 打开解码器
     ret = avcodec_open2(s->h264_ctx, s->h264_codec, NULL);
     if (ret < 0) {
         av_log(avctx, AV_LOG_ERROR, "无法打开FFmpeg H.264解码器: %s\n", av_err2str(ret));
         avcodec_free_context(&s->h264_ctx);
         return ret;
     }
 
     // 分配FFmpeg解码所需的帧和数据包
     s->h264_frame = av_frame_alloc();
     s->h264_pkt = av_packet_alloc();
     s->cached_frame = av_frame_alloc();
     if (!s->h264_frame || !s->h264_pkt || !s->cached_frame) {
         av_log(avctx, AV_LOG_ERROR, "无法分配FFmpeg帧或数据包\n");
         return AVERROR(ENOMEM);
     }
 
     av_log(avctx, AV_LOG_INFO, "FFmpeg H.264解码器初始化成功\n");
     return 0;
 }
 

 // 解码器初始化
 static av_cold int avc3d_init(AVCodecContext *avctx)
 {
     Avc3dDecoderContext *s = avctx->priv_data;
     int ret;
     void *decoder_handle;
 
     // 保存格式上下文
     if (avctx->opaque) {
         s->fmt_ctx = (AVFormatContext*)avctx->opaque;
     } else {
         av_log(avctx, AV_LOG_WARNING, "未提供AVFormatContext，可能影响解码效果\n");
         s->fmt_ctx = NULL;
     }

#if VLC_CHECK
     avctx->width = 1920;
     avctx->height = 1080*2;
     avctx->coded_width = 1920;
     avctx->coded_height = 1088*2; 
     avctx->framerate.num = 24000;
     avctx->framerate.den = 1001;
     avctx->pix_fmt = AV_PIX_FMT_YUV420P;
#endif
 
     // 计算YUV分量大小
     calculate_yuv_component_sizes_2(avctx, s->yuv_size);
     printf("enter avc3d_init 0001 ysize=%d, usize=%d, vsize=%d\n", 
            s->yuv_size[0], s->yuv_size[1], s->yuv_size[2]);

 
     // 初始化第三方解码器
     decoder_handle = xavc3d_decode_init(s->yuv_size);
     if (!decoder_handle) {
         av_log(avctx, AV_LOG_ERROR, "第三方解码器初始化失败\n");
         return AVERROR(ENOMEM);
     }
     s->decHandle = decoder_handle;
  ////////////////////////////////////////////////////////////////////////
  //init wframe
     for(int i = 0; i< INPUT_REF_LEN; i++)
     {
       s->wFrame.inputRef[i].data[0] = (unsigned char *)malloc(s->yuv_size[0]); 
       s->wFrame.inputRef[i].data[1] = (unsigned char *)malloc(s->yuv_size[1]); 
       s->wFrame.inputRef[i].data[2] = (unsigned char *)malloc(s->yuv_size[2]); 
       s->wFrame.inputRef[i].inuse = 0;
       //init mv
       s->wFrame.inputRef[i].pmv = (RefMv *)malloc(s->yuv_size[0]/(4*4)*sizeof(RefMv));
       //RefMv
     }
     // 分配内部缓存
     s->buffer_pkt = av_packet_alloc();
     s->buffer_frame = av_frame_alloc();
     if (!s->buffer_pkt || !s->buffer_frame) {
         xavc3d_decode_close(&s->decHandle);
         av_packet_free(&s->buffer_pkt);
         av_frame_free(&s->buffer_frame);
         return AVERROR(ENOMEM);
     }

     s->left_pkt = av_packet_alloc();
     s->has_left_pkt = 0;
     
     // 设置默认像素格式
     if (avctx->pix_fmt == AV_PIX_FMT_NONE) {
         avctx->pix_fmt = AV_PIX_FMT_YUV420P;
     }
 
     // 初始化FFmpeg默认H.264解码器
     ret = init_ffmpeg_h264_decoder(s, avctx);
     if (ret < 0) {
         av_log(avctx, AV_LOG_ERROR, "FFmpeg解码器初始化失败，退出\n");
         xavc3d_decode_close(&s->decHandle);
         av_packet_free(&s->buffer_pkt);
         av_frame_free(&s->buffer_frame);
         return ret;
     }
 
     av_log(avctx, AV_LOG_INFO, "AVC3D 解码器初始化成功\n");
     return 0;
 }


#if 0

/**
 * 向外部预先分配的 RefMv 数组中填充 4x4 子块 MV（外部需确保数组大小足够）
 * @param mvs 输入：FFmpeg 运动矢量数组
 * @param mv_count 输入：运动矢量数量
 * @param img_width 输入：图像宽度（像素）
 * @param img_height 输入：图像高度（像素）
 * @param ref_mv_out 输入：预先分配的 RefMv 数组（由外部负责分配和释放）
 * @return 成功填充的 4x4 子块数量；-1：失败（参数无效或数组大小不足）
 */
 static int store_mvs_as_4x4_scan(
    const AVMotionVector *mvs,
    int mv_count,
    int img_width,
    int img_height,
    RefMv *ref_mv_out
) {
    // 入参校验
    if (!mvs || mv_count <= 0 || img_width <= 0 || img_height <= 0 || !ref_mv_out) {
        fprintf(stderr, "Invalid input parameters\n");
        return -1;
    }

    // 校验图像宽高为 4 的倍数
    if (img_width % 4 != 0 || img_height % 4 != 0) {
        fprintf(stderr, "Image width/height must be multiples of 4\n");
        return -1;
    }

    // 计算所需的 4x4 子块总数（外部数组必须 >= 此大小）
    int sub4x4_per_row = img_width / 4;
    int sub4x4_per_col = img_height / 4;
    int total_4x4_needed = sub4x4_per_row * sub4x4_per_col;

    // （可选）如果能获取外部数组的实际大小，可在此处校验是否足够
    // （例如通过额外参数传入外部数组大小，这里简化处理）

    int success_count = 0;

    // 遍历运动矢量，填充到外部数组
    for (int i = 0; i < mv_count; i++) {
        const AVMotionVector *mv = &mvs[i];

        // 只处理 P 帧和 4 的倍数大小的块
        if (mv->source >= 0 || mv->w % 4 != 0 || mv->h % 4 != 0) {
            continue;
        }

        int sub_cols = mv->w / 4;
        int sub_rows = mv->h / 4;

        for (int j = 0; j < sub_rows; j++) {
            for (int k = 0; k < sub_cols; k++) {
                int sub_dst_x = mv->dst_x + k * 4;
                int sub_dst_y = mv->dst_y + j * 4;

                if (sub_dst_x < 0 || sub_dst_x + 4 > img_width ||
                    sub_dst_y < 0 || sub_dst_y + 4 > img_height) {
                    continue;
                }

                int global_row = sub_dst_y / 4;
                int global_col = sub_dst_x / 4;
                int index = global_row * sub4x4_per_row + global_col;

                // 检查索引是否在有效范围内（避免外部数组溢出）
                if (index >= 0 && index < total_4x4_needed) {
                    ref_mv_out[index].mv_x = (short)mv->motion_x;
                    ref_mv_out[index].mv_y = (short)mv->motion_y;
                    success_count++;
                }
            }
        }
    }

    printf("Filled %d 4x4 sub-block MVs (total needed: %d)\n", success_count, total_4x4_needed);
    return success_count;
}


#else


#define FF_MV_FILE_PATH_A "/app/ff_mvfile.csv"  // 宏定义，便于集中修改
const char ffMVFILEA[] =FF_MV_FILE_PATH_A;
/**
 * 保留原有功能：将 AVMotionVector 填充到 4x4 子块数组
 * 新增功能：将 AVMotionVector 完整信息及子块映射关系存储到 CSV 文件
 * @param mvs        输入运动向量数组
 * @param mv_count   运动向量数量
 * @param img_width  图像宽度（像素）
 * @param img_height 图像高度（像素）
 * @param ref_mv_out 输出 4x4 子块运动向量数组
 * @param filename   输出 CSV 文件名（新增参数，传 NULL 则不存储）
 * @return 成功填充的子块数量
 */
static int store_mvs_as_4x4_scan(
    const AVMotionVector *mvs,
    int mv_count,
    int img_width,
    int img_height,
    RefMv *ref_mv_out,
    const char *filename  // 新增：CSV 输出文件名
) {
    // 入参校验
    if (!mvs || mv_count <= 0 || img_width <= 0 || img_height <= 0 || !ref_mv_out) {
        fprintf(stderr, "Invalid input parameters\n");
        return -1;
    }

    // 校验图像宽高为 4 的倍数
    if (img_width % 4 != 0 || img_height % 4 != 0) {
        fprintf(stderr, "Image width/height must be multiples of 4\n");
        return -1;
    }

    // 计算所需的 4x4 子块总数
    int sub4x4_per_row = img_width / 4;
    int sub4x4_per_col = img_height / 4;
    int total_4x4_needed = sub4x4_per_row * sub4x4_per_col;

    // 打开 CSV 文件（filename 非 NULL 时才存储）
    FILE *fp = NULL;
    if (filename != NULL) {
        fp = fopen(filename, "a");
        if (fp == NULL) {
            perror("Failed to open MV CSV file");
            // 仅警告，不影响原有功能执行
        } else {
            // 写入 CSV 表头（包含原始 MV 信息和子块映射）

            fprintf(fp, "MV_Index,Source,Block_Width,Block_Height,"
                        "Block_Src_X,Block_Src_Y,"
                        "Block_Dst_X,Block_Dst_Y,"
                        "Motion_X,Motion_Y,"
                        //"SubBlock_Dst_X,SubBlock_Dst_Y,"
                        //"Global_SubBlock_Row,Global_SubBlock_Col,"
                        //"SubBlock_Index\n"
                        "mvscale"
                        "\n"
                    );
        }
    }

    int success_count = 0;

    // 遍历运动矢量，填充数组 + 存储文件
    for (int i = 0; i < mv_count; i++) {
        const AVMotionVector *mv = &mvs[i];


        // 写入 CSV 文件（若文件打开成功）
        if(g_num <=60)
            if (fp != NULL) {
                fprintf(fp, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                        i,                      // 原始 MV 索引
                        mv->source,             // 参考源
                        mv->w, mv->h,           // 原始块宽高
                        mv->src_x, mv->src_y,   // 原始块目标坐标
                        mv->dst_x, mv->dst_y,   // 原始块目标坐标
                        mv->motion_x, mv->motion_y, // 原始运动向量
                        (int)mv->motion_scale
                        //sub_dst_x, sub_dst_y,   // 4x4 子块目标坐标
                        //global_row, global_col, // 子块全局行列
                        //index
                    );                 // 子块全局索引
            }

        // 只处理 P 帧和 4 的倍数大小的块
    #if 0
        if (mv->source >= 0 || mv->w % 4 != 0 || mv->h % 4 != 0) {
            continue;
        }
    #endif
        int sub_cols = mv->w / 4;
        int sub_rows = mv->h / 4;

        for (int j = 0; j < sub_rows; j++) {
            for (int k = 0; k < sub_cols; k++) {
                int sub_dst_x = mv->dst_x + k * 4;
                int sub_dst_y = mv->dst_y + j * 4;            
                // 计算子块全局索引
                int global_row = sub_dst_y / 4;
                int global_col = sub_dst_x / 4;
                int index = global_row * sub4x4_per_row + global_col;
                //printf("----mvinfo sub_dst_x =%d,sub_dst_y=%d, mv->motion_x=%d, mv->motion_y=%d \n",sub_dst_x, sub_dst_y, mv->motion_x,mv->motion_y);

                // 填充到输出数组
                if (index >= 0 && index < total_4x4_needed) {
                    ref_mv_out[index].mv_x = (short)mv->motion_x;
                    ref_mv_out[index].mv_y = (short)mv->motion_y;
                    success_count++;
                }


            }
        }
    }

    // 关闭文件
    if (fp != NULL) {
        fclose(fp);
        printf("MV data stored to %s\n", filename);
    }

    printf("Filled %d 4x4 sub-block MVs (total needed: %d)\n", success_count, total_4x4_needed);
    return success_count;
}
#endif

 


 // 解码函数：FFmpeg和解码器并行处理原始码流，互不干扰
//static FILE *g_avc3d_file = NULL;
static int avc3d_decode(AVCodecContext *avctx, void *frame, int *got_frame, AVPacket *pkt)
{
    Avc3dDecoderContext *s = avctx->priv_data;
    int ret;
    int ffmpeg_got_frame;

    *got_frame = 0;

    // ========== 加在 avc3d_decode() 最前面（处理 pkt 之前） ==========
    if (pkt) {

        int is_right = packet_is_right_eye(pkt);

        if (!is_right) {
            // ---- 这是左眼：先缓存，不参与解码 ----
            av_packet_unref(s->left_pkt);
            av_packet_ref(s->left_pkt, pkt);
            s->has_left_pkt = 1;

            // 不进入后续 decode，直接返回本包大小
            *got_frame = 0;
            return pkt->size;
        }

        // ---- 这里是右眼：如果已经有左眼，则合帧 ----
        if (s->has_left_pkt) {

            // 生成一个新的“左右眼合成包” (AU)
            int merged_size = s->left_pkt->size + pkt->size;

            AVPacket *merged = av_packet_alloc();
            merged->data = av_malloc(merged_size);
            merged->size = merged_size;

            // 先拷贝左眼，再拷贝右眼
            memcpy(merged->data, s->left_pkt->data, s->left_pkt->size);
            memcpy(merged->data + s->left_pkt->size, pkt->data, pkt->size);

            merged->pts = pkt->pts;
            merged->dts = pkt->dts;

            // 用 merged 替换 pkt（交给后续流程）
            pkt = merged;

            // 清理左眼缓存
            s->has_left_pkt = 0;
            av_packet_unref(s->left_pkt);

            // 注意：我们故意不在这里 free merged，后续 FFmpeg/第三方会释放
        }
    }

    // 处理输入数据包
    if (pkt) {
        ret = av_packet_ref(s->buffer_pkt, pkt);
        if (ret < 0) return ret;
    } else {
        s->eof = 1;
    }
    


    g_num++;

    // 1. FFmpeg解码器：处理原始码流，解码后缓存（供联动使用）
      // ======================================
    // 1. FFmpeg 同步解码：发送 pkt 后强制接收帧
    // ======================================
    if (s->buffer_pkt->size > 0 || s->eof) {
        // 复制原始数据包到FFmpeg解码器（输入是原始码流）
        av_packet_unref(s->h264_pkt);
        ret = av_packet_ref(s->h264_pkt, s->buffer_pkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "复制数据包到FFmpeg失败: %s\n", av_err2str(ret));
            return ret;
        }

        ffmpeg_got_frame = 0;
        //printf("---h264 2---\n");
        ret = s->h264_decode_func(s->h264_ctx, s->h264_frame, &ffmpeg_got_frame,  s->h264_pkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "FFmpeg发送数据包失败: %s\n", av_err2str(ret));
            return ret;
        }

        // 接收FFmpeg解码后的帧（供联动）
        /////////////////////////////////////////////////////////
        ///copy now 
        //if(g_num & 1)
        if(ffmpeg_got_frame == 1)
        {
            //printf("----------------ff xxxcopy begin--------------------\n");
            int refindex = 0;
            refindex = selectOneRefBuf(&(s->wFrame));

            
            if(refindex == -1){
                //assert(0);
                printf("------refindex =  -1, not find refBuf---------\n");
                exit(0);
                return 0;
            }
            s->wFrame.inputRef[refindex].pts = s->h264_pkt->pts;
            s->wFrame.inputRef[refindex].dts = s->h264_pkt->dts;
            memcpy(s->wFrame.inputRef[refindex].data[0],s->h264_frame->data[0],s->yuv_size[0]);
            memcpy(s->wFrame.inputRef[refindex].data[1],s->h264_frame->data[1],s->yuv_size[1]);
            memcpy(s->wFrame.inputRef[refindex].data[2],s->h264_frame->data[2],s->yuv_size[2]);
            //printf("----------------ff copy end-------------------size0=%d, size1=%d, size2=%d -\n",s->yuv_size[0],s->yuv_size[1],s->yuv_size[2]);

                        // 提取MV并存储

            AVFrameSideData *sd = av_frame_get_side_data(s->h264_frame, AV_FRAME_DATA_MOTION_VECTORS);
            if(sd) 
            {
                const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
                int mv_count = sd->size / sizeof(AVMotionVector);
                memset(s->wFrame.inputRef[refindex].pmv, 0, sizeof(RefMv)*1920*1080/16);
                store_mvs_as_4x4_scan(mvs, mv_count, 1920, 1080, s->wFrame.inputRef[refindex].pmv, ffMVFILEA);

                //printf("----------------ff copy end（帧号：%d）-------------------\n", g_num);
            } else {
                //printf("帧号：%d，未提取到MV\n", g_num);
            }
        }
        printf("ffmpeg解码成功：pts=%lld, 宽=%d, 高=%d， ffmpeg_got_frame=%d\n", s->h264_frame->pts, s->h264_frame->width, s->h264_frame->height,ffmpeg_got_frame);
        av_frame_unref(s->h264_frame);
        
    }

    // 2. 第三方解码器：处理原始码流（恢复原始输入，与FFmpeg并行）
    if (s->buffer_pkt->size > 0 || s->eof) {
        AVFrame *tframe = (AVFrame *)frame;
        DecPacket wPkt;
        //DecFrame wFrame;
        DecFrame *wFrame = &(s->wFrame);

        // 设置第三方解码器输出帧属性

        tframe->height = avctx->coded_height * 2;
    #if VLC_CHECK
        tframe->height = 2160;
    #endif
        tframe->width = avctx->coded_width;
        tframe->format = avctx->pix_fmt;

        // 分配第三方解码器输出缓冲区
        if (!tframe->data[0] || 
            tframe->width != avctx->coded_width || 
            tframe->height != avctx->coded_height * 2) {
            ret = av_frame_get_buffer(tframe, 0);
            if (ret < 0) {
                av_log(avctx, AV_LOG_ERROR, "分配第三方帧缓冲区失败: %d\n", ret);
                return ret;
            }
        }

        // 关键修正：第三方解码器输入是原始码流（与FFmpeg相同的数据包）
        wPkt.data = s->buffer_pkt->data;    // 原始码流数据（而非FFmpeg的YUV）
        wPkt.size = s->buffer_pkt->size;    // 原始码流大小
        wPkt.pts = s->buffer_pkt->pts;      // 原始时间戳
        wPkt.dts = s->buffer_pkt->dts;

        // 第三方解码器输出缓冲区

        wFrame->data[0] = tframe->data[0];
        wFrame->data[1] = tframe->data[1];
        wFrame->data[2] = tframe->data[2];

        printf("----avc3ddecoder input pts = %lld \n",wPkt.pts);


        // 调用第三方解码器（输入原始码流，恢复其正常解码逻辑）
        //ret = xavc3d_decode_pkt(s->decHandle, &wFrame, got_frame, &wPkt);
        ret = xavc3d_decode_pkt(s->decHandle, wFrame, got_frame, &wPkt);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "第三方解码错误: %d\n", ret);
            return ret;
        }

        if (*got_frame) {
            tframe->pts = wFrame->pts;   
        }

        printf("第三方解码成功：pts=%lld, 宽=%d, 高=%d, 3d_got_frame =%d\n", tframe->pts, tframe->width, tframe->height,got_frame);
        av_packet_unref(s->buffer_pkt);
    }

    return (ret >= 0) ? (pkt ? pkt->size : 0) : ret;
}
 
 // 刷新解码器
 static void avc3d_flush(AVCodecContext *avctx)
 {
     Avc3dDecoderContext *s = avctx->priv_data;
     
     // 清除内部缓存
     av_packet_unref(s->buffer_pkt);
     av_frame_unref(s->buffer_frame);
     av_frame_unref(s->h264_frame);
     av_frame_unref(s->cached_frame);
     av_packet_unref(s->h264_pkt);
     s->got_frame = 0;
     s->eof = 0;
     
     // 刷新两个解码器
     if (s->decHandle) {
         xavc3d_decode_flush(s->decHandle);
     }
     if (s->h264_ctx) {
         avcodec_flush_buffers(s->h264_ctx);
     }
 }
 
 
 // 关闭解码器
 static av_cold int avc3d_close(AVCodecContext *avctx)
 {
     Avc3dDecoderContext *s = avctx->priv_data;
     
     // 释放FFmpeg解码器资源
     if (s->h264_ctx) {
         avcodec_close(s->h264_ctx);
         avcodec_free_context(&s->h264_ctx);
     }
     av_frame_free(&s->h264_frame);
     av_packet_free(&s->h264_pkt);
     av_frame_free(&s->cached_frame);
 
     // 关闭第三方解码器
     if (s->decHandle) {
         xavc3d_decode_close(&s->decHandle);
         s->decHandle = NULL;
     }
     
     // 释放内部资源
     av_packet_free(&s->buffer_pkt);
     av_frame_free(&s->buffer_frame);
     av_packet_free(&s->left_pkt);
     
     av_log(avctx, AV_LOG_INFO, "AVC3D 解码器关闭\n");
     return 0;
 }
 
 
 // 解码器定义
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