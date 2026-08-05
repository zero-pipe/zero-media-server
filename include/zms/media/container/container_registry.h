#ifndef ZMS_CONTAINER_CONTAINER_REGISTRY_H
#define ZMS_CONTAINER_CONTAINER_REGISTRY_H

/**
 * @file container_registry.h
 * @brief 容器格式注册表公共接口。
 *
 * 定义两套 vtable（操作表）：
 *   - zms_container_demuxer_ops：解复用器接口，负责从字节流/文件中拆出 ES 包
 *   - zms_container_muxer_ops  ：复用器接口，负责将 ES 帧封装成容器分段
 *
 * 启动时由 zms_modules_register_all() → zms_container_register_all() 统一注册所有
 * 内置格式，之后通过 zms_container_demuxer_find() / zms_container_muxer_find() 按
 * zms_container_id 查询对应 vtable。
 *
 * 下游消费者：demux_pipeline（流式拆包）、mp4_vod_reader（文件 VOD）、
 * http_hls_segmenter / vod_hls (HLS pack), egress/mpegts (SRT continuous TS).
 */
#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 容器格式 ID。
 * 值同时用作注册表下标，因此不得随意重排；新格式追加在末尾。
 */
typedef enum zms_container_id {
    ZMS_CONTAINER_INVALID = 0,
    ZMS_CONTAINER_RTSP_INTERLEAVED, /**< RTSP $-interleaved 帧（内含 RTP） */
    ZMS_CONTAINER_FLV_TAG,          /**< FLV tag 流（RTMP/HTTP-FLV 推流） */
    ZMS_CONTAINER_MPEGTS,           /**< MPEG-TS（HLS / SRT） */
    ZMS_CONTAINER_MP4,              /**< ISO BMFF / MP4 文件 */
    ZMS_CONTAINER_MKV,              /**< Matroska / WebM 文件 */
    ZMS_CONTAINER_FLV_FILE,         /**< FLV 文件（VOD） */
} zms_container_id;

/** 容器包的类型标签，用于 zms_container_packet.kind */
typedef enum zms_container_pkt_kind {
    ZMS_CONTAINER_PKT_RTP = 1,    /**< RTP 包（来自 RTSP interleaved） */
    ZMS_CONTAINER_PKT_FLV_TAG,    /**< FLV tag（type_id + body） */
    ZMS_CONTAINER_PKT_MP4_SAMPLE, /**< MP4/MKV sample */
} zms_container_pkt_kind;

/**
 * 容器层解出的单个数据包，由 on_packet 回调传递给上层。
 * 字段按 kind 有效范围：
 *   - RTP        ：channel、data、len
 *   - FLV_TAG    ：flv_type_id、tag_dts_ms、data、len
 *   - MP4_SAMPLE ：track_id、pts_ms、dts_ms、codec、key、config，
 *                  config 包时还有 width/height/sample_rate/channels
 */
typedef struct zms_container_packet {
    zms_container_pkt_kind kind; /**< 包类型 */
    uint8_t channel;             /**< RTSP interleaved channel */
    uint8_t flv_type_id;         /**< FLV tag type（8=audio, 9=video, 18=script） */
    uint32_t tag_dts_ms;         /**< FLV tag DTS（毫秒） */
    const uint8_t *data;         /**< 载荷指针（生命周期与 feed/pump 调用帧绑定） */
    size_t len;                  /**< 载荷长度 */
    /* --- MP4/MKV sample 字段 --- */
    uint32_t track_id;  /**< 轨道 ID */
    uint32_t pts_ms;    /**< 显示时间戳（毫秒） */
    uint32_t dts_ms;    /**< 解码时间戳（毫秒） */
    zms_codec_id codec; /**< 编码格式 */
    int key;            /**< 1 = 关键帧 */
    int config;         /**< 1 = codec 参数包（SPS/PPS/ASC 等） */
    /* config 包时有效的轨道元数据 */
    uint16_t width;       /**< 视频宽度（像素） */
    uint16_t height;      /**< 视频高度（像素） */
    uint32_t sample_rate; /**< 音频采样率（Hz） */
    uint8_t channels;     /**< 音频声道数 */
} zms_container_packet;

