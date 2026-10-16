/**
 * @file    gpu/log.h
 * @brief   Log category for the native renderer.
 */
#pragma once

#include <rex/logging.h>

namespace renut::gpu::log {
inline const rex::LogCategoryId Gpu = rex::RegisterLogCategory("renut_gpu");
}

#define RGPU_TRACE(...) REXLOG_CAT_TRACE(::renut::gpu::log::Gpu, __VA_ARGS__)
#define RGPU_DEBUG(...) REXLOG_CAT_DEBUG(::renut::gpu::log::Gpu, __VA_ARGS__)
#define RGPU_INFO(...) REXLOG_CAT_INFO(::renut::gpu::log::Gpu, __VA_ARGS__)
#define RGPU_WARN(...) REXLOG_CAT_WARN(::renut::gpu::log::Gpu, __VA_ARGS__)
#define RGPU_ERROR(...) REXLOG_CAT_ERROR(::renut::gpu::log::Gpu, __VA_ARGS__)
