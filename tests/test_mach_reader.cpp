#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "MachO2fuscator/mach_reader.h"

// ── Minimal test harness ─────────────────────────────────────────
static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) void name()
#define RUN(name)                                \
  do {                                           \
    std::cout << "  running " #name " ... ";     \
    try {                                        \
      name();                                    \
      std::cout << "PASS\n";                     \
      ++g_passed;                                \
    } catch (const std::exception& e) {          \
      std::cout << "FAIL: " << e.what() << "\n"; \
      ++g_failed;                                \
    }                                            \
  } while (0)

#define ASSERT(cond)                                                    \
  do {                                                                  \
    if (!(cond))                                                        \
      throw std::runtime_error("Assertion failed: " #cond " at line " + \
                               std::to_string(__LINE__));               \
  } while (0)

#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) throw std::runtime_error("Expected equal: " #a " vs " #b); \
  } while (0)

// ── Test: loading a non-existent file throws ─────────────────────
TEST(test_load_nonexistent_throws) {
  bool threw = false;
  try {
    loadMachOImage("/nonexistent/path/to/binary");
  } catch (const MachLoadError&) {
    threw = true;
  }
  ASSERT(threw);
}

// ── Test: FileRange helper ────────────────────────────────────────
TEST(test_file_range) {
  FileRange r{100, 50};
  ASSERT_EQ(r.offset, 100u);
  ASSERT_EQ(r.size, 50u);
  ASSERT_EQ(r.end(), 150u);
  ASSERT(!r.empty());

  FileRange empty{0, 0};
  ASSERT(empty.empty());
}

// ── Test: vmOffset → fileOffset translation ──────────────────────
// Build a synthetic MachOSlice with one segment and verify translation.
TEST(test_vm_to_file_offset) {
  MachOSlice slice;

  // Fake segment: vm [0x100000000, 0x100001000), file [0x0, 0x1000)
  MachSegment seg;
  seg.name = "__TEXT";
  seg.vmAddr = 0x100000000ULL;
  seg.vmSize = 0x1000;
  seg.fileOff = 0x0;
  seg.fileSize = 0x1000;
  slice.segments.push_back(seg);

  // vmAddr 0x100000500 should map to fileOffset 0x500
  uint64_t off = slice.fileOffsetFromVmOffset(0x100000500ULL);
  ASSERT_EQ(off, 0x500u);
}

// ── Test: vmOffset outside all segments throws ───────────────────
TEST(test_vm_to_file_offset_out_of_bounds) {
  MachOSlice slice;
  MachSegment seg;
  seg.vmAddr = 0x100000000ULL;
  seg.vmSize = 0x1000;
  seg.fileOff = 0x0;
  seg.fileSize = 0x1000;
  slice.segments.push_back(seg);

  bool threw = false;
  try {
    slice.fileOffsetFromVmOffset(0x200000000ULL);  // outside segment
  } catch (const MachLoadError&) {
    threw = true;
  }
  ASSERT(threw);
}

// ── Test: findSection ────────────────────────────────────────────
TEST(test_find_section) {
  MachOSlice slice;

  MachSection sec;
  sec.segmentName = "__TEXT";
  sec.sectionName = "__objc_classname";
  sec.fileOffset = 0x1000;
  sec.size = 0x200;

  MachSegment seg;
  seg.name = "__TEXT";
  seg.sections.push_back(sec);
  slice.segments.push_back(seg);

  const MachSection* found = slice.findSection("__TEXT", "__objc_classname");
  ASSERT(found != nullptr);
  ASSERT_EQ(found->fileOffset, 0x1000u);
  ASSERT_EQ(found->size, 0x200u);

  // Missing section returns nullptr
  ASSERT(slice.findSection("__TEXT", "__objc_methname") == nullptr);
  // Missing segment returns nullptr
  ASSERT(slice.findSection("__DATA", "__objc_classname") == nullptr);
}

// ── Test: named accessor convenience methods ──────────────────────
TEST(test_named_section_accessors) {
  MachOSlice slice;

  // Add __TEXT segment with __objc_classname
  MachSegment textSeg;
  textSeg.name = "__TEXT";
  {
    MachSection s;
    s.segmentName = "__TEXT";
    s.sectionName = "__objc_classname";
    s.fileOffset = 0x100;
    s.size = 0x50;
    textSeg.sections.push_back(s);
  }
  {
    MachSection s;
    s.segmentName = "__TEXT";
    s.sectionName = "__objc_methname";
    s.fileOffset = 0x200;
    s.size = 0x80;
    textSeg.sections.push_back(s);
  }
  slice.segments.push_back(textSeg);

  // Add __DATA segment with __objc_classlist
  MachSegment dataSeg;
  dataSeg.name = "__DATA";
  {
    MachSection s;
    s.segmentName = "__DATA";
    s.sectionName = "__objc_classlist";
    s.fileOffset = 0x500;
    s.size = 0x40;
    dataSeg.sections.push_back(s);
  }
  slice.segments.push_back(dataSeg);

  ASSERT(slice.objcClassNameSection() != nullptr);
  ASSERT(slice.objcMethNameSection() != nullptr);
  ASSERT(slice.objcClasslistSection() != nullptr);
  ASSERT(slice.objcMethTypeSection() == nullptr);  // not added
  ASSERT(slice.objcCatlistSection() == nullptr);   // not added

  ASSERT_EQ(slice.objcClassNameSection()->fileOffset, 0x100u);
  ASSERT_EQ(slice.objcMethNameSection()->fileOffset, 0x200u);
  ASSERT_EQ(slice.objcClasslistSection()->fileOffset, 0x500u);
}

// ── Test: loading /usr/lib/libSystem.B.dylib (always present on macOS) ───
// This is an integration test against a real system binary.
TEST(test_load_real_binary) {
  // /usr/lib/libSystem.B.dylib is present on every macOS install
  const char* testBin = "/usr/lib/libSystem.B.dylib";
  MachOImage image = loadMachOImage(testBin);

  // Should have at least one slice
  ASSERT(!image.slices.empty());

  // Every slice must have at least a __TEXT segment
  for (const auto& slice : image.slices) {
    bool hasText = false;
    for (const auto& seg : slice.segments) {
      if (seg.name == "__TEXT") {
        hasText = true;
        break;
      }
    }
    ASSERT(hasText);
  }
}

// ── Test: loading /usr/lib/libc++.1.dylib — fat binary on Apple Silicon ──
TEST(test_load_fat_or_thin_binary) {
  // This path always exists on macOS; may be fat or thin depending on platform
  const char* testBin = "/usr/lib/libc++.1.dylib";
  MachOImage image = loadMachOImage(testBin);
  ASSERT(!image.slices.empty());
  // All slices must have non-null data pointers
  for (const auto& slice : image.slices) {
    ASSERT(slice.data != nullptr);
    ASSERT(slice.dataSize > 0);
  }
}

// ────────────────────────────────────────────────────────────────
int main() {
  std::cout
      << "=== Phase 1 Tests: Mach-O Binary Loading & Section Lookup ===\n\n";

  RUN(test_load_nonexistent_throws);
  RUN(test_file_range);
  RUN(test_vm_to_file_offset);
  RUN(test_vm_to_file_offset_out_of_bounds);
  RUN(test_find_section);
  RUN(test_named_section_accessors);
  RUN(test_load_real_binary);
  RUN(test_load_fat_or_thin_binary);

  std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed
            << " failed ===\n";
  return g_failed > 0 ? 1 : 0;
}
