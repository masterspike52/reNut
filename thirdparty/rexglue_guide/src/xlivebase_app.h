#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    2026 - Message coverage ported from xenia-canary netplay
 *              (src/xenia/kernel/xam/apps/xlivebase_app.h/.cc).
 */

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

// ---------------------------------------------------------------------------
// XLive online error/status codes (from netplay xnet.h)
// ---------------------------------------------------------------------------
#define X_ONLINE_E_LOGON_NOT_LOGGED_ON static_cast<X_HRESULT>(0x80151802L)
#define X_ONLINE_E_LOGON_NO_NETWORK_CONNECTION static_cast<X_HRESULT>(0x80151000L)
#define X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED static_cast<X_HRESULT>(0x001510F0L)
#define X_ONLINE_S_LOGON_DISCONNECTED static_cast<X_HRESULT>(0x001510F1L)
#define X_ONLINE_E_STORAGE_FILE_NOT_FOUND static_cast<X_HRESULT>(0x8015C004L)
#define X_ONLINE_E_STORAGE_INVALID_STORAGE_PATH static_cast<X_HRESULT>(0x8015C008L)
#define X_ONLINE_E_STORAGE_INVALID_FACILITY static_cast<X_HRESULT>(0x8015C009L)

#define X_ONLINE_MAX_FRIENDS 100
#define X_MAX_RICHPRESENCE_SIZE 64

// X_ONLINE_FRIEND::state bits. The netplay backend's StateFlag enum uses the
// same values for ONLINE/PLAYING/VOICE/JOINABLE, so a player's presence state
// can be copied straight across without translation.
#define X_ONLINE_FRIENDSTATE_FLAG_NONE 0x00000000
#define X_ONLINE_FRIENDSTATE_FLAG_ONLINE 0x00000001
#define X_ONLINE_FRIENDSTATE_FLAG_PLAYING 0x00000002
#define X_ONLINE_FRIENDSTATE_FLAG_VOICE 0x00000008
#define X_ONLINE_FRIENDSTATE_FLAG_JOINABLE 0x00000010
#define X_ONLINE_FRIENDSTATE_FLAG_JOINABLE_FRIENDS_ONLY 0x00000100
#define X_ONLINE_FRIENDSTATE_ENUM_ONLINE 0x00000000
#define X_ONLINE_FRIENDSTATE_ENUM_CONSOLE_XBOX360 0x00001000

enum X_NAT_TYPE : uint32_t { NAT_OPEN = 1, NAT_MODERATE = 2, NAT_STRICT = 3 };

#pragma pack(push, 8)

struct X_ONLINE_SERVICE_INFO {
  rex::be<uint32_t> id;
  uint32_t ip;  // in_addr, network byte order
  rex::be<uint16_t> port;
  rex::be<uint16_t> reserved;
};
static_assert_size(X_ONLINE_SERVICE_INFO, 0xC);

// Marshaled argument entry used by title-side XLive code for async XLiveBase
// tasks (from netplay xnet.h).
struct X_ARGUMENT_ENTRY {
  rex::be<uint32_t> native_size;
  rex::be<uint64_t> argument_value_ptr;
};
static_assert_size(X_ARGUMENT_ENTRY, 0x10);

struct X_ARGUMENT_LIST {
  X_ARGUMENT_ENTRY entry[32];
  rex::be<uint32_t> argument_count;
};
static_assert_size(X_ARGUMENT_LIST, 0x208);

struct X_CREATE_FRIENDS_ENUMERATOR {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY friends_starting_index;
  X_ARGUMENT_ENTRY friends_amount;
  X_ARGUMENT_ENTRY buffer_ptr;
  X_ARGUMENT_ENTRY handle_ptr;
};

struct X_PRESENCE_INITIALIZE {
  X_ARGUMENT_ENTRY max_peer_subscriptions;
};

