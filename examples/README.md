# 示例程序

在 `ZTK_BUILD_EXAMPLES=ON` 且平台具备 Poller 后端（Linux epoll / Windows wepoll）时随主工程一并构建。

| 可执行文件 | 源码 | 说明 |
|------------|------|------|
| `demo_media_server` | [demo_media_server.c](demo_media_server.c) | 综合媒体服务：RTMP 推流；RTSP / HTTP-FLV / HLS / DASH 播放；点播；HTTP API / WebHook |
| `demo_rtsp_server` | [demo_rtsp_server.c](demo_rtsp_server.c) | 独立 RTSP 服务（RECORD 推流 + PLAY 播放） |
| `demo_rtmp_server` | [demo_rtmp_server.c](demo_rtmp_server.c) | 独立 RTMP 服务（publish + play） |
| `demo_rtsp_player` | [demo_rtsp_player.c](demo_rtsp_player.c) | RTSP 客户端 PLAY（TCP 交织） |
| `demo_proxy_pull` | [demo_proxy_pull.c](demo_proxy_pull.c) | 从远端 RTSP/RTMP 拉流并本地重推（独立进程，不挂接 media_server） |

`demo_media_server`、`demo_rtsp_server`、`demo_rtmp_server` 共用 [demo_server_runtime.c](demo_server_runtime.c) 做配置加载与运行时初始化，服务生命周期统一为：**runtime init → service create → service start → wait → service stop/destroy → runtime fini**。

---

## demo_media_server

主示例：启动 RTMP / RTSP / HTTP 监听，加载 `config.ini` 后对外提供直播与点播。

先在仓库根目录准备配置（可复制 `conf/config.ini`）：

```bash
cp conf/config.ini config.ini
```

运行：

```bash
# Linux
./build/examples/demo_media_server --config config.ini

# Windows
build\examples\Release\demo_media_server.exe --config config.ini
```

可选：`--log /path/to/log.txt` 同时写 stderr 与日志文件。

推流 `rtmp://host:1935/live/{stream}` 或 RTSP RECORD 后，可用 RTSP / HTTP-FLV / HLS / DASH 等 URL 播放（端口以配置为准）。启动日志会按当前流的编解码能力打印可用 URL（不支持的协议不会列出）。

---

## demo_rtsp_server / demo_rtmp_server

单协议最小服务，结构与 `zms_refactor/examples` 下的 `rtsp_tcp_server_example.c`、`rtmp_server_example.c` 对齐：

```bash
# RTSP（默认读 config.ini 中 [rtsp] port）
./build/examples/demo_rtsp_server --config config.ini

# RTMP
./build/examples/demo_rtmp_server --config config.ini --port 1935

# 定时退出（测试用）
./build/examples/demo_rtsp_server --seconds 30
```

---

## demo_rtsp_player

最小 RTSP 拉流客户端，用于验证服务端 RTSP PLAY：

```bash
# Linux
./build/examples/demo_rtsp_player rtsp://127.0.0.1:554/live/{stream}

# Windows
build\examples\Release\demo_rtsp_player.exe rtsp://127.0.0.1:554/live/{stream}
```

---

## demo_proxy_pull

独立验证 **拉流客户端**（`proxy_player` 路径），进程内拉远端流并本地重推，**不会**注册到 `demo_media_server` 的流列表。

在 `conf/config.ini` 的 `[proxy]` 节配置 `proxy_pull=...`，或修改源码中的默认 URL，然后：

```bash
# Linux
./build/examples/demo_proxy_pull

# Windows
build\examples\Release\demo_proxy_pull.exe
```

若要与主服务器同进程做拉流代理，请在 `demo_media_server` 的 `config.ini` 里配置 `[proxy] proxy_pull=...`，或使用 HTTP API `/index/api/addStreamProxy`。

---

## ZTK 网络层示例

更底层的 TCP/UDP/Poller 示例在 `3rdpart/zero-tool-kit/examples/`。
