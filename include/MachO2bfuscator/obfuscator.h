#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "MachO2bfuscator/binary_patcher.h"
#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/mangling_map.h"
#include "MachO2bfuscator/objc_extractor.h"
#include "MachO2bfuscator/symbol_sets.h"

// ═══════════════════════════════════════════════════════════════
//  ObfuscatorConfig
//
//  Mirrors Swift: struct Options
//
//  All inputs needed to run the full obfuscation pipeline.
//  Keeps the pipeline itself free of hard-coded policy.
// ═══════════════════════════════════════════════════════════════
struct ObfuscatorConfig {
  // ── Input / output ────────────────────────────────────────────

  // Paths to binaries we OWN and want to obfuscate.
  // Each binary is read from srcPath and written to the
  // corresponding dstPath.
  // Mirrors Swift: paths.obfuscableImages
  struct ImagePair {
    std::string srcPath;
    std::string dstPath;
  };
  std::vector<ImagePair> images;

  // Paths to binaries we DON'T own (system libs, external dylibs).
  // Their symbols are added to the blacklist.
  // Mirrors Swift: paths.unobfuscableDependencies
  std::vector<std::string> dependencyPaths;

  // ── Mangler ───────────────────────────────────────────────────

  // The mangler to use. Defaults to CaesarMangler(13) if null.
  // Mirrors Swift: mangler parameter in Obfuscator.init
  std::shared_ptr<IMangler> mangler;

  // ── Blacklist ─────────────────────────────────────────────────

  // Manual symbol blacklists — never obfuscate these.
  // Mirrors Swift: objcOptions.selectorsBlacklist / classesBlacklist
  std::unordered_set<std::string> manualSelectorBlacklist;
  std::unordered_set<std::string> manualClassBlacklist;

  // ── Erase options ─────────────────────────────────────────────

  // If true, NUL-out the entire __objc_methtype section after
  // patching. Removes method type information from the binary.
  // Mirrors Swift: options.eraseMethType
  bool eraseMethType = false;

  // If true, NUL-out the SYMTAB (symbol table) after patching.
  // Mirrors Swift: options.eraseSymtab
  bool eraseSymtab = true;

  // ── Dry run ───────────────────────────────────────────────────

  // If true, perform all analysis but do not write any output files.
  // Mirrors Swift: options.dryrun
  bool dryRun = false;

  // ── Verbosity ─────────────────────────────────────────────────
  bool verbose = false;
};

// ═══════════════════════════════════════════════════════════════
//  ObfuscatorStats — summary returned from run()
// ═══════════════════════════════════════════════════════════════
struct ObfuscatorStats {
  uint32_t imagesProcessed = 0;
  uint32_t selectorPatches = 0;
  uint32_t classPatches = 0;
  uint32_t methTypePatches = 0;
  uint32_t whitelistSelectors = 0;
  uint32_t whitelistClasses = 0;
  uint32_t blacklistSelectors = 0;
  uint32_t blacklistClasses = 0;
  uint32_t mangledSelectors = 0;
  uint32_t mangledClasses = 0;
};

// ═══════════════════════════════════════════════════════════════
//  ObfuscatorPipeline
//
//  Mirrors Swift: class Obfuscator
//
//  Wires the full pipeline:
//    Phase 1: load binary       (mach_reader)
//    Phase 3: extract ObjC      (objc_extractor)
//    Phase 4: build symbol sets (symbol_sets)
//    Phase 5: mangle symbols    (mangler)
//    Phase 6: patch binary      (binary_patcher)
// ═══════════════════════════════════════════════════════════════
class ObfuscatorPipeline {
 public:
  explicit ObfuscatorPipeline(ObfuscatorConfig config);

  // Run the full pipeline.
  // Mirrors Swift: Obfuscator.run()
  ObfuscatorStats run();

 private:
  ObfuscatorConfig config_;

  // ── Phase 4+5: collect symbols and build mangling map ─────────
  // Mirrors Swift: ObfuscationSymbols.buildFor() + mangler.mangleSymbols()
  ManglingMap buildManglingMap(ObfuscationSymbols& symbolsOut) const;

  // ── Phase 6 extras: erase sections ───────────────────────────
  // Mirrors Swift: image.eraseMethTypeSection() / image.eraseSymtab()
  static void eraseSectionIfPresent(MachOSlice& slice,
                                    const std::string& segName,
                                    const std::string& secName);
  static void eraseSymtab(MachOSlice& slice);

  void log(const std::string& msg) const;
};
