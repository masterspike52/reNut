// renut - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.

#include "generated/renut_config.h"
#include "generated/renut_init.h"
#include <rex/ppc/types.h>
#include "renut_app.h"
#include <rex/logging.h>



namespace renut::log {
  inline const rex::LogCategoryId Input = rex::RegisterLogCategory("renut");
}

#define RENUT_TRACE(...) REXLOG_CAT_TRACE(::renut::log::Input, __VA_ARGS__)
#define RENUT_DEBUG(...) REXLOG_CAT_DEBUG(::renut::log::Input, __VA_ARGS__)
#define RENUT_INFO(...)  REXLOG_CAT_INFO(::renut::log::Input, __VA_ARGS__)
#define RENUT_WARN(...)  REXLOG_CAT_WARN(::renut::log::Input, __VA_ARGS__)
#define RENUT_ERROR(...) REXLOG_CAT_ERROR(::renut::log::Input, __VA_ARGS__)

REX_DEFINE_APP(renut, RenutApp::Create)


void renutdebugprint(ppc_pchar_t loglevel, ppc_pchar_t message, ppc_u32_t unk) {
    // For now, just print the message. You can use loglevel and unk as needed.
    RENUT_DEBUG("renutdebugprint: {} (loglevel: {}, unk: {})", message.value(), loglevel.value(), unk);
}

PPC_HOOK(sub_82C3C7F0, renutdebugprint)