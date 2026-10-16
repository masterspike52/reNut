#pragma once

#include <rex/logging.h>

namespace renut::online::log {
inline const rex::LogCategoryId Online = rex::RegisterLogCategory("renut_online");
}

#define RONL_DEBUG(...) REXLOG_CAT_DEBUG(::renut::online::log::Online, __VA_ARGS__)
#define RONL_INFO(...)  REXLOG_CAT_INFO(::renut::online::log::Online, __VA_ARGS__)
#define RONL_WARN(...)  REXLOG_CAT_WARN(::renut::online::log::Online, __VA_ARGS__)
#define RONL_ERROR(...) REXLOG_CAT_ERROR(::renut::online::log::Online, __VA_ARGS__)
