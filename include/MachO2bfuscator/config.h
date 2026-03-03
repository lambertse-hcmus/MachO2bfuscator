#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "MachO2bfuscator/mangler.h"
// ═══════════════════════════════════════════════════════════════════
//  ObfuscatorConfig  — unified configuration for the full pipeline.
//
//  Two usage paths share this single struct:
//
//  A) Programmatic / test path
//     Set `mangler` directly. The mangler-spec fields (manglerType,
//     caesarKey, randomSeed) are ignored when mangler != nullptr.
//
//       ObfuscatorConfig cfg;
//       cfg.images.push_back({"in", "out"});
//       cfg.mangler = std::make_shared<CaesarMangler>(13);
//       ObfuscatorPipeline(cfg).run();
//
//  B) CLI path  (src/cli_parser.cpp)
//     Leave `mangler` null. Set manglerType / caesarKey / randomSeed.
//     The pipeline constructs the mangler from those fields on first run.
//
//       ObfuscatorConfig cfg = parseArgs(argc, argv);
//       ObfuscatorPipeline(cfg).run();
//
//  Mirrors Swift: struct Options + Obfuscator.init parameters
// ═══════════════════════════════════════════════════════════════════
struct ObfuscatorConfig {
  // ── Input / output ──────────────────────────────────────────────
  struct ImagePair {
    std::string srcPath;
    std::string dstPath;
  };
  // Binaries to obfuscate. Each is read from srcPath, patched,
  // and written to dstPath. In-place: srcPath == dstPath.
  // Mirrors Swift: paths.obfuscableImages
  std::vector<ImagePair> images;

  // Binaries we don't own — their symbols go into the blacklist.
  // Mirrors Swift: paths.unobfuscableDependencies
  std::vector<std::string> dependencyPaths;

  // ── Mangler — programmatic path ─────────────────────────────────
  // Set this directly to bypass the spec fields below.
  // Defaults to null; the pipeline falls back to manglerType/key/seed.
  // Mirrors Swift: mangler parameter in Obfuscator.init
  std::shared_ptr<IMangler> mangler;

  // ── Mangler — spec (used only when mangler == nullptr) ──────────
  // "caesar" or "random"  (default: "random")
  std::string manglerType = "realwords";
  // Shift key for CaesarMangler, 1–25  (default: 13)
  uint8_t caesarKey = 13;
  // Seed for RandomMangler.  0 = use std::random_device each run.
  uint32_t randomSeed = 0;

  // ── Blacklist ────────────────────────────────────────────────────
  // Symbols that must never be obfuscated, regardless of whitelist.
  // Mirrors Swift: objcOptions.selectorsBlacklist / classesBlacklist
  std::unordered_set<std::string> manualSelectorBlacklist;
  std::unordered_set<std::string> manualClassBlacklist;

  // ── Filter lists (optional) ───────────────────────────────────────
  // When non-empty, ONLY these symbols are obfuscated.
  // They are intersected with the whitelist (blacklisted symbols are still safe).
  // Populated from --class-filter-file / --selector-filter-file.
  std::unordered_set<std::string> classFilterList;     // empty = no filter
  std::unordered_set<std::string> selectorFilterList;  // empty = no filter

  // ── Erase options ────────────────────────────────────────────────
  // NUL-out __objc_methtype after patching.
  // Mirrors Swift: options.eraseMethType
  bool eraseMethType = false;

  // NUL-out the SYMTAB after patching.
  // Mirrors Swift: options.eraseSymtab
  bool eraseSymtab = true;

  // ── Misc ─────────────────────────────────────────────────────────
  // Analyse everything but write no output files.
  // Mirrors Swift: options.dryrun
  bool dryRun = false;
  bool verbose = false;
};


