#include "storage.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using esphome::x2d::JournalFlash;
using namespace ha_x2d::journal;

struct esp_partition_iterator { size_t index = 0; };

namespace fake {
esp_flash_t default_chip, other_chip;
std::vector<esp_partition_t> partitions;
esp_partition_iterator iterator;
bool iterator_live = false;
std::array<uint8_t, REGION_BYTES> bytes;
std::array<unsigned, 3> calls{};  // read, write, erase
esp_err_t read_result = ESP_OK, write_result = ESP_OK, erase_result = ESP_OK;
const esp_partition_t *last_partition = nullptr;
size_t last_offset = 0, last_length = 0;
const void *last_buffer = nullptr;

esp_partition_iterator_t find_next() {
  while (iterator.index < partitions.size()) {
    if (std::strcmp(partitions[iterator.index].label, "x2d_journal") == 0) {
      iterator_live = true;
      return &iterator;
    }
    ++iterator.index;
  }
  iterator_live = false;  // IDF releases the iterator when next() reaches the end.
  return nullptr;
}

void record(const esp_partition_t *partition, size_t offset, size_t length, const void *buffer) {
  assert(partition && offset <= bytes.size() && length <= bytes.size() - offset);
  last_partition = partition;
  last_offset = offset;
  last_length = length;
  last_buffer = buffer;
}
}  // namespace fake

esp_flash_t *esp_flash_default_chip = &fake::default_chip;

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
                                          esp_partition_subtype_t subtype, const char *label) {
  assert(type == ESP_PARTITION_TYPE_ANY && subtype == ESP_PARTITION_SUBTYPE_ANY);
  assert(label && std::strcmp(label, "x2d_journal") == 0 && !fake::iterator_live);
  fake::iterator.index = 0;
  return fake::find_next();
}

const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator) {
  assert(fake::iterator_live && iterator == &fake::iterator);
  return &fake::partitions.at(iterator->index);
}

esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator) {
  assert(fake::iterator_live && iterator == &fake::iterator);
  ++iterator->index;
  return fake::find_next();
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *out, size_t length) {
  ++fake::calls[0];
  fake::record(partition, offset, length, out);
  if (fake::read_result == ESP_OK) std::memcpy(out, fake::bytes.data() + offset, length);
  return fake::read_result;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *data, size_t length) {
  ++fake::calls[1];
  fake::record(partition, offset, length, data);
  if (fake::write_result == ESP_OK) std::memcpy(fake::bytes.data() + offset, data, length);
  return fake::write_result;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t length) {
  ++fake::calls[2];
  fake::record(partition, offset, length, nullptr);
  if (fake::erase_result == ESP_OK) std::memset(fake::bytes.data() + offset, 0xff, length);
  return fake::erase_result;
}

template<typename Call> static void rejected(Call call) {
  const auto before = fake::calls;
  assert(!call());
  assert(fake::calls == before);
}

static void closed(JournalFlash &flash) {
  assert(flash.size() == 0 && !fake::iterator_live);
  uint8_t page[PAGE_BYTES]{};
  rejected([&] { return flash.read(0, page, 1); });
  rejected([&] { return flash.read(0, page, 0); });
  rejected([&] { return flash.program_page(0, page); });
  rejected([&] { return flash.erase_sector(0); });
}

