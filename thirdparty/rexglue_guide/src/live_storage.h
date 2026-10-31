#pragma once
// Xbox LIVE storage (XStorage*), backed by RexLiveless (live_storage.cpp). The
// upload, download and enumerate calls are public (guide.h).

#include <cstdint>

namespace rexglue_guide::storage {

// XStorageBuildServerPath and XStorageBuildServerPathByXuid (XLiveBase
// 0x58035, in-process; arg1 points at their arguments). Returns an HRESULT.
uint32_t BuildServerPath(uint32_t args_ptr);

}  // namespace rexglue_guide::storage
