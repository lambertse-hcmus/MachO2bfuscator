#pragma once

#include <string>
#include <unordered_map>

struct ManglingMap {
  // Selector name mappings: "viewDidLoad" → "xQr7mNpKw2A"
  std::unordered_map<std::string, std::string> selectors;

  // Class name mappings:  "MyViewController" → "AbcDefGhiJkl"
  std::unordered_map<std::string, std::string> classNames;

  bool empty() const { return selectors.empty() && classNames.empty(); }
};
