#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using Byte = uint8_t;
using BytePtr = uint8_t*;

struct FileRange {
  uint64_t offset = 0;
  uint64_t size = 0;

  uint64_t end() const { return offset + size; }
  bool empty() const { return size == 0; }
};

class MachLoadError : public std::runtime_error {
 public:
  explicit MachLoadError(const std::string& msg) : std::runtime_error(msg) {}
};
