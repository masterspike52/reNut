#include "renut_engine/gpu/overlay/draw_list_split.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>

namespace renut::gpu::overlay {
namespace {

// The lists the pieces go in, reused every frame; the renderer reads them before the next Render.
std::vector<std::unique_ptr<ImDrawList>> g_pieces;

// The commands [first, last) of `list`, which draw from one vertex offset, as a list of their own holding only the
// vertices they use.
ImDrawList *Piece(const ImDrawList &list, int first, int last, size_t &used) {
  if (used == g_pieces.size())
    g_pieces.push_back(std::make_unique<ImDrawList>(list._Data));
  ImDrawList *piece = g_pieces[used++].get();
  piece->CmdBuffer.resize(0);
  piece->IdxBuffer.resize(0);
  piece->VtxBuffer.resize(0);
  piece->Flags = list.Flags;

  unsigned int offset = 0;
  unsigned int low = 0xFFFFFFFFu;
  unsigned int high = 0;
  for (int c = first; c < last; ++c) {
    const ImDrawCmd &cmd = list.CmdBuffer[c];
    if (cmd.UserCallback || cmd.ElemCount == 0)
      continue;
    offset = cmd.VtxOffset;
    for (unsigned int i = cmd.IdxOffset; i < cmd.IdxOffset + cmd.ElemCount; ++i) {
      low = std::min<unsigned int>(low, list.IdxBuffer[i]);
      high = std::max<unsigned int>(high, list.IdxBuffer[i]);
    }
  }
  if (low <= high && offset + high < unsigned(list.VtxBuffer.Size)) {
    piece->VtxBuffer.resize(int(high - low + 1));
    std::memcpy(piece->VtxBuffer.Data, list.VtxBuffer.Data + offset + low, sizeof(ImDrawVert) * (high - low + 1));
  } else {
    low = 0;
  }

  for (int c = first; c < last; ++c) {
    const ImDrawCmd &cmd = list.CmdBuffer[c];
    if (!cmd.UserCallback && (cmd.ElemCount == 0 || piece->VtxBuffer.Size == 0))
      continue;
    ImDrawCmd copy = cmd;
    copy.VtxOffset = 0;
    copy.IdxOffset = unsigned(piece->IdxBuffer.Size);
    piece->CmdBuffer.push_back(copy);
    for (unsigned int i = cmd.IdxOffset; i < cmd.IdxOffset + cmd.ElemCount; ++i)
      piece->IdxBuffer.push_back(ImDrawIdx(list.IdxBuffer[i] - low));
  }
  return piece;
}

bool UsesVertexOffsets(const ImDrawList &list) {
  for (const ImDrawCmd &cmd : list.CmdBuffer)
    if (cmd.VtxOffset != 0)
      return true;
  return false;
}

void Split(ImGuiContext &context) {
  if (context.Viewports.Size == 0)
    return;
  ImDrawData &data = context.Viewports[0]->DrawDataP;
  if (!data.Valid || std::none_of(data.CmdLists.begin(), data.CmdLists.end(),
                                  [](const ImDrawList *list) { return UsesVertexOffsets(*list); }))
    return;

  size_t used = 0;
  std::vector<ImDrawList *> lists;
  for (ImDrawList *list : data.CmdLists) {
    if (!UsesVertexOffsets(*list)) {
      lists.push_back(list);
      continue;
    }
    // A piece ends where a drawing command's vertex offset changes; callbacks stay in the piece they're in.
    int first = 0;
    bool started = false;
    unsigned int offset = 0;
    for (int c = 0; c < list->CmdBuffer.Size; ++c) {
      const ImDrawCmd &cmd = list->CmdBuffer[c];
      if (cmd.UserCallback || cmd.ElemCount == 0)
        continue;
      if (started && cmd.VtxOffset != offset) {
        lists.push_back(Piece(*list, first, c, used));
        first = c;
      }
      offset = cmd.VtxOffset;
      started = true;
    }
    lists.push_back(Piece(*list, first, list->CmdBuffer.Size, used));
  }

  data.CmdLists.resize(int(lists.size()));
  std::copy(lists.begin(), lists.end(), data.CmdLists.Data);
  data.CmdListsCount = data.CmdLists.Size;
  data.TotalVtxCount = data.TotalIdxCount = 0;
  for (const ImDrawList *list : data.CmdLists) {
    data.TotalVtxCount += list->VtxBuffer.Size;
    data.TotalIdxCount += list->IdxBuffer.Size;
  }
}

} // namespace

void SplitLargeDrawLists(ImGuiContext *context) {
  // Before NewFrame reads the flag into the frame's draw list flags.
  ImGuiContextHook frame;
  frame.Type = ImGuiContextHookType_NewFramePre;
  frame.Callback = [](ImGuiContext *ctx, ImGuiContextHook *) {
    ctx->IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  };
  ImGui::AddContextHook(context, &frame);

  ImGuiContextHook render;
  render.Type = ImGuiContextHookType_RenderPost;
  render.Callback = [](ImGuiContext *ctx, ImGuiContextHook *) { Split(*ctx); };
  ImGui::AddContextHook(context, &render);
}

} // namespace renut::gpu::overlay
