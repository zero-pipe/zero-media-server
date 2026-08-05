#ifndef ZMS_HAVE_TLS_H
#define ZMS_HAVE_TLS_H

/** 与 ztk 一致：仅当 CMake 找到 OpenSSL 且 ZTK_ENABLE_OPENSSL=ON 时为 1 */
#if defined(ZTK_HAVE_OPENSSL) && ZTK_HAVE_OPENSSL
#define ZMS_HAVE_PULL_TLS 1
#else
#define ZMS_HAVE_PULL_TLS 0
#endif

#endif /* ZMS_HAVE_TLS_H */
