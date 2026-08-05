#ifndef ZMS_ENGINE_STREAM_LIMITS_H
#define ZMS_ENGINE_STREAM_LIMITS_H

/**
 * @file stream_limits.h
 * @brief stream_hub 与配置用的 app/stream/schema 名称缓冲区标准长度。
 */
#define ZMS_SCHEMA_MAX 16
#define ZMS_APP_MAX 64
#define ZMS_STREAM_MAX 128

/** 每个 SDP / payload bank / demux pipeline 的最大并发媒体轨数。 */
#define ZMS_TRACK_SLOT_MAX 4

#endif /* ZMS_ENGINE_STREAM_LIMITS_H */
