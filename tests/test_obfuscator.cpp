#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/obfuscator.h"

#ifdef TEST_OBJC_ABSOLUTE_PATH
static const std::string kBinaryPath = "assets/testckey_objc";
#else
#warning \
    "TEST_OBJC_ABSOLUTE_PATH is not defined. Some tests will be skipped that require a real binary."
static const std::string kBinaryPath = "";
#endif
namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("Cannot read: " + path);
  size_t sz = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<uint8_t> buf(sz);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
  return buf;
}

// ── Helpers: walk a NUL-separated string section ─────────────────────
// Calls cb(std::string) for every non-empty NUL-terminated string in [sec].
static void forEachString(const MachOSlice& slice, const MachSection& sec,
                          const std::function<void(const std::string&)>& cb) {
  uint64_t cursor = sec.fileOffset;
  uint64_t end = sec.fileOffset + sec.size;
  while (cursor < end) {
    const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
    size_t len = strnlen(ptr, static_cast<size_t>(end - cursor));
    if (len > 0) cb(std::string(ptr, len));
    cursor += len + 1;
  }
}

// ════════════════════════════════════════════════════════════════════
//  Unit tests — no binary required
// ════════════════════════════════════════════════════════════════════

TEST(ObfuscatorPipeline, EmptyConfigReturnsZeroStats) {
  ObfuscatorConfig cfg;
  ObfuscatorStats stats = ObfuscatorPipeline(cfg).run();

  EXPECT_EQ(stats.imagesProcessed, 0u);
  EXPECT_EQ(stats.selectorPatches, 0u);
  EXPECT_EQ(stats.classPatches, 0u);
}

TEST(ObfuscatorPipeline, DefaultManglerIsSet) {
  ObfuscatorConfig cfg;
  EXPECT_EQ(cfg.mangler, nullptr);

  ObfuscatorStats stats = ObfuscatorPipeline(cfg).run();
  EXPECT_EQ(stats.imagesProcessed, 0u);
}

// ════════════════════════════════════════════════════════════════════
//  Integration tests — fixture cleans up temp files
// ════════════════════════════════════════════════════════════════════

class ObfuscatorIntegration : public ::testing::Test {
 protected:
  std::vector<std::string> tmpFiles_;

  std::string tmpPath(const std::string& suffix) {
    std::string p = kBinaryPath + suffix;
    tmpFiles_.push_back(p);
    return p;
  }

  void TearDown() override {
    for (const auto& p : tmpFiles_) fs::remove(p);
  }
};

TEST_F(ObfuscatorIntegration, DryRunDoesNotWriteFile) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  std::string dstPath = tmpPath(".phase7_dryrun_tmp");
  fs::remove(dstPath);

  ObfuscatorConfig cfg;
  cfg.images.push_back({kBinaryPath, dstPath});
  cfg.dryRun = true;

  ObfuscatorStats stats = ObfuscatorPipeline(cfg).run();

  EXPECT_EQ(stats.imagesProcessed, 1u) << "dryRun must still count the image";
  EXPECT_FALSE(fs::exists(dstPath)) << "dryRun must not create the output file";
}

TEST_F(ObfuscatorIntegration, BasicPipelineProducesObfuscatedBinary) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  std::string dstPath = tmpPath(".phase7_basic_tmp");

  ObfuscatorConfig cfg;
  cfg.images.push_back({kBinaryPath, dstPath});
  cfg.mangler = std::make_shared<CaesarMangler>(13);

  ObfuscatorStats stats = ObfuscatorPipeline(cfg).run();

  EXPECT_EQ(stats.imagesProcessed, 1u);
  EXPECT_GT(stats.selectorPatches, 0u);
  EXPECT_GT(stats.classPatches, 0u);
  EXPECT_GT(stats.mangledSelectors, 0u);
  EXPECT_GT(stats.mangledClasses, 0u);
  EXPECT_GT(stats.whitelistSelectors, 0u);
  EXPECT_GT(stats.whitelistClasses, 0u);
  EXPECT_GT(stats.blacklistSelectors, 0u);

  ASSERT_TRUE(fs::exists(dstPath));
  EXPECT_EQ(fs::file_size(dstPath), fs::file_size(kBinaryPath));
  EXPECT_NE(readFile(kBinaryPath), readFile(dstPath));

  RecordProperty("selector_patches", std::to_string(stats.selectorPatches));
  RecordProperty("class_patches", std::to_string(stats.classPatches));
  RecordProperty("mangled_selectors", std::to_string(stats.mangledSelectors));
  RecordProperty("mangled_classes", std::to_string(stats.mangledClasses));
}

