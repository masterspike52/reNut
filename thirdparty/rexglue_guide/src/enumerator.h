#pragma once
// The Guide's enumerators (friends, title servers, stats): kernel objects of
// its own, served by its XamEnumerate (enumerator.cpp).

#include <cstdint>
#include <memory>
#include <vector>

#include <rex/kernel/object/xam_object.h>

namespace rexglue_guide::enumerator {

constexpr uint32_t kNoMoreFiles = 18;  // ERROR_NO_MORE_FILES

class Enumerator : public rex::kernel::IXAMObject {
 public:
  static constexpr uint32_t kTag = 0x52474E45;  // 'RGNE'

  Enumerator(uint32_t items_per_enumerate, uint32_t item_size)
      : items_per_enumerate_(items_per_enumerate), item_size_(item_size) {}
  uint32_t GetTag() const override { return kTag; }

  uint32_t items_per_enumerate() const { return items_per_enumerate_; }
  uint32_t item_size() const { return item_size_; }

  // The next items into `buffer` (at guest address buffer_ptr): 0 and how many,
  // or kNoMoreFiles.
  virtual uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer, uint32_t* written_count) = 0;

 private:
  uint32_t items_per_enumerate_;
  uint32_t item_size_;
};

// Items made up front, handed out items_per_enumerate at a time.
class StaticEnumerator final : public Enumerator {
 public:
  using Enumerator::Enumerator;
  // A zeroed item at the end; valid until the next one is added.
  uint8_t* AppendItem();
  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer, uint32_t* written_count) override;

 private:
  std::vector<uint8_t> items_;
  uint32_t count_ = 0;
  uint32_t current_ = 0;
};

// A handle for `e`, with a guest X_KENUMERATOR block (app, open and close messages, user, items,
// flags) and `extra` bytes of private data after it. 0 when it can't be made.
uint32_t Create(std::unique_ptr<Enumerator> e, uint32_t user, uint32_t app, uint32_t open, uint32_t close,
                uint32_t flags, uint32_t extra = 0, uint8_t** private_data = nullptr);

}  // namespace rexglue_guide::enumerator
