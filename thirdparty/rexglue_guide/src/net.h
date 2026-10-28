#pragma once
// Sockets and XNet for titles (net.cpp). Exposed to the rest of the Guide for
// LIVE sessions, the relay and the title server.

#include <cstdint>
#include <functional>
#include <string>

namespace rexglue_guide::net {

// XNADDR as titles see it (36 bytes in guest memory). Addresses are in
// network byte order as stored; port_online is a host-order number.
struct XnAddr {
  uint32_t ina = 0;
  uint32_t ina_online = 0;
  uint16_t port_online = 0;
  uint8_t enet[6] = {};
  uint8_t online[20] = {};

  void Read(const uint8_t* guest);
  void Write(uint8_t* guest) const;
  bool SameMachine(const XnAddr& o) const;  // same instance, by enet + port
  uint64_t Xuid() const;                    // RexLiveless account carried in online[], or 0
};

XnAddr LocalXnAddr();

// Port offset of this instance (System Link ports + offset). Automatic slot
// 0, 10, ... 70 so copies on one PC don't collide.
uint16_t PortOffset();

}  // namespace rexglue_guide::net
