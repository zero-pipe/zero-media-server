#ifndef ZMS_EGRESS_MP4_RECORDER_H
#define ZMS_EGRESS_MP4_RECORDER_H

/**
 * @file egress_mp4_recorder.h
 * @brief 直播 MP4 云录像（对齐 ZLM enable_mp4 / on_record_mp4）。
 *
 * 目录规则（唯一）：
 *   {record.root}/{stream}/{YYYY-MM-dd}/{YYYYMMDDHHMMSS}[_n].mp4
 * - 不含 app：record 已表示录制；app 只进 hook 元数据
 * - stream：一路流；日期目录：当天切片；文件名：起录时分秒（同秒冲突加 _n）
 */
#include "zms/engine/stream/stream_hub.h"
#include "zms/ops/service/config.h"
#include "zms/zms_export.h"
#include "ztk/ztk_errno.h"

struct ztk_poller;
typedef struct ztk_poller ztk_poller;

#ifdef __cplusplus
extern "C" {
#endif

/** 从 [record] 应用 root / mp4_max_second（服务启动时调用一次）。 */
ZMS_API void zms_mp4_recorder_configure(const zms_record_config *rec);

/**
 * 若 src->enable_mp4 且尚未启动，则创建录制器并绑定 poller 定时 tick。
 * @return ZTK_OK 或已在录制；失败返回错误码。
 */
ZMS_API ztk_err_t zms_mp4_recorder_start(zms_media_source *src, ztk_poller *poller);

/** 停止录制：落盘当前文件、触发 on_record_mp4、释放资源。幂等。 */
ZMS_API void zms_mp4_recorder_stop(zms_media_source *src);

/** 校验绝对路径是否落在录像 root 下（禁止 ..）。1=安全。 */
ZMS_API int zms_mp4_recorder_path_under_root(const char *file_path);

/** 当前录像 root（configure 后有效）。 */
ZMS_API const char *zms_mp4_recorder_root(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_MP4_RECORDER_H */
