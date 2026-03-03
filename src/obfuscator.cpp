#include "MachO2bfuscator/obfuscator.h"

#include <fstream>
#include <random>
#include <stdexcept>

#include "MachO2bfuscator/binary_patcher.h"
#include "MachO2bfuscator/mangler.h"
#include "logger.h"

// ── ObfuscatorPipeline ────────────────────────────────────────────

ObfuscatorPipeline::ObfuscatorPipeline(ObfuscatorConfig config)
    : config_(std::move(config)) {
  if (!config_.mangler) {
    config_.mangler = std::make_shared<RealWordsMangler>();
  }
}

// ── eraseSectionIfPresent ─────────────────────────────────────────
//
// NUL-fills the content of the named section in the slice buffer.
// This is a destructive operation — it removes the section's data
// while leaving the section header intact (the binary structure is
// preserved, only the bytes are zeroed).
void ObfuscatorPipeline::eraseSectionIfPresent(MachOSlice& slice,
                                               const std::string& segName,
                                               const std::string& secName) {
  const MachSection* sec = slice.findSection(segName, secName);
  if (!sec || sec->size == 0) return;
  if (sec->fileOffset + sec->size > slice.dataSize) return;

  std::memset(slice.data + sec->fileOffset, 0, static_cast<size_t>(sec->size));
}

void ObfuscatorPipeline::ensureMangler() {
  if (config_.mangler) return;  // already set — programmatic path, do nothing

  if (config_.manglerType == ManglerType::Caesar) {
    config_.mangler =
        std::make_shared<CaesarMangler>(config_.manglerConfig.caesarKey);
  } else if (config_.manglerType == ManglerType::Random) {
    // "random" (default)
    uint32_t seed = config_.manglerConfig.randomSeed;
    if (seed == 0) seed = std::random_device{}();
    config_.mangler = std::make_shared<RandomMangler>(seed);
  } else if (config_.manglerType == ManglerType::RealWords) {
    config_.mangler = std::make_shared<RealWordsMangler>();
  } else {
    throw std::runtime_error("Unknown mangler type: " +
                             manglerTypeToString(config_.manglerType));
  }
}

// ── eraseSymtab ───────────────────────────────────────────────────
//
// NUL-fills both the symbol table entries and the string table.
// The LC_SYMTAB load command itself is left intact.
void ObfuscatorPipeline::eraseSymtab(MachOSlice& slice) {
  if (!slice.symtab) return;
  const MachSymtab& st = *slice.symtab;

  // Erase symbol entries
  if (st.symOff + (st.nSyms * 16) <= slice.dataSize) {
    // nlist_64 is 16 bytes; nlist is 12 bytes — use 16 as safe max
    std::memset(slice.data + st.symOff, 0, static_cast<size_t>(st.nSyms) * 16);
  }

  // Erase string table
  if (st.strTable.offset + st.strTable.size <= slice.dataSize) {
    std::memset(slice.data + st.strTable.offset, 0,
                static_cast<size_t>(st.strTable.size));
  }
}

// ── buildManglingMap ──────────────────────────────────────────────
//
// Phase 4: build whitelist/blacklist from all binaries
// Phase 5: mangle the whitelist
ManglingMap ObfuscatorPipeline::buildManglingMap(
    ObfuscationSymbols& symbolsOut) const {
  // ── Phase 4 ───────────────────────────────────────────────────
  SymbolsCollector::Config cfg;
  for (const auto& img : config_.images) {
    cfg.obfuscablePaths.push_back(img.srcPath);
  }
  cfg.unobfuscablePaths = config_.dependencyPaths;
  cfg.manualSelectorBlacklist = config_.manualSelectorBlacklist;
  cfg.manualClassBlacklist = config_.manualClassBlacklist;

  symbolsOut = SymbolsCollector::collect(cfg);

  LOGGER_INFO("whitelist: {} selectors, {} classes",
              symbolsOut.whitelist.selectors.size(),
              symbolsOut.whitelist.classes.size());
  LOGGER_INFO("blacklist: {} selectors, {} classes",
              symbolsOut.blacklist.selectors.size(),
              symbolsOut.blacklist.classes.size());
  if (logger::verboseAllowed() || config_.dryRun) {
    for (const auto& sel : symbolsOut.whitelist.selectors) {
      LOGGER_INFO("  whitelist selector: {}", sel);
    }
    for (const auto& cls : symbolsOut.whitelist.classes) {
      LOGGER_INFO("  whitelist class: {}", cls);
    }
    for (const auto& sel : symbolsOut.blacklist.selectors) {
      LOGGER_INFO("  blacklist selector: {}", sel);
    }
    for (const auto& cls : symbolsOut.blacklist.classes) {
      LOGGER_INFO("  blacklist class: {}", cls);
    }
  }

  // ── Phase 5 ───────────────────────────────────────────────────
  ManglingMap map = config_.mangler->mangle(symbolsOut);

  // ── Phase 5.5: apply explicit filter lists ────────────────────
  // If --class-filter-file was provided, keep only class entries
  // whose original name appears in the filter set.
  // Same for --selector-filter-file.
  //
  // The blacklist was already applied by SymbolsCollector, so every
  // entry in `map` is already safe to rename. The filter only
  // narrows what we CHOOSE to rename — it cannot re-introduce a
  // blacklisted symbol.
  if (!config_.classFilterList.empty()) {
    std::unordered_map<std::string, std::string> filtered;
    for (const auto& [orig, mangled] : map.classNames) {
      if (config_.classFilterList.count(orig)) {
        filtered[orig] = mangled;
      }
    }
    LOGGER_INFO("class-filter applied: {} → {} classes", map.classNames.size(),
                filtered.size());
    map.classNames = std::move(filtered);
  }

  if (!config_.selectorFilterList.empty()) {
    std::unordered_map<std::string, std::string> filtered;
    for (const auto& [orig, mangled] : map.selectors) {
      if (config_.selectorFilterList.count(orig)) {
        filtered[orig] = mangled;
      }
    }
    LOGGER_INFO("selector-filter applied: {} → {} selectors",
                map.selectors.size(), filtered.size());
    map.selectors = std::move(filtered);
  }

  LOGGER_INFO("mangled: {} selectors, {} classes", map.selectors.size(),
              map.classNames.size());
  return map;
}

