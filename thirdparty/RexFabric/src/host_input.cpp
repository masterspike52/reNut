#include <rexfabric/host_input.h>

namespace rexfabric {
namespace {

// Installed once during setup and only read after, so readers take no lock.
HostInput& Storage() {
  static HostInput input;
  return input;
}

}  // namespace

void SetHostInput(HostInput input) { Storage() = std::move(input); }
const HostInput& GetHostInput() { return Storage(); }
bool HasHostInput() { return static_cast<bool>(Storage().key_down); }

}  // namespace rexfabric
