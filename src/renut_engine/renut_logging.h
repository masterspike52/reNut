#include <rex/logging.h>
#include <rex/ppc/types.h>
#include "generated/renut_config.h"
#include "generated/renut_init.h"




namespace renut::log {
    inline const rex::LogCategoryId Input = rex::RegisterLogCategory("renut_hooks");
}

#define RNUT_TRACE(...) REXLOG_CAT_TRACE(::renut::log::Input, __VA_ARGS__)
#define RNUT_DEBUG(...) REXLOG_CAT_DEBUG(::renut::log::Input, __VA_ARGS__)
#define RNUT_INFO(...)  REXLOG_CAT_INFO(::renut::log::Input, __VA_ARGS__)
#define RNUT_WARN(...)  REXLOG_CAT_WARN(::renut::log::Input, __VA_ARGS__)
#define RNUT_ERROR(...) REXLOG_CAT_ERROR(::renut::log::Input, __VA_ARGS__)


