#pragma once

#include <cstdint>

namespace renut::shader_dump {

// Resolves a SetVertexShader (0x8222A0A8) handle argument back to the ucode
// hash captured when that handle was created (CreateVertexShader,
// 0x8264E8B0/0x8264E998) -- see shader_dump.cpp's g_shaderHandleToUcodeHash
// for the full story. Returns false if the handle was never seen created
// (e.g. it bypassed CreateVertexShader entirely via IM_LOAD -- this is
// exactly the signal docs/ai/archive/native-renderer-rewrite-plan.md's
// Phase 0 needs to measure). Always available regardless of the
// renut_dump_vertex_decl cvar: the underlying capture is unconditional
// (cheap hash+map-insert, no I/O), only file dumping is cvar-gated.
bool TryResolveShaderUcodeHash(uint32_t handle, uint64_t& outHash);

}