TEST_F(ObfuscatorIntegration, EraseMethTypeZerosOutSection) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  std::string dstPath = tmpPath(".phase7_erasemethtype_tmp");

  ObfuscatorConfig cfg;
  cfg.images.push_back({kBinaryPath, dstPath});
  cfg.mangler = std::make_shared<CaesarMangler>(13);
  cfg.eraseMethType = true;

  EXPECT_EQ(ObfuscatorPipeline(cfg).run().imagesProcessed, 1u);

  MachOImage patched = loadMachOImage(dstPath);
  for (const auto& slice : patched.slices) {
    const MachSection* sec = slice.objcMethTypeSection();
    if (!sec || sec->size == 0) continue;
    const uint8_t* ptr = slice.data + sec->fileOffset;
    for (uint64_t i = 0; i < sec->size; ++i) {
      EXPECT_EQ(ptr[i], 0u)
          << "__objc_methtype byte " << i << " not NUL after erase";
    }
  }
}

TEST_F(ObfuscatorIntegration, SourceBinaryUnchanged) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  std::string dstPath = tmpPath(".phase7_srccheck_tmp");
  auto originalBytes = readFile(kBinaryPath);

  ObfuscatorConfig cfg;
  cfg.images.push_back({kBinaryPath, dstPath});
  cfg.mangler = std::make_shared<CaesarMangler>(13);
  ObfuscatorPipeline(cfg).run();

  EXPECT_EQ(originalBytes, readFile(kBinaryPath))
      << "Source binary was modified by the pipeline";
}

TEST_F(ObfuscatorIntegration, ManualBlacklistPreservesSelector) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  std::string dstPath = tmpPath(".phase7_blacklist_tmp");

  SymbolsCollector::Config scCfg;
  scCfg.obfuscablePaths = {kBinaryPath};
  ObfuscationSymbols symbols = SymbolsCollector::collect(scCfg);
  ASSERT_FALSE(symbols.whitelist.selectors.empty());

  std::string blacklistedSel = *symbols.whitelist.selectors.begin();

  ObfuscatorConfig cfg;
  cfg.images.push_back({kBinaryPath, dstPath});
  cfg.mangler = std::make_shared<CaesarMangler>(13);
  cfg.manualSelectorBlacklist = {blacklistedSel};
  ObfuscatorPipeline(cfg).run();

  MachOImage patched = loadMachOImage(dstPath);
  bool found = false;
  for (const auto& slice : patched.slices) {
    const MachSection* sec = slice.objcMethNameSection();
    if (!sec) continue;
    forEachString(slice, *sec, [&](const std::string& s) {
      if (s == blacklistedSel) found = true;
    });
  }
  EXPECT_TRUE(found) << "Blacklisted selector '" << blacklistedSel
                     << "' not found unchanged in patched binary";
}

