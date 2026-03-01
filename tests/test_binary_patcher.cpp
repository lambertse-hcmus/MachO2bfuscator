#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "MachO2bfuscator/binary_patcher.h"
#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"

static int g_passed = 0, g_failed = 0;
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
#define ASSERT(c)                                                    \
  do {                                                               \
    if (!(c))                                                        \
      throw std::runtime_error("Assertion failed: " #c " at line " + \
                               std::to_string(__LINE__));            \
  } while (0)
#define ASSERT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b))                                                            \
      throw std::runtime_error(std::string("Expected: ") + std::to_string(a) + \
                               " got: " + std::to_string(b));                  \
  } while (0)

const std::string objCPath =
    "/Users/tri.le/src/opensource/lambertse/MachO2bfuscator/assets/"
    "testckey_objc";
// ═══════════════════════════════════════════════════════════════
//  FakeSlice
//
//  Owns a heap buffer and a MachOSlice that points into it.
//  The slice.data pointer is fixed up after every move so it
//  always points into THIS object's buffer — never a stale one.
// ═══════════════════════════════════════════════════════════════
struct FakeSlice {
  std::vector<uint8_t> buffer;
  MachOSlice slice;

  // Default constructor — produces an empty FakeSlice
  FakeSlice() = default;

  // Not copyable — slice.data would become a dangling pointer
  FakeSlice(const FakeSlice&) = delete;
  FakeSlice& operator=(const FakeSlice&) = delete;

  // Move constructor — fix up slice.data after the buffer moves
  FakeSlice(FakeSlice&& other) noexcept
      : buffer(std::move(other.buffer)), slice(std::move(other.slice)) {
    // Re-point slice.data into the new buffer location
    slice.data = buffer.data();
    other.slice.data = nullptr;
  }

  // Move assignment — same fix-up
  FakeSlice& operator=(FakeSlice&& other) noexcept {
    if (this != &other) {
      buffer = std::move(other.buffer);
      slice = std::move(other.slice);
      slice.data = buffer.data();
      other.slice.data = nullptr;
    }
    return *this;
  }
};

// ── makeFakeSlice ─────────────────────────────────────────────────
// Builds a FakeSlice containing up to three sections.
// Pass an empty string to omit a section.
static FakeSlice makeFakeSlice(const std::string& methname,
                               const std::string& classname,
                               const std::string& methtype) {
  FakeSlice fs;
  size_t totalSize = methname.size() + classname.size() + methtype.size();

  fs.buffer.resize(totalSize);

  // slice.data will be fixed up by the move constructor on return
  fs.slice.data = fs.buffer.data();
  fs.slice.dataSize = totalSize;
  fs.slice.is64bit = true;

  MachSegment seg;
  seg.name = "__TEXT";
  seg.vmAddr = 0x100000000ULL;
  seg.vmSize = totalSize;
  seg.fileOff = 0;
  seg.fileSize = totalSize;

  uint64_t offset = 0;

  if (!methname.empty()) {
    std::memcpy(fs.buffer.data() + offset, methname.data(), methname.size());
    MachSection sec;
    sec.segmentName = "__TEXT";
    sec.sectionName = "__objc_methname";
    sec.fileOffset = offset;
    sec.size = methname.size();
    seg.sections.push_back(sec);
    offset += methname.size();
  }

  if (!classname.empty()) {
    std::memcpy(fs.buffer.data() + offset, classname.data(), classname.size());
    MachSection sec;
    sec.segmentName = "__TEXT";
    sec.sectionName = "__objc_classname";
    sec.fileOffset = offset;
    sec.size = classname.size();
    seg.sections.push_back(sec);
    offset += classname.size();
  }

  if (!methtype.empty()) {
    std::memcpy(fs.buffer.data() + offset, methtype.data(), methtype.size());
    MachSection sec;
    sec.segmentName = "__TEXT";
    sec.sectionName = "__objc_methtype";
    sec.fileOffset = offset;
    sec.size = methtype.size();
    seg.sections.push_back(sec);
    offset += methtype.size();
  }

  fs.slice.segments.push_back(std::move(seg));
  return fs;
}

