/**
 * @file module_registry.c
 * @brief 模块启动入口（组合根 / composition root）。
 *
 * zms_modules_register_all() 为 ZMS 唯一初始化入口，
 * 进程启动时由业务层（如 demo_server_runtime）调用一次。
 * 按依赖顺序注册内建实现：
 *
 *   1. segment_recorder   直播分片格式（HLS/DASH）
 *   2. rtp_payload        RTP payload 类型（H.264/H.265/AAC/opus 等）
 *   3. frame_codec        帧 codec 策略（key/config/GOP 规则）
 *   4. container          容器 demuxer / muxer
 *   5. live_ingest_codec  直播推流 codec 适配器
 *   6. vod_format         VOD 文件格式探测
 *   7. session_dispatch   协议 dispatch（RTMP/RTSP/HTTP/SRT/WebRTC）
 *
 * 各子系统 register_* 均幂等；此处无需额外守卫。
 */
#include "zms/media/codec/payload/payload_registry.h"
#include "zms/engine/module_registry.h"
#include "zms/egress/egress_segment_recorder.h"
#include "zms/live/play/hls/http_hls_segmenter.h"
#include "zms/live/play/dash/http_dash_segmenter.h"
#include "zms/session/session_dispatcher.h"
#include "zms/vod/io/vod_format.h"

/* 前向声明（定义见各自 .c）。 */
void zms_rtp_payload_register_all(void);
void zms_frame_codec_register_all(void);
void zms_container_register_all(void);
void zms_live_ingest_codec_register_all(void);

void zms_segment_recorder_register_builtins(void)
{
    static int registered;

    if (registered) {
        return;
    }
    registered = 1;
    zms_segment_recorder_register(zms_http_hls_segment_recorder_ops());
    zms_segment_recorder_register(zms_http_dash_segment_recorder_ops());
}

/**
 * 注册所有内建模块。
 * 幂等：多次调用仅执行一次；线程安全由调用方保证（启动单线程）。
 */
void zms_modules_register_all(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
    zms_segment_recorder_register_builtins(); /* HLS/DASH 录制器 */
    zms_rtp_payload_register_all();           /* RTP payload 类型 */
    zms_frame_codec_register_all();           /* 帧 codec 策略 */
    zms_container_register_all();             /* 容器 demuxer / muxer */
    zms_live_ingest_codec_register_all();     /* 直播 ingest codec 适配器 */
    zms_vod_format_register_all();            /* VOD 格式探测 */
    zms_session_dispatch_register_all();      /* 协议 dispatch */
}
