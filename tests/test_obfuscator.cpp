#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/obfuscator.h"

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

const std::string objCPath =
    "/Users/tri.le/src/opensource/lambertse/MachO2bfuscator/assets/"
    "testckey_objc";
namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────

static std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) throw std::runtime_error("Cannot read: " + path);
  size_t sz = static_cast<size_t>(f.tellg());
  f.seekg(0);
  std::vector<uint8_t> buf(sz);
  f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
  return buf;
}

// ── Unit tests (no binary needed) ────────────────────────────────

void test_empty_config_returns_zero_stats() {
  // No images → nothing processed, no crash
  ObfuscatorConfig cfg;
  cfg.verbose = false;

  ObfuscatorPipeline pipeline(cfg);
  ObfuscatorStats stats = pipeline.run();

  ASSERT(stats.imagesProcessed == 0);
  ASSERT(stats.selectorPatches == 0);
  ASSERT(stats.classPatches == 0);
}

void test_default_mangler_is_set() {
  // If no mangler is provided, CaesarMangler(13) is used by default
  ObfuscatorConfig cfg;
  // mangler is null — pipeline must set a default
  ASSERT(cfg.mangler == nullptr);

  ObfuscatorPipeline pipeline(cfg);
  // After construction the pipeline must have a mangler
  // We verify indirectly: run() must not throw even with no images
  ObfuscatorStats stats = pipeline.run();
  ASSERT(stats.imagesProcessed == 0);  // no images, but no crash
}

void test_dry_run_does_not_write_file() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  std::string dstPath = path + ".phase7_dryrun_tmp";
  // Make sure dst does NOT exist before the test
  fs::remove(dstPath);

  ObfuscatorConfig cfg;
  cfg.images.push_back({path, dstPath});
  cfg.dryRun = true;
  cfg.verbose = false;

  ObfuscatorPipeline pipeline(cfg);
  ObfuscatorStats stats = pipeline.run();

  // Dry run must increment imagesProcessed (we analysed it)
  ASSERT(stats.imagesProcessed == 1);

  // But must NOT create the output file
  ASSERT(!fs::exists(dstPath));
}

// ── Integration tests ─────────────────────────────────────────────

void test_pipeline_basic() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  std::string dstPath = path + ".phase7_basic_tmp";

  try {
    ObfuscatorConfig cfg;
    cfg.images.push_back({path, dstPath});
    cfg.mangler = std::make_shared<CaesarMangler>(13);
    cfg.verbose = true;

    ObfuscatorPipeline pipeline(cfg);
    ObfuscatorStats stats = pipeline.run();

    // Basic sanity checks on stats
    ASSERT(stats.imagesProcessed == 1);
    ASSERT(stats.selectorPatches > 0);
    ASSERT(stats.classPatches > 0);
    ASSERT(stats.mangledSelectors > 0);
    ASSERT(stats.mangledClasses > 0);

    // Whitelist must be non-empty
    ASSERT(stats.whitelistSelectors > 0);
    ASSERT(stats.whitelistClasses > 0);

    // Blacklist must contain at least libobjc selectors
    ASSERT(stats.blacklistSelectors > 0);

    // Output file must exist
    ASSERT(fs::exists(dstPath));

    // Output file must have same size as input
    ASSERT(fs::file_size(dstPath) == fs::file_size(path));

    // Output must differ from input (something was patched)
    auto original = readFile(path);
    auto patched = readFile(dstPath);
    ASSERT(original != patched);

    std::cout << "\n    stats:"
              << "\n      images processed:   " << stats.imagesProcessed
              << "\n      selectors patched:  " << stats.selectorPatches
              << "\n      classes patched:    " << stats.classPatches
              << "\n      methtypes patched:  " << stats.methTypePatches
              << "\n      whitelist selectors:" << stats.whitelistSelectors
              << "\n      whitelist classes:  " << stats.whitelistClasses
              << "\n      blacklist selectors:" << stats.blacklistSelectors
              << "\n      mangled selectors:  " << stats.mangledSelectors
              << "\n      mangled classes:    " << stats.mangledClasses << "\n";

    fs::remove(dstPath);

  } catch (...) {
    fs::remove(dstPath);
    throw;
  }
}

