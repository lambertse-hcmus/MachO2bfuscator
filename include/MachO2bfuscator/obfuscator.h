#pragma once

#include <string>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/mangling_map.h"
#include "MachO2bfuscator/symbol_sets.h"
#include "config.h"

// ═══════════════════════════════════════════════════════════════════
//  ObfuscatorStats — summary returned from run()
// ═══════════════════════════════════════════════════════════════════
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

// ═══════════════════════════════════════════════════════════════════
//  ObfuscatorPipeline
// ═══════════════════════════════════════════════════════════════════
class ObfuscatorPipeline {
 public:
  explicit ObfuscatorPipeline(ObfuscatorConfig config);
  ObfuscatorStats run();

 private:
  ObfuscatorConfig config_;

  // Materialise a concrete IMangler from the spec fields if
  // config_.mangler is null. Called once at the start of run().
  void ensureMangler();

  ManglingMap buildManglingMap(ObfuscationSymbols& symbolsOut) const;

  static void eraseSectionIfPresent(MachOSlice& slice,
                                    const std::string& segName,
                                    const std::string& secName);
  static void eraseSymtab(MachOSlice& slice);
  void log(const std::string& msg) const;
};