/** 容器包回调函数类型；pkt 生命周期仅在回调内有效，需要保留请自行拷贝 */
typedef void (*zms_container_packet_cb)(const zms_container_packet *pkt, void *user);

/**
 * 创建 demuxer 实例时传入的配置。
 * on_packet 在每次解出一个完整容器包时被调用。
 */
typedef struct zms_container_demux_opts {
    zms_container_id id;               /**< 期望的容器格式 */
    zms_container_packet_cb on_packet; /**< 包就绪回调 */
    void *user;                        /**< 回调用户指针 */
} zms_container_demux_opts;

/**
 * demuxer vtable（操作表）。
 * 各格式在自己的 .c 文件中实现并导出一个全局常量实例，
 * 由 zms_container_register_all() 注册到注册表。
 *
 * 流式格式（RTSP interleaved、FLV tag、MPEG-TS）实现 feed()；
 * 文件格式（MP4、MKV、FLV 文件）实现 open_file()/pump()/seek() 等。
 * 不适用的槽位设为 NULL。
 */
typedef struct zms_container_demuxer_ops {
    zms_container_id id; /**< 本 vtable 对应的容器 ID */
    const char *name;    /**< 格式名称（日志用） */
    /** 创建 demuxer 实例；失败返回 NULL */
    void *(*create)(const zms_container_demux_opts *opts);
    /** 销毁实例，释放所有资源 */
    void (*destroy)(void *ctx);
    /** 流式喂数据（RTSP interleaved / MPEG-TS 等）；文件型可 NULL */
    ztk_err_t (*feed)(void *ctx, const uint8_t *buf, size_t len);
    /** 输入单条 FLV tag；非 FLV 容器可 NULL */
    ztk_err_t (*input_tag)(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                           uint32_t tag_dts_ms);
    /** 打开文件（MP4/MKV/FLV 文件）；流式容器可 NULL */
    ztk_err_t (*open_file)(void *ctx, const char *path);
    /** 关闭文件 */
    void (*close_file)(void *ctx);
    /** 读取一批样本并触发 on_packet；返回 >0 交付数、0 EOF、<0 错误 */
    int (*pump)(void *ctx);
    /** 跳转到指定时间（毫秒，in/out）；不支持可 NULL */
    ztk_err_t (*seek)(void *ctx, int64_t *seek_ms);
    /** 返回文件总时长（毫秒）；不支持可 NULL */
    uint64_t (*duration_ms)(void *ctx);
} zms_container_demuxer_ops;

/** 返回容器格式的可读名称；id 越界时返回 "invalid" */
ZMS_API const char *zms_container_name(zms_container_id id);

/** 注册一个 demuxer vtable（由 zms_container_register_all 内部调用） */
ZMS_API void zms_container_register_demuxer(const zms_container_demuxer_ops *ops);
/** 按容器 ID 查找 demuxer vtable；未注册返回 NULL */
ZMS_API const zms_container_demuxer_ops *zms_container_demuxer_find(zms_container_id id);

/** MPEG-TS mux 分片就绪回调（pts/dts/duration 均为毫秒） */
typedef int (*zms_container_segment_cb)(void *user, const void *data, size_t bytes, int64_t pts_ms,
                                        int64_t dts_ms, int64_t duration_ms);

/**
 * 创建 muxer 实例时传入的配置。
 * on_segment 在每个 TS 分段就绪时被调用。
 */
typedef struct zms_container_mux_opts {
    zms_container_id id;                 /**< 期望的容器格式 */
    int64_t segment_duration_ms;         /**< 目标分段时长（毫秒） */
    zms_container_segment_cb on_segment; /**< 分段就绪回调 */
    void *user;                          /**< 回调用户指针 */
} zms_container_mux_opts;

/** write_frame() flags：当前帧为关键帧 */
#define ZMS_CONTAINER_MUX_FLAG_KEYFRAME 0x8000

/**
 * muxer vtable（操作表）。
 * 当前只有 MPEG-TS 一种实现（mpegts_segment_muxer.c，基于 libhls）。
 */
