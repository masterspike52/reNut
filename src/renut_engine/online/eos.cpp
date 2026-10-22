/**
 * @file    online/eos.cpp
 *
 * The EOS thread: EOS_Initialize and the platform, then the sign-in (EOS
 * Connect with a Device ID: EOS_Connect_CreateDeviceId once per device, then
 * EOS_Connect_Login, and EOS_Connect_CreateUser the first time the device's
 * ID signs in to this product), then for as long as reNut runs: the SDK's
 * tick, the queued sends, the received messages, and the tickers. A sign-in
 * that fails is tried again after a while; one that expires is renewed.
 *
 * P2P: one socket ("RENUT"), every connection request accepted (what a
 * message may do is up to its receiver), sends delivered once the connection
 * is up (bAllowDelayedDelivery).
 */
#include "renut_engine/online/eos.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "renut_engine/app/app_paths.h"
#include "renut_engine/online/online_log.h"

#if defined(RENUT_EOS)
#include <eos_connect.h>
#include <eos_logging.h>
#include <eos_p2p.h>
#include <eos_sdk.h>
#endif

#if !defined(RENUT_EOS_PRODUCT_ID)
#define RENUT_EOS_PRODUCT_ID ""
#define RENUT_EOS_SANDBOX_ID ""
#define RENUT_EOS_DEPLOYMENT_ID ""
#define RENUT_EOS_CLIENT_ID ""
#define RENUT_EOS_CLIENT_SECRET ""
#endif

