#include "MachO2bfuscator/obfuscator.h"

#include <mach-o/nlist.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <fstream>
#include <stdexcept>

#include "MachO2bfuscator/binary_patcher.h"
#include "logger.h"

namespace {
const void stripMemory(void* data, size_t size) {
  if (data && size > 0) {
    std::memset(data, 0, size);
  }
}
}  // namespace

ObfuscatorPipeline::ObfuscatorPipeline(ObfuscatorConfig config)
    : config_(std::move(config)) {
  assert(config_.mangler != nullptr);
}

// ── eraseSectionIfPresent ─────────────────────────────────────────
void ObfuscatorPipeline::eraseSectionIfPresent(MachOSlice& slice,
                                               const std::string& segName,
                                               const std::string& secName) {
  const MachSection* sec = slice.findSection(segName, secName);
  if (!sec || sec->size == 0) return;
  if (sec->fileOffset + sec->size > slice.dataSize) return;

  stripMemory(slice.data + sec->fileOffset, static_cast<size_t>(sec->size));
}

// ── eraseSymtab ───────────────────────────────────────────────────
//
// NUL-fills both the symbol table entries and the string table.
// The LC_SYMTAB load command itself is left intact.
void ObfuscatorPipeline::eraseSymtab(MachOSlice& slice) {
  if (!slice.symtab) return;
  const MachSymtab& st = *slice.symtab;
  const static size_t nlist_size =
      std::max(sizeof(struct nlist), sizeof(struct nlist_64));

  // Erase symbol entries
  if (st.symOff + (st.nSyms * nlist_size) <= slice.dataSize) {
    stripMemory(slice.data + st.symOff,
                static_cast<size_t>(st.nSyms) * nlist_size);
  }

  // Erase string table
  if (st.strTable.offset + st.strTable.size <= slice.dataSize) {
    stripMemory(slice.data + st.strTable.offset,
                static_cast<size_t>(st.strTable.size));
  }
}

// ── buildManglingMap ──────────────────────────────────────────────
ManglingMap ObfuscatorPipeline::buildManglingMap(
    ObfuscationSymbols& symbolsOut) const {
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
      LOGGER_VERBOSE("  whitelist selector: {}", sel);
    }
    for (const auto& cls : symbolsOut.whitelist.classes) {
      LOGGER_VERBOSE("  whitelist class: {}", cls);
    }
    for (const auto& sel : symbolsOut.blacklist.selectors) {
      LOGGER_VERBOSE("  blacklist selector: {}", sel);
    }
    for (const auto& cls : symbolsOut.blacklist.classes) {
      LOGGER_VERBOSE("  blacklist class: {}", cls);
    }
  }

  // Collect mangled names for all obfuscable symbols
  ManglingMap map = config_.mangler->mangle(symbolsOut);

  // Apply explicit filter lists ────────────────────
  if (!config_.classFilterList.empty()) {
    std::unordered_map<std::string, std::string> filtered;
    for (const auto& [orig, mangled] : map.classNames) {
      if (config_.classFilterList.count(orig)) {
        filtered[orig] = mangled;
      }
    }
    LOGGER_VERBOSE("class-filter applied: {} → {} classes",
                   map.classNames.size(), filtered.size());
    map.classNames = std::move(filtered);
  }

  if (!config_.selectorFilterList.empty()) {
    std::unordered_map<std::string, std::string> filtered;
    for (const auto& [orig, mangled] : map.selectors) {
      if (config_.selectorFilterList.count(orig)) {
        filtered[orig] = mangled;
      }
    }
    LOGGER_VERBOSE("selector-filter applied: {} → {} selectors",
                   map.selectors.size(), filtered.size());
    map.selectors = std::move(filtered);
  }

  LOGGER_INFO("mangled: {} selectors, {} classes", map.selectors.size(),
              map.classNames.size());
  return map;
}

// ── run ───────────────────────────────────────────────────────────
// Full pipeline:
//   1. Collect symbols from all images    (Phase 4)
//   2. Build mangling map                 (Phase 5)
//   3. For each image:
//      a. Patch symbols                   (Phase 6)
//      b. Optionally erase methtype       (Phase 6 extra)
//      c. Optionally erase symtab         (Phase 6 extra)
//      d. Write to dstPath
ObfuscatorStats ObfuscatorPipeline::run() {
  ObfuscatorStats stats;

  if (config_.images.empty()) {
    LOGGER_INFO("No images to obfuscate.");
    return stats;
  }

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
    LOGGER_WARN("Mangling map is empty — nothing to patch.");
    return stats;
  }

  // Patch each image ─────────────────────────────────
  for (const auto& img : config_.images) {
    LOGGER_INFO("Obfuscating: {}", img.srcPath);

    try {
      if (config_.dryRun) {
        // Dry run: analyse only, do not write
        LOGGER_INFO("  [dry run] skipping write to {}", img.dstPath);
        ++stats.imagesProcessed;
        continue;
      }

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
          LOGGER_INFO("  erased symbol table");
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
      LOGGER_WARN("Failed to obfuscate '{}': {}", img.srcPath, e.what());
    }
  }

  LOGGER_INFO("Done. {} image(s) obfuscated",
              std::to_string(stats.imagesProcessed));

  return stats;
}
