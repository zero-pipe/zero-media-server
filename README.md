# Zero Media Server

ZMS 是我用 C99 写的流媒体服务，也可以当库嵌进自己的进程里用。一路推上来的流，可以按需要用 RTMP、RTSP、HTTP-FLV、HLS、DASH 去播；SRT 和 WebRTC（WHIP/WHEP）默认不开，要用再编进去。

重点不在「协议越多越好」，而在中间那条共用的媒体路径：进来先把时间线和帧形态理顺，放进 GOP 环形缓存，出去再按各协议重封。协议层尽量薄。我更希望你能直接链 `zms`，自己管启动和退出，而不是只能起一个独立进程当黑盒。

| 依赖 | 作用 | 路径 |
|------|------|------|
| [zero-tool-kit](https://github.com/zero-pipe/zero-tool-kit) | 网络、Poller、缓冲、日志 | `3rdpart/zero-tool-kit` |
| [zero-media-kit](https://github.com/zero-pipe/zero-media-kit) | FLV / RTP / TS / MP4 / HLS / DASH 等 | `3rdpart/zero-media-kit` |

---

## 我在解决什么

做流媒体服务，真正容易出问题的往往不是少一种协议，而是：

- 一路推、多路播时，时间戳和关键帧各搞各的
- 直播缓存、点播、拉流代理、切片互相抢状态
- 想嵌进自己的程序时，只能再拉一个进程，生命周期对不上

所以 ZMS 的选择是：协议可以多，**媒体路径尽量少**。直播进 `gop_queue`，播放和 HLS/DASH 旁路都从同一条环上读；配置和 hooks 走稳定接口，协议内部细节我不保证 ABI 长期不变。

---

## 支持什么

| | |
|--|--|
| 推流 | RTMP、RTSP RECORD、SRT（MPEG-TS）、WHIP |
| 播放 | RTMP、RTSP、HTTP-FLV、HLS、DASH、SRT、WHEP |
| 点播 | 本地文件（例如 `www/vod/`），HTTP / RTSP / RTMP 等按配置走 |
| 代理 | 按需拉流（`[proxy]` 或 `addStreamProxy`） |
| 运维 | HTTP API、可选 webhook、健康检查和缓冲池统计 |
| 嵌入 | `#include "zms/sdk.h"`，create → start → run → stop → destroy |

编解码组合在 `include/zms/session/codec_filter.h` 里卡死；入口不支持的直接拒掉，避免半通不通的路径。

默认只编 RTMP / RTSP / HTTP。**SRT、WebRTC 默认关着**，机器上有 libsrt、OpenSSL 再打开。

---

## 快速开始

```bash
git clone --recursive https://github.com/zero-pipe/zero-media-server.git
cd zero-media-server
```

如果子模块没拉下来：

```bash
git submodule update --init --recursive
```

### 最小编译（只要 RTMP / RTSP / HTTP）

```bash
# Linux / macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Windows（PowerShell）
cmake -S . -B build -DZTK_ENABLE_WEPOLL=ON
cmake --build build --config Release
```

编出来的程序：

- Windows：`build\examples\Release\demo_media_server.exe`
- Linux / macOS：`build/examples/demo_media_server`

### 运行

```bash
cp conf/config.ini config.ini
# 端口按需改；本地的 config.ini 已在 gitignore 里

./build/examples/demo_media_server --config config.ini
# Windows: build\examples\Release\demo_media_server.exe --config config.ini
```

模板里默认端口：RTMP `1935`，RTSP `8554`，HTTP `8080`，SRT `9000`。WebRTC 信令走 HTTP 端口。

```bash
ffmpeg -re -stream_loop -1 -i input.mp4 -c copy -f flv rtmp://127.0.0.1:1935/live/test
ffplay http://127.0.0.1:8080/live/test.flv
```

起来之后会在终端打出本机推流、播放地址。点播文件放到 `www/vod/`（或配置里 `[record] root` 指的目录）。

---

## 嵌进自己的程序

稳定对外入口就一个：`zms/sdk.h`（里面会带上 `server.h`）。协议、GOP、egress 那些头属于进阶用法，我暂时不把它们算作稳定 API。

```c
#include "zms/sdk.h"

int main(void)
{
    zms_server *s = zms_server_create_from_ini("config.ini", NULL);
    if (!s || zms_server_start(s) != ZTK_OK)
        return 1;

    zms_server_print_endpoints(s);
    zms_server_install_default_signals(s);
    zms_server_run(s, -1); /* Ctrl+C 退出 */

    zms_server_stop(s);
    zms_server_destroy(s);
    return 0;
}
```

链接方式和现有 demo 一样：`zms` + `ztk`。更完整的最小例子见 `examples/hello_server.c`。

相关文档：

| 文档 | 内容 |
|------|------|
| [docs/integrator-guide.md](docs/integrator-guide.md) | 怎么嵌、线程和 hooks |
| [docs/api-tiers.md](docs/api-tiers.md) | Stable / Advanced / Internal 怎么分 |
| [docs/globals-and-lifecycle.md](docs/globals-and-lifecycle.md) | 进程里的全局状态和销毁顺序 |
| [docs/roadmap-sdk.md](docs/roadmap-sdk.md) | 后面打算怎么把 SDK 再收一收 |

---

## 协议和地址

推流时用的名字，要和 URL 里的 `{stream}` 对上。同一路直播名再推一次，会踢掉原来的推流端，清掉该源的 GOP，由新的接上（不会两路同时往里写）。

| 方向 | 协议 | 示例 |
|------|------|------|
| 推 | RTMP | `rtmp://host:1935/live/{stream}` |
| 推 | RTSP RECORD | `rtsp://host:8554/live/{stream}` |
| 推 | SRT | `srt://host:9000?streamid=#!::r=live/{stream},m=publish` |
| 推 | WHIP | `POST http://host:8080/index/api/whip?app=live&stream={stream}` |
| 播 | RTSP / RTMP / HTTP-FLV / HLS / DASH / SRT / WHEP | 同一个 app / stream |

| 能力 | 编译 | 配置 |
|------|------|------|
| SRT | `-DZMS_ENABLE_SRT=ON` | `[protocol] enable_srt=1`，`[srt] port=` |
| WebRTC | `-DZMS_ENABLE_WEBRTC=ON`，并链 OpenSSL | `[webrtc] enable=1`，`[general] externIP=` 填外网能访问到的 IP |

---

## 打开 SRT / WebRTC

### Windows（全开）

依赖目录大致这样摆：

| 依赖 | 路径举例 | 里面要有 |
|------|----------|----------|
| libsrt | `D:\srt` | `include\srt\srt.h`、`lib\srt.lib`、`bin\srt.dll` |
| OpenSSL | `C:\Program Files\OpenSSL-Win64` | `include\openssl\ssl.h` |
| libice | 仓库自带 | `3rdpart/zero-media-kit/libice/` |

也可以照着 `cmake/build.local.cmake.example` 写成 `cmake/build.local.cmake`（这个文件已 gitignore）：

```powershell
cmake -S . -B build `
  -DZTK_ENABLE_WEPOLL=ON `
  -DZMS_ENABLE_SRT=ON `
  -DZMS_ENABLE_WEBRTC=ON `
  -DZMS_WEBRTC_USE_LIBICE=ON `
  -DZMS_SRT_ROOT="D:/srt" `
  -DOPENSSL_ROOT_DIR="C:/Program Files/OpenSSL-Win64"

cmake --build build --config Release
Copy-Item "D:\srt\bin\srt.dll" "build\examples\Release\"
```

配成功时，CMake 输出里大概会有：

```text
-- ZMS: SRT transport enabled (libsrt found)
-- ZMS: WebRTC libice enabled
-- ZMS: WebRTC DTLS-SRTP enabled (OpenSSL)
```

缺库只会打 WARNING，对应功能不会编进去。改过选项之后，重新跑一遍 `cmake -S . -B build ...` 再编译。

### Linux / macOS

```bash
# Debian/Ubuntu: sudo apt install libsrt-openssl-dev libssl-dev
# macOS: brew install srt openssl@3

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DZMS_ENABLE_SRT=ON \
  -DZMS_ENABLE_WEBRTC=ON \
  -DZMS_WEBRTC_USE_LIBICE=ON
cmake --build build -j
```

macOS 找不到 OpenSSL 时，加上 `-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"`。

### 常用 CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `ZMS_ENABLE_SRT` | OFF | 打开 SRT（MPEG-TS），需要 libsrt |
| `ZMS_ENABLE_WEBRTC` | OFF | 打开 WHIP / WHEP |
| `ZMS_WEBRTC_USE_LIBICE` | ON | 用仓库里的 libice |
| `ZMS_SRT_ROOT` | — | SRT 安装前缀（Windows 常用） |
| `OPENSSL_ROOT_DIR` | Windows 下常能自动找到 | OpenSSL 前缀 |
| `ZTK_ENABLE_OPENSSL` | ON | TLS / WebRTC 的 DTLS |
| `ZTK_BUILD_EXAMPLES` | ON | 是否编 demo |
| `ZMS_BUILD_TESTS` | OFF | 是否编生命周期等测试 |
| `ZTK_ENABLE_WEPOLL` | ON | Windows 上的 Poller |

Windows 的 OpenSSL 可以用 [Win64 OpenSSL](https://slproweb.com/products/Win32OpenSSL.html)。

---

## 目录说明

```
include/zms/     对外头文件（嵌进业务用 sdk.h）
src/             实现：engine → egress → session → live | vod
examples/        demo_media_server、hello_server 等
docs/            嵌入说明和 API 分层
conf/            config.ini 模板
cmake/           构建脚本，以及 build.local.cmake.example
3rdpart/         zero-tool-kit、zero-media-kit
```

`demo_media_server` 是完整服务进程；`hello_server` 是最小嵌入例子；另外还有 `demo_rtsp_player`、`demo_proxy_pull`。

子模块更新之后重新编译：

```bash
git submodule update --remote 3rdpart/zero-tool-kit
git submodule update --remote 3rdpart/zero-media-kit
cmake --build build --config Release   # 或 cmake --build build -j
```

---

## 许可证

以仓库里的 LICENSE 为准。协议和容器不少逻辑在 zero-media-kit / zero-tool-kit 里；改媒体行为前，先分清是 ZMS 这一层，还是 kit 那一层。