// ── MethTypeObfuscator unit tests ─────────────────────────────────

void test_methtype_no_change_when_no_match() {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  std::string input = "v16@0:8";
  ASSERT(obf.obfuscate(input) == input);
}

void test_methtype_replaces_in_double_quotes() {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  std::string input = "v32@0:8@\"MyClass\"16";
  std::string expected = "v32@0:8@\"ObfClass\"16";
  ASSERT(obf.obfuscate(input) == expected);
}

void test_methtype_replaces_in_angle_brackets() {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  std::string input = "@\"<MyClass>\"";
  std::string expected = "@\"<ObfClass>\"";
  ASSERT(obf.obfuscate(input) == expected);
}

void test_methtype_replaces_multiple_classes() {
  MethTypeObfuscator obf(
      {{"MyClass", "ObfClass"}, {"AnotherClass", "XyzClass"}});
  std::string input = "v48@0:8@\"MyClass\"16@\"AnotherClass\"24";
  std::string expected = "v48@0:8@\"ObfClass\"16@\"XyzClass\"24";
  ASSERT(obf.obfuscate(input) == expected);
}

void test_methtype_does_not_replace_partial_match() {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  std::string input = "v32@0:8@\"MyClassExtended\"16";
  ASSERT(obf.obfuscate(input) == input);
}

void test_methtype_all_delimiter_pairs() {
  MethTypeObfuscator obf({{"Foo", "Bar"}});
  ASSERT(obf.obfuscate("\"Foo\"") == "\"Bar\"");
  ASSERT(obf.obfuscate("(Foo)") == "(Bar)");
  ASSERT(obf.obfuscate("[Foo]") == "[Bar]");
  ASSERT(obf.obfuscate("<Foo>") == "<Bar>");
  ASSERT(obf.obfuscate("{Foo}") == "{Bar}");
}

// ── BinaryPatcher in-memory unit tests ───────────────────────────

