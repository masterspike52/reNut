#pragma once
// The Guide's log category; Configure adds it to the logger.

#include <rex/logging/log_category.h>

REXLOG_CATEGORY(guide, "Rexglue Guide and RexLiveless")
REXLOG_TABLE(kGuideLog, guide)

#define RG_INFO(...) REXLOG_CAT_INFO(::rex::log::guide, __VA_ARGS__)
#define RG_WARN(...) REXLOG_CAT_WARN(::rex::log::guide, __VA_ARGS__)
#define RG_ERROR(...) REXLOG_CAT_ERROR(::rex::log::guide, __VA_ARGS__)