// ════════════════════════════════════════════════════════════════════
//  NEW: Obfuscated naming conventions
//
//  Uses RandomMangler because it GUARANTEES:
//    - class names  → first char A-Z  (upper)
//    - method names → first char a-z  (lower)
//  CaesarMangler shifts bytes arithmetically and makes no such promise.
//
//  Strategy:
//    1. Collect the mangling map so we know which names were actually
//       obfuscated (whitelist → mangled name).
//    2. Run the full pipeline to produce the output binary.
//    3. Walk __objc_classname and __objc_methname in the output binary.
//    4. For every string that IS a mangled value (i.e. it appears as a
//       value in the map), assert the case rule holds.
//       Strings that were NOT obfuscated (blacklisted / not in map) are
//       intentionally skipped — we don't own their names.
// ════════════════════════════════════════════════════════════════════
TEST_F(ObfuscatorIntegration,
       RandomManglerObfuscatedNamesFollowCaseConvention) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  std::string dstPath = tmpPath(".phase7_case_convention_tmp");

  // ── Step 1: build the mangling map independently ─────────────
  SymbolsCollector::Config scCfg;
  scCfg.obfuscablePaths = {kBinaryPath};
  ObfuscationSymbols symbols = SymbolsCollector::collect(scCfg);

  const uint32_t kSeed = 42;
  RandomMangler mangler(kSeed);
  ManglingMap map = mangler.mangle(symbols);

  ASSERT_FALSE(map.classNames.empty()) << "No classes were mangled";
  ASSERT_FALSE(map.selectors.empty()) << "No selectors were mangled";

  // Build reverse-lookup sets: mangled value → true
  // These are the names we expect to find in the binary.
  std::unordered_set<std::string> mangledClassValues;
  for (const auto& [orig, mangled] : map.classNames)
    mangledClassValues.insert(mangled);

  std::unordered_set<std::string> mangledSelectorValues;
  for (const auto& [orig, mangled] : map.selectors)
    mangledSelectorValues.insert(mangled);

  // ── Step 2: run the pipeline ──────────────────────────────────
  ObfuscatorConfig cfg;
  cfg.images.push_back({kBinaryPath, dstPath});
  cfg.mangler = std::make_shared<RandomMangler>(kSeed);

  ObfuscatorStats stats = ObfuscatorPipeline(cfg).run();
  ASSERT_EQ(stats.imagesProcessed, 1u);
  ASSERT_GT(stats.classPatches, 0u);
  ASSERT_GT(stats.selectorPatches, 0u);

  // ── Step 3 & 4: walk the patched binary and check case rules ──
  MachOImage patched = loadMachOImage(dstPath);
  ASSERT_FALSE(patched.slices.empty());

  uint32_t checkedClasses = 0;
  uint32_t checkedSelectors = 0;

  for (const auto& slice : patched.slices) {
    // ── Class names: every obfuscated name must start A-Z ─────
    const MachSection* classnameSec = slice.objcClassNameSection();
    if (classnameSec) {
      forEachString(slice, *classnameSec, [&](const std::string& name) {
        if (mangledClassValues.count(name) == 0)
          return;  // not obfuscated — skip
        EXPECT_FALSE(name.empty());
        EXPECT_TRUE(std::isupper(static_cast<unsigned char>(name[0])))
            << "Obfuscated class name does not start with uppercase: '" << name
            << "'";
        ++checkedClasses;
      });
    }

    // ── Method names: every obfuscated name must start a-z ────
    const MachSection* methnameSec = slice.objcMethNameSection();
    if (methnameSec) {
      forEachString(slice, *methnameSec, [&](const std::string& name) {
        if (mangledSelectorValues.count(name) == 0)
          return;  // not obfuscated — skip

        EXPECT_FALSE(name.empty());

        // Setter selectors always begin with "set" + uppercase — that is
        // correct ObjC convention and is not a violation of "lowercase first".
        // We only enforce a-z on non-setter mangled selectors.
        if (SymbolsCollector::isSetter(name)) {
          // "set" prefix must be intact
          EXPECT_EQ(name.substr(0, 3), "set")
              << "Mangled setter does not begin with 'set': '" << name << "'";
          EXPECT_EQ(name.back(), ':')
              << "Mangled setter does not end with ':': '" << name << "'";
        } else {
          EXPECT_TRUE(std::islower(static_cast<unsigned char>(name[0])))
              << "Obfuscated selector does not start with lowercase: '" << name
              << "'";
        }
        ++checkedSelectors;
      });
    }
  }

  // Sanity guard: we must have actually checked something.
  // If both are 0, the binary layout changed and the test is broken.
  EXPECT_GT(checkedClasses, 0u)
      << "No obfuscated class names were found in the patched binary — "
         "verify the binary path and that RandomMangler used the same seed";
  EXPECT_GT(checkedSelectors, 0u)
      << "No obfuscated selectors were found in the patched binary";

  RecordProperty("checked_classes", std::to_string(checkedClasses));
  RecordProperty("checked_selectors", std::to_string(checkedSelectors));
}