typedef struct zms_container_muxer_ops {
    zms_container_id id; /**< 本 vtable 对应的容器 ID */
    const char *name;    /**< 格式名称（日志用） */
    /** 创建 muxer 实例；失败返回 NULL */
    void *(*create)(const zms_container_mux_opts *opts);
    /** 销毁实例，释放所有资源 */
    void (*destroy)(void *ctx);
    /** 设置流的 codec extradata（SPS/PPS/ASC 等），在首帧前调用 */
    void (*set_extradata)(void *ctx, int stream_type, const void *extra, size_t len);
    /** 写入一帧 ES 数据；stream_type 为 libmpeg MPEG_xxx 常量 */
    ztk_err_t (*write_frame)(void *ctx, int stream_type, const void *data, size_t len,
                             int64_t pts_ms, int64_t dts_ms, int flags);
    /** 冲刷指定流（发送 EOS 强制切段） */
    void (*flush)(void *ctx, int stream_type, int64_t pts_ms);
} zms_container_muxer_ops;

/** 注册一个 muxer vtable（由 zms_container_register_all 内部调用） */
ZMS_API void zms_container_register_muxer(const zms_container_muxer_ops *ops);
/** 按容器 ID 查找 muxer vtable；未注册返回 NULL */
ZMS_API const zms_container_muxer_ops *zms_container_muxer_find(zms_container_id id);

/**
 * 注册所有内置容器格式（demuxer + muxer）。
 * 由 zms_modules_register_all() 在进程启动时调用一次，幂等。
 * 不要在业务代码中直接调用。
 */
ZMS_API void zms_container_register_all(void);

/** 解析一条完整的 RTSP interleaved 帧（$ + channel + len + payload） */
ZMS_API ztk_err_t zms_container_rtsp_interleaved_parse(const uint8_t *data, size_t len,
                                                       zms_container_packet *out);

struct mov_reader_t;
/** 获取 MP4 demuxer ctx 内部的 mov_reader（供 VOD 索引构建使用）；非 MP4 ctx 返回 NULL */
ZMS_API struct mov_reader_t *zms_container_mp4_mov_reader(void *ctx);

/** 单条 MP4 sample 的精简信息（供 VOD FLV 索引等 sample 表遍历） */
typedef struct zms_mp4_sample_info {
    int64_t dts_ms; /**< 解码时间戳（毫秒） */
    uint32_t bytes; /**< sample 字节数 */
    int is_video;   /**< 1 = 视频轨 */
    int key;        /**< 1 = 视频关键帧（sync sample） */
} zms_mp4_sample_info;

/** sample 遍历回调；返回 <0 中止遍历 */
typedef int (*zms_mp4_sample_fn)(const zms_mp4_sample_info *s, void *user);

/**
 * 按交织 DTS 顺序遍历 MP4 demuxer ctx 的全部 sample（只读，遍历后复位游标）。
 * 仅对 ZMS_CONTAINER_MP4 的 ctx 有效。
 * @return 访问到的 sample 数；<0 表示出错。
 */
ZMS_API int zms_container_mp4_for_each_sample(void *ctx, zms_mp4_sample_fn fn, void *user);

/** 只读探测 MP4 文件总时长（毫秒）；失败返回 0 */
ZMS_API uint64_t zms_container_mp4_file_duration_ms(const char *path);

/** 获取已打开的 MKV demuxer ctx 的视频/音频 codec（open_file 成功后有效） */
ZMS_API void zms_container_demux_mkv_codecs(void *ctx, zms_codec_id *video, zms_codec_id *audio);
/** 获取已打开的 MP4 demuxer ctx 的视频/音频 codec */
ZMS_API void zms_container_demux_mp4_codecs(void *ctx, zms_codec_id *video, zms_codec_id *audio);
/** 获取已打开的 FLV 文件 demuxer ctx 的视频/音频 codec */
ZMS_API void zms_container_demux_flv_file_codecs(void *ctx, zms_codec_id *video,
                                                 zms_codec_id *audio);

/** 只读探测 FLV 文件总时长（毫秒）；失败返回 0 */
ZMS_API uint64_t zms_container_flv_file_duration_ms(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CONTAINER_CONTAINER_REGISTRY_H */
