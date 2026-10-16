/**
 * @file    gpu/render/vertex_format.h
 * @brief   Xbox 360 vertex declarations and how their elements reach the host
 *          input assembler.
 *
 * N&B's D3D patches each vertex shader's vfetch formats from the bound
 * declaration at draw time; the recompiled shaders instead take every element
 * as float4 from the input layout. So each element is laid out on the host in
 * a format whose input-assembler result equals what the patched Xenos fetch
 * would produce: the guest bytes pass through (endian-swapped) when a host
 * format decodes them identically, otherwise they are expanded to floats.
 */
#pragma once

#include <cstdint>
#include <vector>

#include <plume_render_interface.h>

#include <rex/types.h>

namespace renut::gpu {

// D3DVERTEXELEMENT9 as the guest stores it.
struct GuestVertexElement {
  be_u16 Stream;
  be_u16 Offset;
  be_u32 Type;   // D3DDECLTYPE (encodes the Xenos vertex format, see DeclType)
  u8 Method;
  u8 Usage;      // D3DDECLUSAGE
  u8 UsageIndex;
  u8 pad;
};
static_assert(sizeof(GuestVertexElement) == 12);

constexpr u32 kDeclTypeUnused = 0xFFFFFFFFu;   // D3DDECL_END's Type
constexpr u32 kMaxVertexStreams = 16;
// Input slot bound to a zero-filled buffer with stride 0, feeding every shader
// input the declaration does not provide.
constexpr u32 kZeroStreamSlot = kMaxVertexStreams;
constexpr u32 kZeroStreamSize = 16;

// Fields of an Xbox 360 D3DDECLTYPE, e.g. FLOAT3 = 0x2A23B9:
// bits 0-5 Xenos vertex format, bits 6-7 the data's endian (1 8in16, 2 8in32),
// bit 8 signed, bit 9 integer (else normalised), bits 10-21 four 3-bit fetch
// swizzles (0-3 XYZW, 4 = 0, 5 = 1).
struct DeclType {
  u32 format = 0;
  bool is_signed = false;
  bool is_integer = false;
  u8 swizzle[4] = {0, 1, 2, 3};

  // Decodes the type as bound (BoundDeclType).
  static DeclType Decode(u32 type);
};

// The type with the swizzle D3D binds it with. Elements whose data is 8in16
// (FLOAT16_n, SHORTn, ...) are fetched with the stream's 8in32 swap, which
// exchanges each pair of 16-bit components, so D3D swaps x/y and z/w in their
// swizzle (PatchVertexShaderToMatchVertexDeclaration).
u32 BoundDeclType(u32 type);

// How one element is written into the host vertex buffer. Every Xenos vertex
// format is built from 32-bit words whose first component sits in the low bits
// once the fetch constant's endian swap is applied, which is also how a
// little-endian host format reads a word.
enum class ElementConversion : u8 {
  Copy32,       // each word endian-swapped and stored little-endian
  ExpandFloat,  // decoded (and swizzled) to four 32-bit floats on the CPU
};

struct HostVertexElement {
  u32 guest_offset = 0;   // within the guest vertex
  u32 host_offset = 0;    // within the host vertex
  u32 host_size = 0;
  plume::RenderFormat host_format = plume::RenderFormat::UNKNOWN;
  ElementConversion conversion = ElementConversion::Copy32;
  // Copy32 of 16-bit components whose bound swizzle exchanges each pair: the
  // halves of every word are swapped back instead.
  bool swap_halves = false;
  DeclType type;
  u32 components = 0;     // host components written (ExpandFloat)
  u8 usage = 0;
  u8 usage_index = 0;
  u32 location = 0;       // SPIR-V input location (RENUT_VERTEX_INPUT_LOCATIONS)
};

struct HostVertexStream {
  bool used = false;
  u32 host_stride = 0;
  std::vector<HostVertexElement> elements;
};

// Host layout for a whole declaration. Elements whose usage has no host
// input location are dropped (no N&B shader reads them).
struct HostVertexLayout {
  HostVertexStream streams[kMaxVertexStreams];
  std::vector<plume::RenderInputElement> input_elements;  // semantic names are static strings
};

HostVertexLayout BuildHostVertexLayout(const GuestVertexElement *elements, u32 count);

// D3DDECLTYPE bits for a vertex fetch of 'format' (GPUVERTEXFORMAT) reading its
// components in order (the shader applies its own swizzle), or kDeclTypeUnused
// for a format the layout builder does not handle.
u32 FetchDeclType(u32 format, bool is_signed, bool is_integer);

// Converts 'vertex_count' vertices of one stream from guest bytes (big-endian,
// guest stride) into the host layout. 'endian' is the Xenos swap mode from the
// stream's vertex fetch constant (usually 2 = 8in32).
void ConvertVertices(const HostVertexStream &stream, const u8 *guest, u32 guest_stride,
                     u32 vertex_count, u32 endian, u8 *host);

const char *UsageSemantic(u8 usage);

} // namespace renut::gpu
