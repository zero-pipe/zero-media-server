#ifndef ZMS_ENGINE_MEDIA_LIMITS_H
#define ZMS_ENGINE_MEDIA_LIMITS_H

/**
 * ZMS 媒体层策略常量（非线格式魔数）。
 * 协议字面量见 rtmp.h / rtp.h / h264_over_rtp.c 等。
 */
#include "zms/engine/gop/gop_queue.h"

/** RTMP/RTSP/channel 大 I/O 缓冲（64KB）。 */
#define ZMS_MEDIA_IO_BUF_SIZE 65536u

/** WebRTC WHEP 播放出站（与 RTSP play_sender 调参对齐；仅 WebRTC）。 */
#define ZMS_WEBRTC_PLAY_RTPQ_CAP 512u
#define ZMS_WEBRTC_PLAY_RTPQ_HIGH_WATER 128u
#define ZMS_WEBRTC_PLAY_RTP_SLOT_MAX 2048u
#define ZMS_WEBRTC_PLAY_RTP_FLUSH 256
#define ZMS_WEBRTC_PLAY_CRYPT_BYTES 4096u
#define ZMS_WEBRTC_PLAY_DTLS_IO 2048u
#define ZMS_WEBRTC_PLAY_H264_STAP_MAX 512u

/** 并发 WHEP/WHIP 会话数与 libice agent→poller 映射容量。 */
#define ZMS_WEBRTC_SESSION_MAX 64u
#define ZMS_WEBRTC_ICE_AGENT_MAX 64u

/** VOD 容器 demux 初始缓冲；4K H.264 高码率 IDR 可达约 1.5MB，
 *  8K H.265 IDR 可达约 2MB。取 2MB 避免 mpegps/mpegts 静默丢帧。 */
#define ZMS_VOD_DEMUX_READ_MIN (2u * 1024u * 1024u)

/** MPEG-TS（SRT 推流）H264 访问单元上限；1080p IDR 常见约 10000KB。 */
#define ZMS_MPEGTS_AU_MAX ZMS_VOD_DEMUX_READ_MIN

#define ZMS_MEDIA_TS_REWIND_THRESHOLD_MS 5000u
/** 时间戳单调步进（gop_queue）。 */
#define ZMS_MEDIA_TS_MIN_STEP_MS 40u
/** AAC-LC 1024 samples @ 44.1kHz */
#define ZMS_AAC_FRAME_MS 23u
/** 音轨相对视频允许的最大落后（毫秒） */
#define ZMS_AV_SYNC_LAG_MS 200u

/** read_muxed A/V 交织偏差上限（与 sync lag 预算相同）。 */
#define ZMS_GOP_QUEUE_MUX_MAX_SKEW_MS ZMS_AV_SYNC_LAG_MS

/** RTP 重排序 jitter 窗口；槽位数与 GOP ring 相同。 */
#define ZMS_RTP_JITTER_SLOTS_DEFAULT ZMS_GOP_QUEUE_CAPACITY

/** RTP 排序/重排保持时间（毫秒），缺口跳过前；UDP 需更大，TCP interleaved 更小。 */
#define ZMS_RTP_JITTER_MS_UDP_DEFAULT 200
#define ZMS_RTP_JITTER_MS_TCP_DEFAULT 50

/*
 * GOP 队列容量/读 lag 策略常量（CAPACITY / MAX_GOP / TARGET_GOPS /
 * MAX_READERS / PLAY_MAX_LAG）统一定义于 zms/engine/gop/gop_limits.h，
 * 由 gop_queue.h 引入，避免多处重复定义。
 */

#endif /* ZMS_ENGINE_MEDIA_LIMITS_H */