int main() {
  const esp_partition_t valid{esp_flash_default_chip, 0x40, 0x01, X2D_JOURNAL_ADDRESS,
                              REGION_BYTES, SECTOR_BYTES, "x2d_journal", false, false};
  JournalFlash flash;
  closed(flash);
  assert(!flash.open());  // No partition at all.
  closed(flash);

  auto unrelated = valid;
  std::strcpy(unrelated.label, "other");
  unrelated.type = 0;
  fake::partitions = {unrelated};
  assert(!flash.open());  // A different label cannot supply the journal.
  closed(flash);
  fake::partitions = {unrelated, valid, unrelated};
  assert(flash.open() && flash.size() == REGION_BYTES && !fake::iterator_live);
  uint8_t byte = 0;
  assert(flash.read(0, &byte, 1));
  assert(fake::last_partition == &fake::partitions[1]);
  fake::partitions.clear();
  assert(!flash.open());  // Reopen must discard the previous handle.
  closed(flash);

  auto invalid_partition = [&](auto change) {
    fake::partitions = {valid};
    assert(flash.open() && flash.size() == REGION_BYTES);
    const auto before = fake::calls;
    change(fake::partitions[0]);
    assert(!flash.open() && fake::calls == before);
    closed(flash);
  };
  invalid_partition([](auto &p) { p.flash_chip = &fake::other_chip; });
  invalid_partition([](auto &p) { p.flash_chip = nullptr; });
  invalid_partition([](auto &p) { p.type = 0x00; });
  invalid_partition([](auto &p) { p.type = 0x01; });
  invalid_partition([](auto &p) { p.subtype = 0x00; });
  invalid_partition([](auto &p) { p.subtype = 0x02; });
  invalid_partition([](auto &p) { p.address -= PAGE_BYTES; });
  invalid_partition([](auto &p) { p.address += PAGE_BYTES; });
  invalid_partition([](auto &p) { p.size -= PAGE_BYTES; });
  invalid_partition([](auto &p) { p.size += PAGE_BYTES; });
  invalid_partition([](auto &p) { p.erase_size = 0; });
  invalid_partition([](auto &p) { p.erase_size = SECTOR_BYTES / 2; });
  invalid_partition([](auto &p) { p.erase_size = SECTOR_BYTES * 2; });
  invalid_partition([](auto &p) { p.encrypted = true; });
  invalid_partition([](auto &p) { p.readonly = true; });

  for (uint8_t duplicate_type : {0x00, 0x01, 0x40}) {
    for (bool duplicate_first : {false, true}) {
      fake::partitions = {valid};
      assert(flash.open());
      auto duplicate = valid;
      duplicate.type = duplicate_type;
      fake::partitions = duplicate_first ? std::vector<esp_partition_t>{duplicate, valid}
                                         : std::vector<esp_partition_t>{valid, duplicate};
      const auto before = fake::calls;
      assert(!flash.open() && fake::calls == before);
      closed(flash);
    }
  }

  fake::partitions = {valid};
  assert(flash.open() && flash.open() && flash.size() == REGION_BYTES);
  assert(!fake::iterator_live);
  fake::bytes.fill(0xff);
  std::array<uint8_t, PAGE_BYTES> page{}, out{};
  for (size_t i = 0; i < page.size(); ++i) page[i] = static_cast<uint8_t>(i);

  // Accepted operations must forward the relative offset, length, buffer and handle.
  for (uint32_t offset : {0u, PAGE_BYTES, REGION_BYTES - PAGE_BYTES}) {
    const auto before = fake::calls;
    assert(flash.program_page(offset, page.data()));
    assert(fake::last_partition == &fake::partitions[0] && fake::last_offset == offset &&
           fake::last_length == PAGE_BYTES && fake::last_buffer == page.data());
    assert(flash.read(offset, out.data(), PAGE_BYTES) && out == page);
    assert(fake::last_partition == &fake::partitions[0] && fake::last_offset == offset &&
           fake::last_length == PAGE_BYTES && fake::last_buffer == out.data());
    assert(fake::calls[0] == before[0] + 1 && fake::calls[1] == before[1] + 1 &&
           fake::calls[2] == before[2]);
  }
  assert(flash.read(1, &byte, 1) && byte == page[1]);  // Reads need no page alignment.
  std::array<uint8_t, REGION_BYTES> whole{};
  assert(flash.read(0, whole.data(), REGION_BYTES) && whole == fake::bytes);
  for (uint32_t offset : {0u, REGION_BYTES}) {
    const auto before = fake::calls;
    assert(flash.read(offset, &byte, 0));  // Empty range at the end is valid.
    assert(fake::last_offset == offset && fake::last_length == 0);
    assert(fake::calls[0] == before[0] + 1 && fake::calls[1] == before[1] &&
           fake::calls[2] == before[2]);
  }
  assert(flash.read(REGION_BYTES - 1, &byte, 1) && byte == page.back());

  const uint32_t invalid_reads[][2] = {
      {0, REGION_BYTES + 1}, {1, REGION_BYTES}, {REGION_BYTES - 1, 2},
      {REGION_BYTES, 1}, {REGION_BYTES + 1, 0}, {UINT32_MAX, 0},
      {UINT32_MAX, 1}, {1, UINT32_MAX}, {REGION_BYTES - 1, UINT32_MAX},
      {UINT32_MAX, UINT32_MAX}, {UINT32_MAX - PAGE_BYTES + 1, PAGE_BYTES}};
  for (const auto &range : invalid_reads)
    rejected([&] { return flash.read(range[0], out.data(), range[1]); });
  const uint32_t invalid_pages[] = {1, PAGE_BYTES - 1, PAGE_BYTES + 1,
      REGION_BYTES - PAGE_BYTES + 1, REGION_BYTES - 1, REGION_BYTES,
      REGION_BYTES + PAGE_BYTES, UINT32_MAX, UINT32_MAX - PAGE_BYTES + 1};
  for (uint32_t offset : invalid_pages)
    rejected([&] { return flash.program_page(offset, page.data()); });
  for (uint32_t offset : {0u, PAGE_BYTES, REGION_BYTES - PAGE_BYTES})
    rejected([&] { return flash.program_page(offset, nullptr); });
  const uint32_t invalid_sectors[] = {1, SECTOR_BYTES - 1, SECTOR_BYTES + 1,
      REGION_BYTES - SECTOR_BYTES + 1, REGION_BYTES - 1, REGION_BYTES,
      REGION_BYTES + SECTOR_BYTES, UINT32_MAX, UINT32_MAX - SECTOR_BYTES + 1};
  for (uint32_t offset : invalid_sectors)
    rejected([&] { return flash.erase_sector(offset); });

  for (esp_err_t error : {-1, 1}) {
    auto before = fake::calls;
    byte = 0xa5;
    fake::read_result = error;
    assert(!flash.read(1, &byte, 1) && byte == 0xa5);
    fake::read_result = ESP_OK;
    assert(fake::calls[0] == before[0] + 1 && fake::calls[1] == before[1] &&
           fake::calls[2] == before[2]);
    before = fake::calls;
    fake::write_result = error;
    assert(!flash.program_page(PAGE_BYTES * 2, page.data()));
    fake::write_result = ESP_OK;
    assert(fake::bytes[PAGE_BYTES * 2] == 0xff);
    assert(fake::calls[0] == before[0] && fake::calls[1] == before[1] + 1 &&
           fake::calls[2] == before[2]);
    before = fake::calls;
    fake::erase_result = error;
    assert(!flash.erase_sector(0));
    fake::erase_result = ESP_OK;
    assert(fake::bytes[0] == page[0]);
    assert(fake::calls[0] == before[0] && fake::calls[1] == before[1] &&
           fake::calls[2] == before[2] + 1);
    assert(flash.size() == REGION_BYTES);  // API failures do not close the handle.
  }
  assert(flash.read(1, &byte, 1) && byte == page[1]);
  assert(flash.program_page(PAGE_BYTES * 2, page.data()));
  for (uint32_t offset : {0u, SECTOR_BYTES, REGION_BYTES - SECTOR_BYTES}) {
    fake::bytes.fill(0x5a);
    const auto before = fake::calls;
    assert(flash.erase_sector(offset));
    assert(fake::last_partition == &fake::partitions[0] && fake::last_offset == offset &&
           fake::last_length == SECTOR_BYTES);
    assert(fake::calls[0] == before[0] && fake::calls[1] == before[1] &&
           fake::calls[2] == before[2] + 1);
    for (size_t i = 0; i < fake::bytes.size(); ++i)
      assert(fake::bytes[i] == (i >= offset && i - offset < SECTOR_BYTES ? 0xff : 0x5a));
  }
  puts("storage: partition validation, reopen, bounds/alignment, API errors and rejected-call isolation passed");
}
