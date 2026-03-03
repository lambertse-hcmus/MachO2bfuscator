#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "MachO2bfuscator/mangler.h"

struct ObfuscatorConfig {
  struct ImagePair {
    std::string srcPath;
    std::string dstPath;
  };
  std::vector<ImagePair> images;

  // Binaries we don't own — their symbols go into the blacklist.
  std::vector<std::string> dependencyPaths;

  std::shared_ptr<IMangler> mangler;
  ManglerType manglerType = ManglerType::RealWords;
  union {
    uint8_t caesarKey;
    uint32_t randomSeed;
  } manglerConfig;

  // ── Blacklist ────────────────────────────────────────────────────
  // Symbols that must never be obfuscated, regardless of whitelist.
  std::unordered_set<std::string> manualSelectorBlacklist;
  std::unordered_set<std::string> manualClassBlacklist;

  std::unordered_set<std::string> classFilterList;
  std::unordered_set<std::string> selectorFilterList;

  bool eraseMethType = false;
  bool eraseSymtab = true;

  // ── Misc ─────────────────────────────────────────────────────────
  // Analyse everything but write no output files.
  bool dryRun = false;
  int logLevel = 0;  // 0=info, 1=debug, 2=verbose
};
