# 3rdpart


| 子模块                      | 路径                       | 仓库                                                                      |
| ------------------------ | ------------------------ | ----------------------------------------------------------------------- |
| **zero-tool-kit** (ZTK)  | `3rdpart/zero-tool-kit`  | [zero-pipe/zero-tool-kit](https://github.com/zero-pipe/zero-tool-kit)   |
| **zero-media-kit** (ZMK) | `3rdpart/zero-media-kit` | [zero-pipe/zero-media-kit](https://github.com/zero-pipe/zero-media-kit) |


命名说明：

- **zero-media-server** — 本仓库 GitHub 名（ZMS 流媒体服务器）
- **zero-tool-kit** — 网络库（ZTK）
- **zero-media-kit** — 编解码/容器库（ZMK）

## 首次拉取

```bash
git submodule update --init --recursive
```

## CMake

```cmake
add_subdirectory(3rdpart/zero-tool-kit)
set(ZMS_KIT "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zero-media-kit")
add_subdirectory(${ZMS_KIT}/libmpeg)
# 以及 libflv, libhls, librtp, librtsp, librtmp, libmov。
```
