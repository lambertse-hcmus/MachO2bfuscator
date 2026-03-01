#pragma once

#include <string>
#include <unordered_set>
#include <vector>


// ═══════════════════════════════════════════════════════════════
//  ObjCSymbolSets
//
//  A flat set of selector names and class names.
//  Used for both whitelist and blacklist.
// ═══════════════════════════════════════════════════════════════
struct ObjCSymbolSets {
  std::unordered_set<std::string> selectors;
  std::unordered_set<std::string> classes;

  // Merge another set into this one (union in-place)
  void mergeFrom(const ObjCSymbolSets& other);

  bool empty() const { return selectors.empty() && classes.empty(); }
};

// ═══════════════════════════════════════════════════════════════
//  ObfuscationSymbols
//
//  The final result of Phase 4.
//  - whitelist:   symbols that ARE safe to obfuscate
//  - blacklist:   symbols that must NEVER be renamed
//  - removedList: user symbols that were removed because they
//                 appeared in the blacklist (useful for diagnostics)
// ═══════════════════════════════════════════════════════════════
struct ObfuscationSymbols {
  ObjCSymbolSets whitelist;    // obfuscate these
  ObjCSymbolSets blacklist;    // never touch these
  ObjCSymbolSets removedList;  // user symbols dropped by blacklist
};

// ═══════════════════════════════════════════════════════════════
//  SymbolsCollector
//
//  Takes a set of obfuscable binaries and unobfuscable dependencies,
//  extracts symbols from all of them, then computes:
//
//    whitelist = userSymbols - blacklist
//    blacklist = systemSymbols + libobjcSelectors + manualBlacklist

class SymbolsCollector {
 public:
  struct Config {
    std::vector<std::string> obfuscablePaths;
    std::vector<std::string> unobfuscablePaths;
    std::unordered_set<std::string> manualClassBlacklist;
    std::unordered_set<std::string> manualSelectorBlacklist;
  };

  static ObfuscationSymbols collect(const Config& config);
  static ObjCSymbolSets extractFromBinary(const std::string& path);

  // Public — also used by Phase 5 mangler for getter/setter consistency
  static std::string toSetterName(const std::string& getterName);
  static bool isSetter(const std::string& selector);
};
