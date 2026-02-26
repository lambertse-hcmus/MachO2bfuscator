#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────
// Byte buffer alias
// ─────────────────────────────────────────────

using Byte = uint8_t;
using BytePtr = uint8_t*;

// ─────────────────────────────────────────────
// A contiguous file range [offset, offset+size)
// ─────────────────────────────────────────────
struct FileRange {
  uint64_t offset = 0;
  uint64_t size = 0;

  uint64_t end() const { return offset + size; }
  bool empty() const { return size == 0; }
};

// ─────────────────────────────────────────────
// Generic error for loading failures
// ─────────────────────────────────────────────
class MachLoadError : public std::runtime_error {
 public:
  explicit MachLoadError(const std::string& msg) : std::runtime_error(msg) {}
};