void test_pipeline_erase_methtype() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  std::string dstPath = path + ".phase7_erasemethtype_tmp";

  try {
    ObfuscatorConfig cfg;
    cfg.images.push_back({path, dstPath});
    cfg.mangler = std::make_shared<CaesarMangler>(13);
    cfg.eraseMethType = true;
    cfg.verbose = false;

    ObfuscatorPipeline pipeline(cfg);
    ObfuscatorStats stats = pipeline.run();

    ASSERT(stats.imagesProcessed == 1);

    // Verify __objc_methtype section is all NUL in the output
    MachOImage patched = loadMachOImage(dstPath);
    for (const auto& slice : patched.slices) {
      const MachSection* sec = slice.objcMethTypeSection();
      if (!sec || sec->size == 0) continue;

      const uint8_t* ptr = slice.data + sec->fileOffset;
      bool allZero = true;
      for (uint64_t i = 0; i < sec->size; ++i) {
        if (ptr[i] != 0) {
          allZero = false;
          break;
        }
      }
      ASSERT(allZero);
    }

    fs::remove(dstPath);

  } catch (...) {
    fs::remove(dstPath);
    throw;
  }
}

void test_pipeline_src_unchanged() {
  // The source binary must never be modified
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  std::string dstPath = path + ".phase7_srccheck_tmp";

  auto originalBytes = readFile(path);

  try {
    ObfuscatorConfig cfg;
    cfg.images.push_back({path, dstPath});
    cfg.mangler = std::make_shared<CaesarMangler>(13);

    ObfuscatorPipeline pipeline(cfg);
    pipeline.run();

    // Source must be byte-for-byte identical to before
    auto afterBytes = readFile(path);
    ASSERT(originalBytes == afterBytes);

    fs::remove(dstPath);

  } catch (...) {
    fs::remove(dstPath);
    throw;
  }
}

void test_pipeline_manual_blacklist_respected() {
  // Symbols in the manual blacklist must not appear in the output
  // as mangled names — they must be left unchanged.
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  std::string dstPath = path + ".phase7_blacklist_tmp";

  try {
    // First: run without blacklist, collect what gets mangled
    ObfuscatorConfig cfg1;
    cfg1.images.push_back({path, path + ".phase7_bl_ref_tmp"});
    cfg1.mangler = std::make_shared<CaesarMangler>(13);
    cfg1.dryRun = true;  // just collect stats, don't write

    // We need the symbols to know what to blacklist
    SymbolsCollector::Config scCfg;
    scCfg.obfuscablePaths = {path};
    ObfuscationSymbols symbols = SymbolsCollector::collect(scCfg);

    // Pick one selector from the whitelist to blacklist
    if (symbols.whitelist.selectors.empty()) {
      throw std::runtime_error("No whitelisted selectors to test with");
    }
    std::string blacklistedSel = *symbols.whitelist.selectors.begin();

    // Now run with that selector blacklisted
    ObfuscatorConfig cfg2;
    cfg2.images.push_back({path, dstPath});
    cfg2.mangler = std::make_shared<CaesarMangler>(13);
    cfg2.manualSelectorBlacklist = {blacklistedSel};

    ObfuscatorPipeline pipeline(cfg2);
    pipeline.run();

    // The blacklisted selector must appear unchanged in the output
    MachOImage patched = loadMachOImage(dstPath);
    bool found = false;
    for (const auto& slice : patched.slices) {
      const MachSection* sec = slice.objcMethNameSection();
      if (!sec) continue;
      uint64_t cursor = sec->fileOffset;
      uint64_t end = sec->fileOffset + sec->size;
      while (cursor < end) {
        const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
        size_t len = strnlen(ptr, static_cast<size_t>(end - cursor));
        if (len > 0 && std::string(ptr, len) == blacklistedSel) {
          found = true;
          break;
        }
        cursor += len + 1;
      }
      if (found) break;
    }
    ASSERT(found);

    fs::remove(dstPath);

  } catch (...) {
    fs::remove(dstPath);
    throw;
  }
}

// ─────────────────────────────────────────────────────────────────
int main() {
  std::cout << "=== Phase 7 Tests: Obfuscator Pipeline ===\n\n";

  std::cout << "── Unit tests ──\n";
  RUN(test_empty_config_returns_zero_stats);
  RUN(test_default_mangler_is_set);
  RUN(test_dry_run_does_not_write_file);

  std::cout << "\n── Integration tests ──\n";
  RUN(test_pipeline_basic);
  RUN(test_pipeline_erase_methtype);
  RUN(test_pipeline_src_unchanged);
  RUN(test_pipeline_manual_blacklist_respected);

  std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed
            << " failed ===\n";
  return g_failed > 0 ? 1 : 0;
}
