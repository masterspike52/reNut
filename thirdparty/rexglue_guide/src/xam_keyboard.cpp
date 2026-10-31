// XamShowKeyboardUI: the title's text entry on the Guide's on-screen keyboard
// (keyboard.h), in place of the rexglue runtime's text box.

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include "guest.h"
#include "keyboard.h"

namespace {

using namespace rexglue_guide::guest;
// The X_ERROR_* macros use it unqualified.
using rex::X_RESULT;

// VKBD_LATIN_PASSWORD: show * for each character.
constexpr uint32_t kPasswordFlag = 0x00000080;
// XN_SYS_UI: system UI is showing (1) or gone (0).
constexpr uint32_t kNotifySystemUi = 0x00000009;

// Guest strings are big-endian UTF-16, zero-terminated.
std::u16string ReadString(uint32_t address) {
  std::u16string s;
  const uint8_t* p = At(address);
  if (!p) return s;
  for (size_t i = 0; i < 4096; ++i) {
    const char16_t c = char16_t(p[i * 2] << 8 | p[i * 2 + 1]);
    if (!c) break;
    s.push_back(c);
  }
  return s;
}

void WriteString(uint32_t address, uint32_t capacity, const std::u16string& s) {
  uint8_t* p = At(address);
  if (!p || !capacity) return;
  const size_t n = std::min<size_t>(s.size(), capacity - 1);
  for (size_t i = 0; i < n; ++i) Store16(p + i * 2, uint16_t(s[i]));
  Store16(p + n * 2, 0);
}

}  // namespace

// XamShowKeyboardUI(user, flags, default_text, title, description, buffer,
// buffer_length, overlapped). Done or cancelled, it succeeds; cancelled has
// the extended error ERROR_CANCELLED and leaves the buffer alone, as the
// runtime's does.
extern "C" REX_FUNC(__imp__XamShowKeyboardUI) {
  const uint32_t flags = ctx.r4.u32;
  const uint32_t buffer = ctx.r8.u32;
  const uint32_t buffer_length = ctx.r9.u32;
  const uint32_t overlapped = ctx.r10.u32;
  if (!buffer || !buffer_length) return Return(ctx, X_ERROR_INVALID_PARAMETER);

  auto answer = std::make_shared<std::promise<std::optional<std::string>>>();
  std::shared_future<std::optional<std::string>> result = answer->get_future().share();
  rexglue_guide::KeyboardRequest request;
  request.text = rexglue_guide::Utf16ToUtf8(ReadString(ctx.r5.u32));
  request.title = rexglue_guide::Utf16ToUtf8(ReadString(ctx.r6.u32));
  request.description = rexglue_guide::Utf16ToUtf8(ReadString(ctx.r7.u32));
  request.masked = (flags & kPasswordFlag) != 0;
  request.max_length = buffer_length - 1;
  request.done = [answer](std::optional<std::string> text) { answer->set_value(std::move(text)); };
  // Without the Guide (not installed), the runtime's own.
  if (!rexglue_guide::ShowTitleKeyboard(std::move(request))) {
    return CallOriginal("__imp__XamShowKeyboardUI", ctx, base);
  }
  REXKRNL_INFO("Rexglue Guide: XamShowKeyboardUI (flags {:08X}, up to {} characters)", flags, buffer_length - 1);

  auto run = [buffer, buffer_length, result](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    const std::optional<std::string> text = result.get();
    length = 0;
    if (!text) {
      extended_error = X_ERROR_CANCELLED;
      return X_ERROR_SUCCESS;
    }
    WriteString(buffer, buffer_length, rexglue_guide::Utf8ToUtf16(*text));
    extended_error = X_ERROR_SUCCESS;
    return X_ERROR_SUCCESS;
  };
  // System UI up while it's shown; gone a little after, for titles that only
  // start listening once the overlapped completes.
  auto pre = [] { REX_KERNEL_STATE()->BroadcastNotification(kNotifySystemUi, 1); };
  auto post = [] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REX_KERNEL_STATE()->BroadcastNotification(kNotifySystemUi, 0);
  };
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped, pre, post);
    return Return(ctx, X_ERROR_IO_PENDING);
  }
  pre();
  uint32_t extended_error = 0, length = 0;
  const X_RESULT status = run(extended_error, length);
  post();
  Return(ctx, status);
}
