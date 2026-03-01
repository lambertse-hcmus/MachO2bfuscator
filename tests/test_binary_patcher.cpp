#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "MachO2bfuscator/binary_patcher.h"
#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"

#ifdef TEST_OBJC_ABSOLUTE_PATH
static const std::string kBinaryPath = "assets/testckey_objc";
#else
#warning \
    "TEST_OBJC_ABSOLUTE_PATH is not defined. Some tests will be skipped that require a real binary."
static const std::string kBinaryPath = "";
#endif

// ════════════════════════════════════════════════════════════════════
//  FakeSlice fixture — owns the buffer, slice points into it
// ════════════════════════════════════════════════════════════════════
struct FakeSlice {
  std::vector<uint8_t> buffer;
  MachOSlice slice;

  FakeSlice() = default;
  FakeSlice(const FakeSlice&) = delete;
  FakeSlice& operator=(const FakeSlice&) = delete;
  FakeSlice(FakeSlice&& o) noexcept
      : buffer(std::move(o.buffer)), slice(std::move(o.slice)) {
    slice.data = buffer.data();
    o.slice.data = nullptr;
  }
  FakeSlice& operator=(FakeSlice&& o) noexcept {
    if (this != &o) {
      buffer = std::move(o.buffer);
      slice = std::move(o.slice);
      slice.data = buffer.data();
      o.slice.data = nullptr;
    }
    return *this;
  }
};

static FakeSlice makeFakeSlice(const std::string& methname,
                               const std::string& classname,
                               const std::string& methtype) {
  FakeSlice fs;
  size_t totalSize = methname.size() + classname.size() + methtype.size();
  fs.buffer.resize(totalSize);
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
  auto addSection = [&](const std::string& data, const std::string& secName) {
    if (data.empty()) return;
    std::memcpy(fs.buffer.data() + offset, data.data(), data.size());
    MachSection sec;
    sec.segmentName = "__TEXT";
    sec.sectionName = secName;
    sec.fileOffset = offset;
    sec.size = data.size();
    seg.sections.push_back(sec);
    offset += data.size();
  };

  addSection(methname, "__objc_methname");
  addSection(classname, "__objc_classname");
  addSection(methtype, "__objc_methtype");

  fs.slice.segments.push_back(std::move(seg));
  return fs;
}

// ════════════════════════════════════════════════════════════════════
//  MethTypeObfuscator
// ════════════════════════════════════════════════════════════════════

TEST(MethTypeObfuscator, NoChangeWhenNoMatch) {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  std::string input = "v16@0:8";
  EXPECT_EQ(obf.obfuscate(input), input);
}

TEST(MethTypeObfuscator, ReplacesInDoubleQuotes) {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  EXPECT_EQ(obf.obfuscate("v32@0:8@\"MyClass\"16"), "v32@0:8@\"ObfClass\"16");
}

TEST(MethTypeObfuscator, ReplacesInAngleBrackets) {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  EXPECT_EQ(obf.obfuscate("@\"<MyClass>\""), "@\"<ObfClass>\"");
}

TEST(MethTypeObfuscator, ReplacesMultipleClasses) {
  MethTypeObfuscator obf(
      {{"MyClass", "ObfClass"}, {"AnotherClass", "XyzClass"}});
  EXPECT_EQ(obf.obfuscate("v48@0:8@\"MyClass\"16@\"AnotherClass\"24"),
            "v48@0:8@\"ObfClass\"16@\"XyzClass\"24");
}

TEST(MethTypeObfuscator, DoesNotReplacePartialMatch) {
  MethTypeObfuscator obf({{"MyClass", "ObfClass"}});
  std::string input = "v32@0:8@\"MyClassExtended\"16";
  EXPECT_EQ(obf.obfuscate(input), input);
}

