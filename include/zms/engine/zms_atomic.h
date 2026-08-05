#ifndef ZMS_ENGINE_ZMS_ATOMIC_H
#define ZMS_ENGINE_ZMS_ATOMIC_H

/**
 * @file zms_atomic.h
 * @brief 可移植的 64 位原子自增与加载原语。
 *
 * 仅暴露 stream_stats 实际需要的两种操作：
 *   ZMS_ATOMIC_ADD64(ptr, val)  fetch-and-add，返回旧值（uint64_t）
 *   ZMS_ATOMIC_LOAD64(ptr)      relaxed 加载（uint64_t）
 *
 * 内存序说明：
 *   统计计数由单入站线程写入，HTTP API 线程经 zms_media_stats_fill() 读取。
 *   读侧只需看到大致最新的值，无需完全顺序一致性。
 *   relaxed 原子加载足够，且在 x86_64 上零开销（普通 MOV）。
 *   add 在非 x86 目标上使用 acquire-release，防止编译器将其提升/下沉穿越。
 *
 * Copyright (c) zero-media-server
 */

#include <stdint.h>

#if defined(_MSC_VER) && defined(_WIN64)
/* ── MSVC x64 ─────────────────────────────────────────────────────────── */
#include <intrin.h>
#pragma intrinsic(_InterlockedExchangeAdd64)
/*
 * _InterlockedExchangeAdd64 仅 x64 可用；返回*旧*值，具完整屏障语义（x86_64 映射为 LOCK XADD）。
 */
#define ZMS_ATOMIC_ADD64(ptr, val) \
    ((uint64_t)_InterlockedExchangeAdd64((__int64 volatile *)(ptr), (__int64)(val)))
/*
 * x86_64 上对齐的 64 位 volatile 加载对程序序等价于顺序一致；不发射 fence。
 */
#define ZMS_ATOMIC_LOAD64(ptr) (*(volatile uint64_t const *)(ptr))

#elif defined(_MSC_VER)
/* ── MSVC x86（32 位）：无 64 位原子 intrinsic；使用 volatile 读。 ── */
#define ZMS_ATOMIC_ADD64(ptr, val) (*(ptr) += (uint64_t)(val), *(ptr) - (uint64_t)(val))
#define ZMS_ATOMIC_LOAD64(ptr) (*(volatile uint64_t const *)(ptr))

#elif defined(__GNUC__) || defined(__clang__)
/* ── GCC / Clang ─────────────────────────────────────────────────────── */
#define ZMS_ATOMIC_ADD64(ptr, val) \
    ((uint64_t)__atomic_fetch_add((uint64_t *)(ptr), (uint64_t)(val), __ATOMIC_ACQ_REL))
#define ZMS_ATOMIC_LOAD64(ptr)                                         \
    ({                                                                 \
        uint64_t _v;                                                   \
        __atomic_load((uint64_t const *)(ptr), &_v, __ATOMIC_RELAXED); \
        _v;                                                            \
    })

#else
/* ── 回退：非原子（单线程或未知编译器） ───────── */
#define ZMS_ATOMIC_ADD64(ptr, val) (*(ptr) += (uint64_t)(val), *(ptr) - (uint64_t)(val))
#define ZMS_ATOMIC_LOAD64(ptr) (*(ptr))
#endif

#endif /* ZMS_ENGINE_ZMS_ATOMIC_H */
