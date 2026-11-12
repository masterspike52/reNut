#pragma once

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>

REXLOG_CATEGORY(renut_hooks, "reNut")
REXLOG_TABLE(kRenutLog, renut_hooks)

#define RNUT_TRACE(...) REXLOG_CAT_TRACE(::rex::log::renut_hooks, __VA_ARGS__)
#define RNUT_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::renut_hooks, __VA_ARGS__)
#define RNUT_INFO(...)  REXLOG_CAT_INFO(::rex::log::renut_hooks, __VA_ARGS__)
#define RNUT_WARN(...)  REXLOG_CAT_WARN(::rex::log::renut_hooks, __VA_ARGS__)
#define RNUT_ERROR(...) REXLOG_CAT_ERROR(::rex::log::renut_hooks, __VA_ARGS__)