TEST(MethTypeObfuscator, AllDelimiterPairs) {
  MethTypeObfuscator obf({{"Foo", "Bar"}});
  EXPECT_EQ(obf.obfuscate("\"Foo\""), "\"Bar\"");
  EXPECT_EQ(obf.obfuscate("(Foo)"), "(Bar)");
  EXPECT_EQ(obf.obfuscate("[Foo]"), "[Bar]");
  EXPECT_EQ(obf.obfuscate("<Foo>"), "<Bar>");
  EXPECT_EQ(obf.obfuscate("{Foo}"), "{Bar}");
}

// ════════════════════════════════════════════════════════════════════
//  BinaryPatcher — in-memory
// ════════════════════════════════════════════════════════════════════

TEST(BinaryPatcher, PatchSelectorsInMemory) {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("viewDidLoad\0init\0"s, "", "");

  ManglingMap map;
  map.selectors["viewDidLoad"] = "xxxxxXxxXxx";  // len 11
  map.selectors["init"] = "xNit";                // len 4

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  EXPECT_EQ(r.selectorPatches, 2u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  EXPECT_STREQ(ptr, "xxxxxXxxXxx");
  ptr += 12;
  EXPECT_STREQ(ptr, "xNit");
}

TEST(BinaryPatcher, PatchClassNamesInMemory) {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("", "MyViewController\0AppDelegate\0"s, "");

  ManglingMap map;
  map.classNames["MyViewController"] = "XyViewController";  // len 16
  map.classNames["AppDelegate"] = "XppDelegate";            // len 11

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  EXPECT_EQ(r.classPatches, 2u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  EXPECT_STREQ(ptr, "XyViewController");
  ptr += 17;
  EXPECT_STREQ(ptr, "XppDelegate");
}

TEST(BinaryPatcher, PatchMethTypeInMemory) {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("", "", "v32@0:8@\"MyClass\"16\0"s);

  ManglingMap map;
  map.classNames["MyClass"] = "ObfClss";  // len 7

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  EXPECT_EQ(r.methTypePatches, 1u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  EXPECT_STREQ(ptr, "v32@0:8@\"ObfClss\"16");
}

TEST(BinaryPatcher, ShorterNameNulPadded) {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("", "LongClassName\0NextClass\0"s, "");

  ManglingMap map;
  map.classNames["LongClassName"] = "Short";  // 5 < 13

  BinaryPatcher::patch(fs.slice, map);

  const uint8_t* ptr = fs.buffer.data();
  EXPECT_EQ(std::memcmp(ptr, "Short", 5), 0);
  for (int i = 5; i < 14; ++i) {
    EXPECT_EQ(ptr[i], 0u) << "Expected NUL padding at byte " << i;
  }
  ptr += 14;
  EXPECT_STREQ(reinterpret_cast<const char*>(ptr), "NextClass");
}

TEST(BinaryPatcher, PatchOrderMethTypeBeforeClassName) {
  // Methtype must be patched before classname is overwritten,
  // otherwise the methtype obfuscator can't find the original name.
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("", "MyClass\0"s, "v16@0:8@\"MyClass\"8\0"s);

  ManglingMap map;
  map.classNames["MyClass"] = "ObfClss";  // len 7

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  EXPECT_EQ(r.classPatches, 1u);
  EXPECT_EQ(r.methTypePatches, 1u);

  const char* classnamePtr = reinterpret_cast<const char*>(fs.buffer.data());
  EXPECT_STREQ(classnamePtr, "ObfClss");

  const char* methtypePtr = classnamePtr + 8;  // 7 chars + NUL
  EXPECT_STREQ(methtypePtr, "v16@0:8@\"ObfClss\"8");
}

TEST(BinaryPatcher, UnmatchedSymbolUnchanged) {
  using namespace std::string_literals;
  FakeSlice fs = makeFakeSlice("unmappedSelector\0"s, "", "");

  ManglingMap map;
  map.selectors["somethingElse"] = "xxxxxxxxxxxxx";

  PatchResult r = BinaryPatcher::patch(fs.slice, map);
  EXPECT_EQ(r.selectorPatches, 0u);

  const char* ptr = reinterpret_cast<const char*>(fs.buffer.data());
  EXPECT_STREQ(ptr, "unmappedSelector");
}

// ════════════════════════════════════════════════════════════════════
//  Integration
// ════════════════════════════════════════════════════════════════════

class BinaryPatcherIntegration : public ::testing::Test {
 protected:
  std::string dstPath;
  void TearDown() override {
    if (!dstPath.empty()) std::filesystem::remove(dstPath);
  }
};

TEST_F(BinaryPatcherIntegration, PatchRealBinary) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  dstPath = kBinaryPath + ".phase6_test_tmp";

  SymbolsCollector::Config cfg;
  cfg.obfuscablePaths = {kBinaryPath};
  ObfuscationSymbols symbols = SymbolsCollector::collect(cfg);

  CaesarMangler mangler(13);
  ManglingMap map = mangler.mangle(symbols);
  ASSERT_FALSE(map.selectors.empty());
  ASSERT_FALSE(map.classNames.empty());

  PatchResult result = BinaryPatcher::patchFile(kBinaryPath, dstPath, map);
  EXPECT_GT(result.selectorPatches, 0u);
  EXPECT_GT(result.classPatches, 0u);

  RecordProperty("selector_patches", std::to_string(result.selectorPatches));
  RecordProperty("class_patches", std::to_string(result.classPatches));
  RecordProperty("methtype_patches", std::to_string(result.methTypePatches));

  // Verify old names are gone from the patched binary
  MachOImage patched = loadMachOImage(dstPath);
  ASSERT_FALSE(patched.slices.empty());

  for (const auto& slice : patched.slices) {
    auto checkSection =
        [&](const MachSection* sec,
            const std::unordered_map<std::string, std::string>& orig2mangled) {
          if (!sec) return;
          uint64_t cursor = sec->fileOffset;
          uint64_t end = sec->fileOffset + sec->size;
          while (cursor < end) {
            const char* ptr =
                reinterpret_cast<const char*>(slice.data + cursor);
            size_t len = strnlen(ptr, static_cast<size_t>(end - cursor));
            if (len > 0) {
              std::string found(ptr, len);
              EXPECT_EQ(orig2mangled.find(found), orig2mangled.end())
                  << "Original unobfuscated name still present: " << found;
            }
            cursor += len + 1;
          }
        };
    checkSection(slice.objcMethNameSection(), map.selectors);
    checkSection(slice.objcClassNameSection(), map.classNames);
  }
}

TEST_F(BinaryPatcherIntegration, CaesarRoundtrip) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  std::string encryptedPath = kBinaryPath + ".phase6_enc_tmp";
  std::string restoredPath = kBinaryPath + ".phase6_rst_tmp";
  dstPath = encryptedPath;  // TearDown cleans at least one

  SymbolsCollector::Config cfg;
  cfg.obfuscablePaths = {kBinaryPath};
  ObfuscationSymbols symbols = SymbolsCollector::collect(cfg);

  ManglingMap forwardMap = CaesarMangler(13).mangle(symbols);
  BinaryPatcher::patchFile(kBinaryPath, encryptedPath, forwardMap);

  ManglingMap reverseMap;
  for (const auto& [orig, mangled] : forwardMap.selectors)
    reverseMap.selectors[mangled] = orig;
  for (const auto& [orig, mangled] : forwardMap.classNames)
    reverseMap.classNames[mangled] = orig;

  BinaryPatcher::patchFile(encryptedPath, restoredPath, reverseMap);

  auto readFile = [](const std::string& p) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(sz));
    return buf;
  };

  auto original = readFile(kBinaryPath);
  auto restored = readFile(restoredPath);
  EXPECT_EQ(original.size(), restored.size());
  EXPECT_EQ(original, restored);

  std::filesystem::remove(encryptedPath);
  std::filesystem::remove(restoredPath);
}
