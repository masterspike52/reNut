// RexFabric's log category ("rexfabric").

#pragma once

#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>

REXLOG_CATEGORY(rexfabric, "RexFabric mods")
REXLOG_TABLE(kRexFabricLog, rexfabric)

#define RF_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::rexfabric, __VA_ARGS__)
#define RF_INFO(...) REXLOG_CAT_INFO(::rex::log::rexfabric, __VA_ARGS__)
#define RF_WARN(...) REXLOG_CAT_WARN(::rex::log::rexfabric, __VA_ARGS__)
#define RF_ERROR(...) REXLOG_CAT_ERROR(::rex::log::rexfabric, __VA_ARGS__)