namespace renut::online::eos {

namespace {

constexpr uint64_t kXuidBase = 0x0009000000000000ull;
constexpr uint64_t kXuidHashMask = 0x0000FFFFFFFFFFFFull;
constexpr auto kTickerInterval = std::chrono::seconds(2);
constexpr auto kSignInRetry = std::chrono::seconds(30);
constexpr size_t kMaxQueued = 4096;
constexpr uint8_t kChannelCount = 3;
constexpr size_t kMaxTickers = 8;
constexpr size_t kMaxPeerListeners = 4;

enum class State { kOff, kSigningIn, kSignedIn, kRetrying, kFailed };

std::atomic<State> g_state{State::kOff};
std::mutex g_status_mutex;
std::string g_status;  // what Status reports
std::string g_own_id;  // under g_status_mutex
std::atomic<uint64_t> g_own_xuid{0};

std::atomic<Receiver> g_receivers[kChannelCount] = {};
std::atomic<Ticker> g_tickers[kMaxTickers] = {};
std::atomic<size_t> g_ticker_count{0};
std::atomic<PeerListener> g_peer_listeners[kMaxPeerListeners] = {};
std::atomic<size_t> g_peer_listener_count{0};

void TellPeerListeners(const std::string &id, bool connected) {
  if (id.empty())
    return;
  const size_t count = std::min(g_peer_listener_count.load(), kMaxPeerListeners);
  for (size_t i = 0; i < count; ++i) {
    if (const PeerListener listener = g_peer_listeners[i])
      listener(id, connected);
  }
}

struct Outgoing {
  std::string id;
  uint8_t channel = 0;
  bool reliable = false;
  std::vector<uint8_t> data;
};
std::mutex g_send_mutex;
std::vector<Outgoing> g_sends;

std::mutex &IdsMutex() {
  static std::mutex mutex;
  return mutex;
}
std::unordered_map<uint64_t, std::string> &Ids() {
  static std::unordered_map<uint64_t, std::string> ids;
  return ids;
}

void SetStatus(State state, std::string status) {
  {
    std::lock_guard lock(g_status_mutex);
    g_status = std::move(status);
  }
  g_state = state;
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

#if defined(RENUT_EOS)

EOS_HPlatform g_platform = nullptr;
EOS_HConnect g_connect = nullptr;
EOS_HP2P g_p2p = nullptr;
EOS_ProductUserId g_local = nullptr;
EOS_P2P_SocketId g_socket = {EOS_P2P_SOCKETID_API_LATEST, "RENUT"};
std::unordered_map<std::string, EOS_ProductUserId> g_handles;  // EOS thread only
bool g_notifications_added = false;
std::chrono::steady_clock::time_point g_retry_at;

std::string IdString(EOS_ProductUserId id) {
  char buffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
  int32_t length = sizeof(buffer);
  if (!id || EOS_ProductUserId_ToString(id, buffer, &length) != EOS_EResult::EOS_Success)
    return {};
  return ParseId(buffer);
}

EOS_ProductUserId Handle(const std::string &id) {
  auto it = g_handles.find(id);
  if (it != g_handles.end())
    return it->second;
  const EOS_ProductUserId handle = EOS_ProductUserId_FromString(id.c_str());
  if (EOS_ProductUserId_IsValid(handle))
    g_handles[id] = handle;
  return EOS_ProductUserId_IsValid(handle) ? handle : nullptr;
}

void Fail(const char *what, EOS_EResult result) {
  const std::string text = std::string(what) + ": " + EOS_EResult_ToString(result);
  RONL_WARN("EOS: {}; trying again in {} s", text, kSignInRetry.count());
  g_retry_at = std::chrono::steady_clock::now() + kSignInRetry;
  SetStatus(State::kRetrying, text);
}

void EOS_CALL OnLog(const EOS_LogMessage *message) {
  if (message->Level <= EOS_ELogLevel::EOS_LOG_Error)
    RONL_ERROR("EOS [{}] {}", message->Category, message->Message);
  else
    RONL_WARN("EOS [{}] {}", message->Category, message->Message);
}

void EOS_CALL OnConnectionRequest(const EOS_P2P_OnIncomingConnectionRequestInfo *info) {
  EOS_P2P_AcceptConnectionOptions options = {};
  options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
  options.LocalUserId = g_local;
  options.RemoteUserId = info->RemoteUserId;
  options.SocketId = info->SocketId;
  EOS_P2P_AcceptConnection(g_p2p, &options);
}

void EOS_CALL OnConnectionEstablished(const EOS_P2P_OnPeerConnectionEstablishedInfo *info) {
  const std::string id = IdString(info->RemoteUserId);
  Remember(id);
  RONL_INFO("EOS: connection to {} {} ({})", id,
            info->ConnectionType == EOS_EConnectionEstablishedType::EOS_CET_Reconnection ? "back"
                                                                                          : "open",
            info->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_RelayedConnection ? "relayed"
                                                                                        : "direct");
  TellPeerListeners(id, true);
}

void EOS_CALL OnConnectionClosed(const EOS_P2P_OnRemoteConnectionClosedInfo *info) {
  RONL_INFO("EOS: connection to {} closed (reason {})", IdString(info->RemoteUserId),
            int(info->Reason));
  // Closed here (signing out, shutting down): not the other player leaving.
  if (info->Reason == EOS_EConnectionClosedReason::EOS_CCR_ClosedByLocalUser ||
      info->Reason == EOS_EConnectionClosedReason::EOS_CCR_Unknown)
    return;
  TellPeerListeners(IdString(info->RemoteUserId), false);
}

void Login();

void EOS_CALL OnAuthExpiration(const EOS_Connect_AuthExpirationCallbackInfo *) {
  // Renewed with the same Device ID: the player stays the same.
  Login();
}

void SignedInAs(EOS_ProductUserId local) {
  g_local = local;
  const std::string id = IdString(local);
  {
    std::lock_guard lock(g_status_mutex);
    g_own_id = id;
  }
  g_own_xuid = XuidOf(id);
  Remember(id);
  if (!g_notifications_added) {
    g_notifications_added = true;
    EOS_P2P_AddNotifyPeerConnectionRequestOptions request = {};
    request.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
    request.LocalUserId = g_local;
    request.SocketId = &g_socket;
    EOS_P2P_AddNotifyPeerConnectionRequest(g_p2p, &request, nullptr, &OnConnectionRequest);
    EOS_P2P_AddNotifyPeerConnectionEstablishedOptions established = {};
    established.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
    established.LocalUserId = g_local;
    established.SocketId = &g_socket;
    EOS_P2P_AddNotifyPeerConnectionEstablished(g_p2p, &established, nullptr, &OnConnectionEstablished);
    EOS_P2P_AddNotifyPeerConnectionClosedOptions closed = {};
    closed.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
    closed.LocalUserId = g_local;
    closed.SocketId = &g_socket;
    EOS_P2P_AddNotifyPeerConnectionClosed(g_p2p, &closed, nullptr, &OnConnectionClosed);
    EOS_Connect_AddNotifyAuthExpirationOptions expiration = {};
    expiration.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
    EOS_Connect_AddNotifyAuthExpiration(g_connect, &expiration, nullptr, &OnAuthExpiration);
  }
  SetStatus(State::kSignedIn, {});
  RONL_INFO("EOS: signed in, online ID {}", id);
}

void EOS_CALL OnCreateUser(const EOS_Connect_CreateUserCallbackInfo *info) {
  if (info->ResultCode == EOS_EResult::EOS_Success)
    SignedInAs(info->LocalUserId);
  else
    Fail("EOS could not make this device's player", info->ResultCode);
}

void EOS_CALL OnLogin(const EOS_Connect_LoginCallbackInfo *info) {
  if (info->ResultCode == EOS_EResult::EOS_Success) {
    SignedInAs(info->LocalUserId);
    return;
  }
  if (info->ResultCode == EOS_EResult::EOS_InvalidUser && info->ContinuanceToken) {
    // The first sign-in of this device's ID to the product: its player is made.
    EOS_Connect_CreateUserOptions options = {};
    options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
    options.ContinuanceToken = info->ContinuanceToken;
    EOS_Connect_CreateUser(g_connect, &options, nullptr, &OnCreateUser);
    return;
  }
  Fail("EOS sign-in failed", info->ResultCode);
}

void Login() {
  EOS_Connect_Credentials credentials = {};
  credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
  credentials.Token = nullptr;
  credentials.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
  EOS_Connect_UserLoginInfo user = {};
  user.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
  user.DisplayName = "reNut player";
  EOS_Connect_LoginOptions options = {};
  options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
  options.Credentials = &credentials;
  options.UserLoginInfo = &user;
  EOS_Connect_Login(g_connect, &options, nullptr, &OnLogin);
}

void EOS_CALL OnDeviceId(const EOS_Connect_CreateDeviceIdCallbackInfo *info) {
  // Made now, or made before on this device.
  if (info->ResultCode != EOS_EResult::EOS_Success &&
      info->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed) {
    Fail("EOS could not make this device's ID", info->ResultCode);
    return;
  }
  Login();
}

void BeginSignIn() {
  SetStatus(State::kSigningIn, "signing in");
  EOS_Connect_CreateDeviceIdOptions options = {};
  options.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
  options.DeviceModel = "PC";
  EOS_Connect_CreateDeviceId(g_connect, &options, nullptr, &OnDeviceId);
}

void FlushSends() {
  std::vector<Outgoing> sends;
  {
    std::lock_guard lock(g_send_mutex);
    sends.swap(g_sends);
  }
  for (const Outgoing &send : sends) {
    const EOS_ProductUserId remote = Handle(send.id);
    if (!remote)
      continue;
    EOS_P2P_SendPacketOptions options = {};
    options.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
    options.LocalUserId = g_local;
    options.RemoteUserId = remote;
    options.SocketId = &g_socket;
    options.Channel = send.channel;
    options.DataLengthBytes = uint32_t(send.data.size());
    options.Data = send.data.data();
    options.bAllowDelayedDelivery = EOS_TRUE;
    options.Reliability = send.reliable ? EOS_EPacketReliability::EOS_PR_ReliableOrdered
                                        : EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
    options.bDisableAutoAcceptConnection = EOS_FALSE;
    const EOS_EResult result = EOS_P2P_SendPacket(g_p2p, &options);
    if (result != EOS_EResult::EOS_Success) {
      static uint32_t logged = 0;
      if (logged++ < 8)
        RONL_WARN("EOS: send to {} failed: {}", send.id, EOS_EResult_ToString(result));
    }
  }
}

void ReceiveMessages() {
  uint8_t buffer[kMaxMessage];
  for (int i = 0; i < 1024; ++i) {
    EOS_P2P_ReceivePacketOptions options = {};
    options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
    options.LocalUserId = g_local;
    options.MaxDataSizeBytes = sizeof(buffer);
    options.RequestedChannel = nullptr;
    EOS_ProductUserId peer = nullptr;
    EOS_P2P_SocketId socket = {};
    uint8_t channel = 0;
    uint32_t size = 0;
    if (EOS_P2P_ReceivePacket(g_p2p, &options, &peer, &socket, &channel, buffer, &size) !=
        EOS_EResult::EOS_Success)
      return;
    const std::string id = IdString(peer);
    if (id.empty() || channel >= kChannelCount)
      continue;
    Remember(id);
    if (const Receiver receiver = g_receivers[channel])
      receiver(id, buffer, size);
  }
}

void RunTickers() {
  const size_t count = std::min(g_ticker_count.load(), kMaxTickers);
  for (size_t i = 0; i < count; ++i) {
    if (const Ticker ticker = g_tickers[i])
      ticker();
  }
}

void Run() {
  EOS_InitializeOptions init = {};
  init.ApiVersion = EOS_INITIALIZE_API_LATEST;
  init.ProductName = "reNut";
  init.ProductVersion = "1.0";
  const EOS_EResult initialized = EOS_Initialize(&init);
  if (initialized != EOS_EResult::EOS_Success &&
      initialized != EOS_EResult::EOS_AlreadyConfigured) {
    SetStatus(State::kFailed, std::string("EOS did not start: ") + EOS_EResult_ToString(initialized));
    RONL_ERROR("EOS: did not start: {}", EOS_EResult_ToString(initialized));
    return;
  }
  EOS_Logging_SetCallback(&OnLog);
  EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Warning);

  const std::string cache = (renut::paths::WritableFolder() / "eos_cache").string();
  EOS_Platform_Options options = {};
  options.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
  options.ProductId = RENUT_EOS_PRODUCT_ID;
  options.SandboxId = RENUT_EOS_SANDBOX_ID;
  options.DeploymentId = RENUT_EOS_DEPLOYMENT_ID;
  options.ClientCredentials.ClientId = RENUT_EOS_CLIENT_ID;
  options.ClientCredentials.ClientSecret = RENUT_EOS_CLIENT_SECRET;
  options.bIsServer = EOS_FALSE;
  options.Flags = EOS_PF_DISABLE_OVERLAY;
  options.CacheDirectory = cache.c_str();
  g_platform = EOS_Platform_Create(&options);
  if (!g_platform) {
    SetStatus(State::kFailed, "EOS did not start: the platform could not be made (check the "
                              "product's IDs in this build)");
    RONL_ERROR("EOS: EOS_Platform_Create failed");
    return;
  }
  g_connect = EOS_Platform_GetConnectInterface(g_platform);
  g_p2p = EOS_Platform_GetP2PInterface(g_platform);
  BeginSignIn();

  auto next_tick = std::chrono::steady_clock::now();
  for (;;) {
    EOS_Platform_Tick(g_platform);
    const auto now = std::chrono::steady_clock::now();
    if (g_state == State::kRetrying && now >= g_retry_at)
      BeginSignIn();
    if (g_state == State::kSignedIn) {
      FlushSends();
      ReceiveMessages();
      if (now >= next_tick) {
        next_tick = now + kTickerInterval;
        RunTickers();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

#endif

} // namespace

bool Start() {
  static std::once_flag once;
  std::call_once(once, [] {
#if defined(RENUT_EOS)
    if (!*RENUT_EOS_PRODUCT_ID || !*RENUT_EOS_SANDBOX_ID || !*RENUT_EOS_DEPLOYMENT_ID ||
        !*RENUT_EOS_CLIENT_ID || !*RENUT_EOS_CLIENT_SECRET) {
      SetStatus(State::kFailed, "this build has no EOS product IDs (RENUT_EOS_* in CMake)");
      RONL_ERROR("EOS: this build has no EOS product IDs");
      return;
    }
    SetStatus(State::kSigningIn, "starting");
    std::thread(Run).detach();
#else
    SetStatus(State::kFailed, "this build has no EOS SDK");
#endif
  });
  return g_state != State::kFailed;
}

bool SignedIn() {
  return g_state == State::kSignedIn;
}

std::string Status() {
  std::lock_guard lock(g_status_mutex);
  return g_state == State::kSignedIn ? std::string() : g_status;
}

std::string OwnId() {
  std::lock_guard lock(g_status_mutex);
  return SignedIn() ? g_own_id : std::string();
}

uint64_t OwnXuid() {
  return SignedIn() ? g_own_xuid.load() : 0;
}

std::string ParseId(std::string_view text) {
  std::string id;
  for (char c : text) {
    if (c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n')
      continue;
    const int digit = HexDigit(c);
    if (digit < 0)
      return {};
    id += "0123456789abcdef"[digit];
  }
  return id.size() == kIdLength ? id : std::string();
}

bool IdToBytes(std::string_view id, IdBytes &out) {
  if (id.size() != kIdLength)
    return false;
  for (size_t i = 0; i < out.size(); ++i) {
    const int hi = HexDigit(id[2 * i]), lo = HexDigit(id[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out[i] = uint8_t(hi << 4 | lo);
  }
  return true;
}

std::string IdFromBytes(const IdBytes &bytes) {
  std::string id;
  for (uint8_t b : bytes) {
    id += "0123456789abcdef"[b >> 4];
    id += "0123456789abcdef"[b & 15];
  }
  return id;
}

uint64_t XuidOf(std::string_view id) {
  if (id.empty())
    return 0;
  // FNV-1a over the lower-case ID.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (char c : id) {
    hash ^= uint8_t(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    hash *= 0x100000001B3ull;
  }
  return kXuidBase | (hash & kXuidHashMask);
}

void Remember(std::string_view id) {
  const std::string parsed = ParseId(id);
  if (parsed.empty())
    return;
  std::lock_guard lock(IdsMutex());
  Ids()[XuidOf(parsed)] = parsed;
}

std::string IdOf(uint64_t xuid) {
  std::lock_guard lock(IdsMutex());
  auto it = Ids().find(xuid);
  return it != Ids().end() ? it->second : std::string();
}

bool Send(std::string_view id, uint8_t channel, const void *data, size_t size, bool reliable) {
  if (!SignedIn() || id.empty() || size > kMaxMessage || channel >= kChannelCount)
    return false;
  Outgoing send;
  send.id = std::string(id);
  send.channel = channel;
  send.reliable = reliable;
  send.data.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + size);
  std::lock_guard lock(g_send_mutex);
  if (g_sends.size() >= kMaxQueued)
    return false;
  g_sends.push_back(std::move(send));
  return true;
}

void SetReceiver(uint8_t channel, Receiver receiver) {
  if (channel < kChannelCount)
    g_receivers[channel] = receiver;
}

void AddTicker(Ticker ticker) {
  const size_t index = g_ticker_count.fetch_add(1);
  if (index < kMaxTickers)
    g_tickers[index] = ticker;
}

void AddPeerListener(PeerListener listener) {
  const size_t index = g_peer_listener_count.fetch_add(1);
  if (index < kMaxPeerListeners)
    g_peer_listeners[index] = listener;
}

} // namespace renut::online::eos