// ── run ───────────────────────────────────────────────────────────
//
// Full pipeline:
//   1. Collect symbols from all images    (Phase 4)
//   2. Build mangling map                 (Phase 5)
//   3. For each image:
//      a. Patch symbols                   (Phase 6)
//      b. Optionally erase methtype       (Phase 6 extra)
//      c. Optionally erase symtab         (Phase 6 extra)
//      d. Write to dstPath
ObfuscatorStats ObfuscatorPipeline::run() {
  ensureMangler();
  ObfuscatorStats stats;

  if (config_.images.empty()) {
    LOGGER_INFO("No images to obfuscate.");
    return stats;
  }

  // ── Phases 4+5: build the mangling map once for all images ────
  LOGGER_INFO("Collecting symbols...");
  ObfuscationSymbols symbols;
  ManglingMap map = buildManglingMap(symbols);

  stats.whitelistSelectors =
      static_cast<uint32_t>(symbols.whitelist.selectors.size());
  stats.whitelistClasses =
      static_cast<uint32_t>(symbols.whitelist.classes.size());
  stats.blacklistSelectors =
      static_cast<uint32_t>(symbols.blacklist.selectors.size());
  stats.blacklistClasses =
      static_cast<uint32_t>(symbols.blacklist.classes.size());
  stats.mangledSelectors = static_cast<uint32_t>(map.selectors.size());
  stats.mangledClasses = static_cast<uint32_t>(map.classNames.size());

  if (map.empty()) {
    LOGGER_INFO("Warning: mangling map is empty — nothing to patch.");
    return stats;
  }

  // ── Phase 6: patch each image ─────────────────────────────────
  for (const auto& img : config_.images) {
    LOGGER_INFO("Obfuscating: {}", img.srcPath);

    try {
      if (config_.dryRun) {
        // Dry run: analyse only, do not write
        LOGGER_INFO("  [dry run] skipping write to {}", img.dstPath);
        ++stats.imagesProcessed;
        continue;
      }

      // Load src into memory
      MachOImage image = loadMachOImage(img.srcPath);

      // Patch all slices in-place
      PatchResult patchResult;
      for (auto& slice : image.slices) {
        PatchResult r = BinaryPatcher::patch(slice, map);
        patchResult.selectorPatches += r.selectorPatches;
        patchResult.classPatches += r.classPatches;
        patchResult.methTypePatches += r.methTypePatches;

        // ── Erase methtype section ─────────────────────────
        if (config_.eraseMethType) {
          eraseSectionIfPresent(slice, "__TEXT", "__objc_methtype");
          LOGGER_INFO("  erased __objc_methtype");
        }

        // ── Erase SYMTAB ───────────────────────────────────
        if (config_.eraseSymtab) {
          eraseSymtab(slice);
        }
      }

      stats.selectorPatches += patchResult.selectorPatches;
      stats.classPatches += patchResult.classPatches;
      stats.methTypePatches += patchResult.methTypePatches;

      LOGGER_INFO("  patched: {} selectors, {} classes, {} methtypes",
                  patchResult.selectorPatches, patchResult.classPatches,
                  patchResult.methTypePatches);
      // Write patched rawData to dstPath
      std::ofstream out(img.dstPath, std::ios::binary | std::ios::trunc);
      if (!out) {
        throw std::runtime_error("Cannot open for writing: " + img.dstPath);
      }
      out.write(reinterpret_cast<const char*>(image.rawData),
                static_cast<std::streamsize>(image.rawDataSize));
      if (!out) {
        throw std::runtime_error("Write failed for: " + img.dstPath);
      }

      ++stats.imagesProcessed;
      LOGGER_INFO("  written to: {}", img.dstPath);

    } catch (const std::exception& e) {
      LOGGER_WARN("Warning: Failed to obfuscate '{}': {}", img.srcPath,
                  e.what());
    }
  }

  LOGGER_INFO("Done. {} image(s) obfuscated",
              std::to_string(stats.imagesProcessed));

  return stats;
}
