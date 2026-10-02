#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_flash.h"

using esp_err_t = int32_t;
constexpr esp_err_t ESP_OK = 0;
enum esp_partition_type_t { ESP_PARTITION_TYPE_ANY = 0xff };
enum esp_partition_subtype_t { ESP_PARTITION_SUBTYPE_ANY = 0xff };

// Only the fields and APIs used by the real storage.h are needed here.
struct esp_partition_t {
  esp_flash_t *flash_chip;
  uint8_t type;
  uint8_t subtype;
  uint32_t address;
  uint32_t size;
  uint32_t erase_size;
  char label[17];
  bool encrypted;
  bool readonly;
};
struct esp_partition_iterator;
using esp_partition_iterator_t = esp_partition_iterator *;

esp_partition_iterator_t esp_partition_find(esp_partition_type_t type,
                                          esp_partition_subtype_t subtype, const char *label);
const esp_partition_t *esp_partition_get(esp_partition_iterator_t iterator);
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t iterator);
esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *out, size_t length);
esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *data, size_t length);
esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t length);
