#include <rexfabric/midasm.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rexfabric {
namespace {

struct Claim {
  uint64_t id;
  MidasmFn fn;
  void* user;
  const RexFabricMod* owner;
};

// Sites are never erased: a MidasmSite in a stub's static holds `claimed` for the life of the process.
struct Site {
  std::atomic<bool> claimed{false};
  std::vector<Claim> claims;
};

struct Registry {
  // Recursive: a handler may claim or release a site from inside its own call.
  std::recursive_mutex mutex;
  std::unordered_map<uint32_t, std::unique_ptr<Site>> sites;
  uint64_t next_id = 1;
};

Registry& Get() {
  static Registry registry;
  return registry;
}

Site& Intern(Registry& registry, uint32_t address) {
  auto& site = registry.sites[address];
  if (!site) site = std::make_unique<Site>();
  return *site;
}

void Sync(Site& site) { site.claimed.store(!site.claims.empty(), std::memory_order_release); }

}  // namespace

MidasmSite::MidasmSite(uint32_t address) : address_(address) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  claimed_ = &Intern(registry, address).claimed;
}

bool MidasmSite::Call(std::initializer_list<MidasmReg> regs) const {
  if (!claimed_->load(std::memory_order_relaxed)) return false;
  // A copy, so handlers run without the registry lock and may claim or release freely.
  std::vector<Claim> claims;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    claims = registry.sites[address_]->claims;
  }
  bool patch = false;
  for (const Claim& claim : claims) patch |= claim.fn(claim.user, address_, regs.begin(), regs.size());
  return patch;
}

uint64_t ClaimMidasm(const RexFabricMod* owner, uint32_t address, MidasmFn fn, void* user) {
  if (!fn) return 0;
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  Site& site = Intern(registry, address);
  const uint64_t id = registry.next_id++;
  site.claims.push_back({id, fn, user, owner});
  Sync(site);
  return id;
}

bool ReleaseMidasm(uint64_t id) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  for (auto& [address, site] : registry.sites) {
    if (std::erase_if(site->claims, [id](const Claim& claim) { return claim.id == id; })) {
      Sync(*site);
      return true;
    }
  }
  return false;
}

size_t ReleaseAllMidasm(const RexFabricMod* owner) {
  Registry& registry = Get();
  std::lock_guard lock(registry.mutex);
  size_t released = 0;
  for (auto& [address, site] : registry.sites) {
    released += std::erase_if(site->claims, [owner](const Claim& claim) { return claim.owner == owner; });
    Sync(*site);
  }
  return released;
}

std::vector<MidasmClaim> MidasmClaims() {
  std::vector<MidasmClaim> claims;
  {
    Registry& registry = Get();
    std::lock_guard lock(registry.mutex);
    for (const auto& [address, site] : registry.sites) {
      for (const Claim& claim : site->claims) claims.push_back({claim.id, address, claim.owner});
    }
  }
  std::sort(claims.begin(), claims.end(), [](const MidasmClaim& a, const MidasmClaim& b) { return a.address != b.address ? a.address < b.address : a.id < b.id; });
  return claims;
}

}  // namespace rexfabric
