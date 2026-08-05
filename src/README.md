# ZMS 源码目录

主路径：`engine → egress → session → live|vod`。

支撑：`media/`（codec、容器）、`ops/`（api、配置）、`util/`、`webrtc/`。

| 改什么 | 目录 |
|--------|------|
| GOP | `engine/gop/` |
| 出站泵 / pacing | `egress/` |
| RTP mux / pump | `egress/rtp/` |
| 协议信令 | `session/` |
| HTTP-FLV / HLS 会话 | `live/play/` |
| 点播读文件 | `vod/io/` |
| 点播输出 | `vod/play/` |
| codec / 容器 | `media/` |
| WebAPI / 配置 | `ops/` |
