#pragma once

#include <x2d/journal.h>
#include <esp_partition.h>
#include <esp_flash.h>

namespace esphome::x2d {

class JournalFlash final : public ::x2d::journal::Flash {
 public:
  bool open() {
    partition_ = nullptr;
    // Search by label across types as well: duplicate/wrong-type labels are
    // configuration errors, not an excuse to open a different journal.
    auto iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "x2d_journal");
    unsigned matches = 0;
    while (iterator) {
      partition_ = esp_partition_get(iterator);
      ++matches;
      iterator = esp_partition_next(iterator);
    }
    if (matches != 1 || !partition_ || partition_->flash_chip != esp_flash_default_chip ||
        partition_->type != 0x40 || partition_->subtype != 0x01 ||
        partition_->size != ::x2d::journal::REGION_BYTES ||
        partition_->erase_size != ::x2d::journal::SECTOR_BYTES ||
        partition_->address != X2D_JOURNAL_ADDRESS || partition_->encrypted || partition_->readonly) {
      partition_ = nullptr;
      return false;
    }
    return true;
  }
  uint32_t size() const override { return partition_ ? partition_->size : 0; }
  bool read(uint32_t offset, void *out, uint32_t length) override {
    return range(offset, length) && esp_partition_read(partition_, offset, out, length) == ESP_OK;
  }
  bool erase_sector(uint32_t offset) override {
    constexpr auto bytes = ::x2d::journal::SECTOR_BYTES;
    return offset % bytes == 0 && range(offset, bytes) &&
           esp_partition_erase_range(partition_, offset, bytes) == ESP_OK;
  }
  bool program_page(uint32_t offset, const uint8_t *page) override {
    constexpr auto bytes = ::x2d::journal::PAGE_BYTES;
    return page && offset % bytes == 0 && range(offset, bytes) &&
           esp_partition_write(partition_, offset, page, bytes) == ESP_OK;
  }
 private:
  bool range(uint32_t offset, uint32_t length) const {
    return partition_ && offset <= partition_->size && length <= partition_->size - offset;
  }
  const esp_partition_t *partition_ = nullptr;
};

}  // namespace esphome::x2d
