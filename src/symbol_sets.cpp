#include "MachO2bfuscator/symbol_sets.h"
#include "logger.h"

#include <stdexcept>

// ── ObjCSymbolSets::mergeFrom ─────────────────────────────────────
void ObjCSymbolSets::mergeFrom(const ObjCSymbolSets& other) {
  selectors.insert(other.selectors.begin(), other.selectors.end());
  classes.insert(other.classes.begin(), other.classes.end());
}

// ── SymbolsCollector::toSetterName ────────────────────────────────
// "title"    → "setTitle:"
// "isHidden" → "setIsHidden:"
// ""         → ""
std::string SymbolsCollector::toSetterName(const std::string& getterName) {
  if (getterName.empty()) return getterName;

  // Capitalise the first letter of the getter name
  std::string capitalised = getterName;
  capitalised[0] =
      static_cast<char>(toupper(static_cast<unsigned char>(capitalised[0])));

  return "set" + capitalised + ":";
}

// ── SymbolsCollector::isSetter ────────────────────────────────────
// Returns true if selector looks like "setXxx:"
bool SymbolsCollector::isSetter(const std::string& selector) {
  // Must start with "set", have at least one more char that is uppercase,
  // and end with ":"
  if (selector.size() < 5) return false;  // "setX:" is 5 chars minimum
  if (selector.substr(0, 3) != "set") return false;
  if (!isupper(static_cast<unsigned char>(selector[3]))) return false;
  if (selector.back() != ':') return false;
  return true;
}

// ── SymbolsCollector::extractFromBinary ───────────────────────────
//
// Loads a binary, extracts all slices, and collects selectors
// and class names from each slice.
ObjCSymbolSets SymbolsCollector::extractFromBinary(const std::string& path) {
  ObjCSymbolSets result;

  MachOImage image = loadMachOImage(path);

  for (const auto& slice : image.slices) {
    // ── Selectors ─────────────────────────────────────────────
    // Read directly from __objc_methname section.
    auto selectors = ObjcExtractor::extractSelectors(slice);
    for (const auto& s : selectors) {
      if (!s.value.empty()) {
        result.selectors.insert(s.value);
      }
    }

    // ── Class names ───────────────────────────────────────────
    // Walk ObjC metadata and collect obfuscatable class names.
    ObjcMetadata meta = ObjcExtractor::extractMetadata(slice);
    auto classNames = ObjcExtractor::collectClassNames(slice, meta);
    for (const auto& cn : classNames) {
      if (!cn.value.empty()) {
        result.classes.insert(cn.value);
      }
    }
  }

  return result;
}

// ── SymbolsCollector::collect ─────────────────────────────────────
//
// Pipeline:
//   1. Extract symbols from all OBFUSCABLE binaries  → userSymbols
//   2. Extract symbols from all UNOBFUSCABLE binaries → systemSymbols
//   3. Build blacklist = systemSymbols
//                      + setters of blacklisted getters
//                      + libobjcSelectors
//                      + manualBlacklist
//   4. whitelist  = userSymbols - blacklist
//   5. removedList = userSymbols ∩ blacklist (for diagnostics)
ObfuscationSymbols SymbolsCollector::collect(const Config& config) {
  // ── Step 1: user symbols ──────────────────────────────────────
  ObjCSymbolSets userSymbols;
  for (const auto& path : config.obfuscablePaths) {
    try {
      userSymbols.mergeFrom(extractFromBinary(path));
    } catch (const std::exception& e) {
      LOGGER_WARN("Warning: failed to extract from obfuscable '{}': {}", path,
                  e.what());
    }
  }

  // ── Step 2: system symbols ────────────────────────────────────
  ObjCSymbolSets systemSymbols;
  for (const auto& path : config.unobfuscablePaths) {
    try {
      systemSymbols.mergeFrom(extractFromBinary(path));
    } catch (const std::exception& e) {
      LOGGER_WARN("Warning: failed to extract from dependency '{}': {}", path,
                  e.what());
    }
  }

  // ── Step 3: build blacklist ───────────────────────────────────

  ObjCSymbolSets blacklist;

  // 3a. system symbols
  blacklist.mergeFrom(systemSymbols);

  // 3b. libobjc built-in selectors
  for (const auto& sel : ObjcExtractor::libobjcSelectors()) {
    blacklist.selectors.insert(sel);
  }

  // 3c. manual blacklists
  for (const auto& sel : config.manualSelectorBlacklist) {
    blacklist.selectors.insert(sel);
  }
  for (const auto& cls : config.manualClassBlacklist) {
    blacklist.classes.insert(cls);
  }

  // 3d. Auto-generate setters for ALL blacklisted getters.
  //     ↑ Must run AFTER all sources are merged into blacklist,
  //       so it covers system symbols + libobjc + manual entries.
  // Mirrors Swift: blacklistSetters = blackListGetters.map { $0.asSetter }
  {
    std::vector<std::string> settersToAdd;
    for (const auto& sel : blacklist.selectors) {
      if (!isSetter(sel)) {
        settersToAdd.push_back(toSetterName(sel));
      }
    }
    for (auto& s : settersToAdd) {
      blacklist.selectors.insert(std::move(s));
    }
  }

  // ── Step 4: whitelist = userSymbols - blacklist ───────────────
  ObjCSymbolSets whitelist;
  for (const auto& sel : userSymbols.selectors) {
    if (!blacklist.selectors.count(sel)) whitelist.selectors.insert(sel);
  }
  for (const auto& cls : userSymbols.classes) {
    if (!blacklist.classes.count(cls)) whitelist.classes.insert(cls);
  }

  // ── Step 5: removedList = userSymbols ∩ blacklist ─────────────
  ObjCSymbolSets removedList;
  for (const auto& sel : userSymbols.selectors) {
    if (blacklist.selectors.count(sel)) removedList.selectors.insert(sel);
  }
  for (const auto& cls : userSymbols.classes) {
    if (blacklist.classes.count(cls)) removedList.classes.insert(cls);
  }

  return ObfuscationSymbols{std::move(whitelist), std::move(blacklist),
                            std::move(removedList)};
}