struct XLIVEBASE_UPDATE_ACCESS_TIMES {
  rex::be<uint32_t> user_index;
  rex::be<uint32_t> title_id;
  rex::be<uint32_t> content_categories;
};

struct X_STORAGE_BUILD_SERVER_PATH {
  rex::be<uint32_t> user_index;
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> storage_location;
  rex::be<uint32_t> storage_location_info_ptr;
  rex::be<uint32_t> storage_location_info_size;
  rex::be<uint32_t> file_name_ptr;
  rex::be<uint32_t> server_path_ptr;
  rex::be<uint32_t> server_path_length_ptr;
};
static_assert_size(X_STORAGE_BUILD_SERVER_PATH, 0x28);

// XLive async task structures (title-side XLive marshaling, netplay xnet.h).
struct BASE_ENDIAN_BUFFER {
  rex::be<uint32_t> buffer_ptr;
  rex::be<uint32_t> buffer_size;
  rex::be<uint32_t> unk_08;
  rex::be<uint32_t> unk_0C;
  rex::be<uint32_t> unk_10;
};
static_assert_size(BASE_ENDIAN_BUFFER, 0x14);

struct XLIVE_ASYNC_TASK {
  rex::be<uint32_t> ordinal;
  rex::be<uint32_t> schema_data_ptr;  // SCHEMA_DATA*
  rex::be<uint32_t> schema_index;
  rex::be<uint32_t> task_flags;
  rex::be<uint32_t> live_async_task_internal_ptr;
  rex::be<uint32_t> internal_task_size;
  rex::be<uint32_t> marshalled_request_ptr;
  rex::be<uint32_t> marshalled_request_size;
  rex::be<uint32_t> total_wire_buffe_size;
  rex::be<uint32_t> counter;
  rex::be<uint32_t> logon_id;
  rex::be<uint32_t> results_ptr;
  rex::be<uint32_t> results_size;
  BASE_ENDIAN_BUFFER wire_buffer;
  rex::be<uint32_t> overlapped_ptr;
};
static_assert_size(XLIVE_ASYNC_TASK, 0x4C);

struct XLIVEBASE_ASYNC_MESSAGE {
  rex::be<uint32_t> xlive_async_task_ptr;
  rex::be<uint64_t> current_numerator;
  rex::be<uint64_t> current_denominator;
  rex::be<uint64_t> last_numerator;
  rex::be<uint64_t> last_denominator;
};
static_assert_size(XLIVEBASE_ASYNC_MESSAGE, 0x28);

#pragma pack(push, 4)
struct X_ONLINE_FRIEND {
  rex::be<uint64_t> xuid;
  char Gamertag[16];
  rex::be<uint32_t> state;
  uint8_t session_id[8];
  rex::be<uint32_t> title_id;
  rex::be<uint64_t> ftUserTime;
  uint8_t xnkidInvite[8];
  rex::be<uint64_t> gameinviteTime;
  rex::be<uint32_t> cchRichPresence;
  rex::be<char16_t> wszRichPresence[X_MAX_RICHPRESENCE_SIZE];
};
static_assert_size(X_ONLINE_FRIEND, 0xC4);
#pragma pack(pop)

#pragma pack(pop)

class XLiveBaseApp : public system::xam::App {
 public:
  explicit XLiveBaseApp(system::KernelState* kernel_state);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  X_HRESULT XOnlineGetServiceInfo(uint32_t service_id, uint32_t service_info);
  X_HRESULT XFriendsCreateEnumerator(uint32_t buffer_ptr,
                                     uint32_t buffer_length);
  X_HRESULT XStorageBuildServerPath(uint32_t buffer_ptr);
  X_HRESULT XStringVerify(uint32_t buffer_ptr);
  X_HRESULT ZeroAsyncTaskResults(uint32_t buffer_ptr);

  // Reads argument N of a marshaled X_ARGUMENT_LIST as a 32-bit value.
  uint32_t ReadArgument32(const X_ARGUMENT_ENTRY& entry);
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
