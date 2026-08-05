#ifndef ZMS_EXPORT_H
#define ZMS_EXPORT_H

#if defined(ZMS_BUILD_SHARED)
#if defined(_WIN32) && defined(ZMS_EXPORTS)
#define ZMS_API __declspec(dllexport)
#elif defined(_WIN32)
#define ZMS_API __declspec(dllimport)
#elif defined(__GNUC__)
#define ZMS_API __attribute__((visibility("default")))
#else
#define ZMS_API
#endif
#else
#define ZMS_API
#endif

#endif /* ZMS_EXPORT_H */