void test_patch_selectors_in_memory() {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("viewDidLoad\0init\0"s, "", "");

  ManglingMap map;
  map.selectors["viewDidLoad"] = "xxxxxXxxXxx";  // length 11
  map.selectors["init"] = "xNit";                // length 4

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  ASSERT_EQ(r.selectorPatches, 2u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  ASSERT(std::string(ptr) == "xxxxxXxxXxx");
  ptr += 12;  // 11 chars + NUL
  ASSERT(std::string(ptr) == "xNit");
}

void test_patch_classnames_in_memory() {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("", "MyViewController\0AppDelegate\0"s, "");

  ManglingMap map;
  map.classNames["MyViewController"] = "XyViewController";  // length 16
  map.classNames["AppDelegate"] = "XppDelegate";            // length 11

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  ASSERT_EQ(r.classPatches, 2u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  ASSERT(std::string(ptr) == "XyViewController");
  ptr += 17;  // 16 chars + NUL
  ASSERT(std::string(ptr) == "XppDelegate");
}

void test_patch_methtype_in_memory() {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("", "", "v32@0:8@\"MyClass\"16\0"s);

  ManglingMap map;
  map.classNames["MyClass"] = "ObfClss";  // same length = 7

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  ASSERT_EQ(r.methTypePatches, 1u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  ASSERT(std::string(ptr) == "v32@0:8@\"ObfClss\"16");
}

void test_patch_shorter_name_nul_padded() {
  using namespace std::string_literals;
  // "LongClassName\0NextClass\0" — two adjacent NUL-terminated strings
  FakeSlice fs = makeFakeSlice("", "LongClassName\0NextClass\0"s, "");

  ManglingMap map;
  map.classNames["LongClassName"] = "Short";  // 5 < 13

  BinaryPatcher::patch(fs.slice, map);

  const uint8_t* ptr = fs.buffer.data();

  // First 5 bytes = "Short"
  ASSERT(std::memcmp(ptr, "Short", 5) == 0);
  // Bytes 5..13 must be NUL (8 padding + 1 NUL terminator)
  for (int i = 5; i < 14; ++i) {
    ASSERT(ptr[i] == 0);
  }
  // The next string must be intact
  ptr += 14;
  ASSERT(std::string(reinterpret_cast<const char*>(ptr)) == "NextClass");
}

void test_patch_order_methtype_before_classname() {
  // If classname were patched first, the methtype obfuscator would
  // search for the already-obfuscated name and find nothing.
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("", "MyClass\0"s, "v16@0:8@\"MyClass\"8\0"s);

  ManglingMap map;
  map.classNames["MyClass"] = "ObfClss";  // same length = 7

  PatchResult r = BinaryPatcher::patch(fs.slice, map);

  ASSERT_EQ(r.classPatches, 1u);
  ASSERT_EQ(r.methTypePatches, 1u);

  // classname section
  const char* classnamePtr = reinterpret_cast<const char*>(fs.buffer.data());
  ASSERT(std::string(classnamePtr) == "ObfClss");

  // methtype section (starts after classname section: 7 chars + NUL = 8 bytes)
  const char* methtypePtr = classnamePtr + 8;
  ASSERT(std::string(methtypePtr) == "v16@0:8@\"ObfClss\"8");
}

void test_patch_unmatched_symbol_unchanged() {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("unmappedSelector\0"s, "", "");

  ManglingMap map;
  map.selectors["somethingElse"] = "xxxxxxxxxxxxx";

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  ASSERT_EQ(r.selectorPatches, 0u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  ASSERT(std::string(ptr) == "unmappedSelector");
}

// ── Integration tests ─────────────────────────────────────────────

void test_patch_real_binary() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("TEST_BINARY_PATH not set");

  namespace fs = std::filesystem;
  std::string dstPath = path + ".phase6_test_tmp";

  try {
    SymbolsCollector::Config config;
    config.obfuscablePaths = {path};
    ObfuscationSymbols symbols = SymbolsCollector::collect(config);

    CaesarMangler mangler(13);
    ManglingMap map = mangler.mangle(symbols);

    ASSERT(!map.selectors.empty());
    ASSERT(!map.classNames.empty());

    // ── Patch src → dst, original untouched ───────────────────
    PatchResult result = BinaryPatcher::patchFile(path, dstPath, map);

    std::cout << "\n    patched: " << result.selectorPatches << " selectors, "
              << result.classPatches << " class names, "
              << result.methTypePatches << " methtype strings\n";

    ASSERT(result.selectorPatches > 0);
    ASSERT(result.classPatches > 0);

    // Reload patched binary and verify old names are gone
    MachOImage patched = loadMachOImage(dstPath);
    ASSERT(!patched.slices.empty());

    for (const auto& slice : patched.slices) {
      const MachSection* methnameSec = slice.objcMethNameSection();
      if (methnameSec) {
        uint64_t cursor = methnameSec->fileOffset;
        uint64_t end = methnameSec->fileOffset + methnameSec->size;
        while (cursor < end) {
          const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
          size_t len = strnlen(ptr, static_cast<size_t>(end - cursor));
          if (len > 0) {
            std::string found(ptr, len);
            ASSERT(map.selectors.find(found) == map.selectors.end());
          }
          cursor += len + 1;
        }
      }

      const MachSection* classnameSec = slice.objcClassNameSection();
      if (classnameSec) {
        uint64_t cursor = classnameSec->fileOffset;
        uint64_t end = classnameSec->fileOffset + classnameSec->size;
        while (cursor < end) {
          const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
          size_t len = strnlen(ptr, static_cast<size_t>(end - cursor));
          if (len > 0) {
            std::string found(ptr, len);
            ASSERT(map.classNames.find(found) == map.classNames.end());
          }
          cursor += len + 1;
        }
      }

      if (methnameSec && result.selectorPatches > 0) {
        bool foundMangled = false;
        uint64_t cursor = methnameSec->fileOffset;
        uint64_t end = methnameSec->fileOffset + methnameSec->size;
        while (cursor < end && !foundMangled) {
          const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
          size_t len = strnlen(ptr, static_cast<size_t>(end - cursor));
          if (len > 0) {
            std::string found(ptr, len);
            for (const auto& [orig, mangled] : map.selectors) {
              if (found == mangled) {
                foundMangled = true;
                break;
              }
            }
          }
          cursor += len + 1;
        }
        ASSERT(foundMangled);
      }
    }

  } catch (...) {
    throw;
  }
}

void test_caesar_roundtrip() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("TEST_BINARY_PATH not set");

  namespace fs = std::filesystem;
  std::string encryptedPath = path + ".phase6_encrypted_tmp";
  std::string restoredPath = path + ".phase6_restored_tmp";

  try {
    SymbolsCollector::Config config;
    config.obfuscablePaths = {path};
    ObfuscationSymbols symbols = SymbolsCollector::collect(config);

    // ── Forward pass: src → encrypted ─────────────────────────
    CaesarMangler forward(13);
    ManglingMap forwardMap = forward.mangle(symbols);
    BinaryPatcher::patchFile(path, encryptedPath, forwardMap);

    // ── Build reverse map: mangled → original ──────────────────
    ManglingMap reverseMap;
    for (const auto& [orig, mangled] : forwardMap.selectors) {
      reverseMap.selectors[mangled] = orig;
    }
    for (const auto& [orig, mangled] : forwardMap.classNames) {
      reverseMap.classNames[mangled] = orig;
    }

    // ── Reverse pass: encrypted → restored ────────────────────
    BinaryPatcher::patchFile(encryptedPath, restoredPath, reverseMap);

    // ── Compare original vs restored byte-for-byte ────────────
    auto readFile = [](const std::string& p) -> std::vector<uint8_t> {
      std::ifstream f(p, std::ios::binary | std::ios::ate);
      if (!f) throw std::runtime_error("Cannot read " + p);
      size_t sz = static_cast<size_t>(f.tellg());
      f.seekg(0);
      std::vector<uint8_t> buf(sz);
      f.read(reinterpret_cast<char*>(buf.data()),
             static_cast<std::streamsize>(sz));
      return buf;
    };

    auto original = readFile(path);
    auto restored = readFile(restoredPath);

    ASSERT(original.size() == restored.size());
    ASSERT(original == restored);

    fs::remove(encryptedPath);
    fs::remove(restoredPath);

  } catch (...) {
    fs::remove(encryptedPath);
    fs::remove(restoredPath);
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────
int main() {
  std::cout << "=== Phase 6 Tests: Binary Patching ===\n\n";

  // std::cout << "── MethTypeObfuscator ──\n";
  // RUN(test_methtype_no_change_when_no_match);
  // RUN(test_methtype_replaces_in_double_quotes);
  // RUN(test_methtype_replaces_in_angle_brackets);
  // RUN(test_methtype_replaces_multiple_classes);
  // RUN(test_methtype_does_not_replace_partial_match);
  // RUN(test_methtype_all_delimiter_pairs);
  //
  // std::cout << "\n── BinaryPatcher (in-memory) ──\n";
  // RUN(test_patch_selectors_in_memory);
  // RUN(test_patch_classnames_in_memory);
  // RUN(test_patch_methtype_in_memory);
  // RUN(test_patch_shorter_name_nul_padded);
  // RUN(test_patch_order_methtype_before_classname);
  // RUN(test_patch_unmatched_symbol_unchanged);

  std::cout << "\n── Integration ──\n";
  RUN(test_patch_real_binary);
  // RUN(test_caesar_roundtrip);

  std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed
            << " failed ===\n";
  return g_failed > 0 ? 1 : 0;
}
