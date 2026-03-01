#pragma once

#include <string>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════
//  ManglingMap
//
//  Note: we omit exportTrieObfuscationMap from the Swift original
//  because export trie patching is a separate concern handled
//  later. The core obfuscation only needs selectors + classNames.
// ═══════════════════════════════════════════════════════════════
struct ManglingMap {
  // Selector name mappings: "viewDidLoad" → "xQr7mNpKw2A"
  std::unordered_map<std::string, std::string> selectors;

  // Class name mappings:  "MyViewController" → "AbcDefGhiJkl"
  std::unordered_map<std::string, std::string> classNames;

  bool empty() const { return selectors.empty() && classNames.empty(); }
};
