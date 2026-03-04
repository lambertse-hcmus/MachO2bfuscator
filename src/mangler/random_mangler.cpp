#include <cctype>
#include <random>

#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"

RandomMangler::RandomMangler(uint32_t seed, uint32_t maxRetries)
    : seed_(seed), maxRetries_(maxRetries) {}

// ── randomName ────────────────────────────────────────────────────
// firstCharMode:
//   'u' → first char from A-Z  (26 chars) — for class names
//   'l' → first char from a-z  (26 chars) — for selectors
//   'a' → first char from a-zA-Z0-9 (62 chars) — unrestricted
//
// Remaining chars always from a-zA-Z0-9 (62 chars).
std::string RandomMangler::randomName(size_t byteLen, uint32_t& state,
                                      char firstCharMode) const {
  static constexpr char kAll[] =
      "abcdefghijklmnopqrstuvwxyz"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "0123456789";
  static constexpr char kLower[] = "abcdefghijklmnopqrstuvwxyz";
  static constexpr char kUpper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

  static constexpr size_t kNAll = 62;
  static constexpr size_t kNLower = 26;
  static constexpr size_t kNUpper = 26;

  // xorshift32 step
  auto advance = [](uint32_t s) -> uint32_t {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  };

  std::string result(byteLen, '\0');
  for (size_t i = 0; i < byteLen; ++i) {
    state = advance(state);
    if (i == 0) {
      switch (firstCharMode) {
        case 'u':
          result[i] = kUpper[state % kNUpper];
          break;
        case 'l':
          result[i] = kLower[state % kNLower];
          break;
        default:
          result[i] = kAll[state % kNAll];
          break;
      }
    } else {
      result[i] = kAll[state % kNAll];
    }
  }
  return result;
}

// ── getterFromSetter ─────────────────────────────────────────────
// "setTitle:"   → "title"
// "setIsHidden:" → "isHidden"
std::string RandomMangler::getterFromSetter(const std::string& setter) {
  if (!SymbolsCollector::isSetter(setter)) return "";

  // Drop "set" prefix and trailing ":"
  std::string getterPart = setter.substr(3, setter.size() - 4);

  // Lowercase the first letter
  if (!getterPart.empty()) {
    getterPart[0] =
        static_cast<char>(tolower(static_cast<unsigned char>(getterPart[0])));
  }
  return getterPart;
}

// ── setterFromGetter ─────────────────────────────────────────────
// "title" → "setTitle:"
std::string RandomMangler::setterFromGetter(const std::string& getter) {
  return SymbolsCollector::toSetterName(getter);
}

// ── RandomMangler::mangle ─────────────────────────────────────────
//
// Pipeline:
//   1. Mangle non-setter selectors  (random, first char a-z)
//   2. Derive setter mappings from getter mappings
//   3. Mangle class names           (random, first char A-Z)
ManglingMap RandomMangler::mangle(const ObfuscationSymbols& symbols) const {
  ManglingMap result;

  // Initialise PRNG state
  uint32_t state = seed_;
  if (state == 0) {
    std::random_device rd;
    state = rd();
    if (state == 0) state = 0xDEADBEEFu;  // fallback if rd returns 0
  }

  std::unordered_set<std::string> usedSelectors;
  for (const auto& s : symbols.blacklist.selectors) usedSelectors.insert(s);
  for (const auto& s : symbols.whitelist.selectors) usedSelectors.insert(s);

  std::unordered_set<std::string> usedClasses;
  for (const auto& c : symbols.blacklist.classes) usedClasses.insert(c);
  for (const auto& c : symbols.whitelist.classes) usedClasses.insert(c);

  // ── Step 1: mangle non-setter selectors ───────────────────────
  std::unordered_map<std::string, std::string> nonSetterMap;

  for (const auto& sel : symbols.whitelist.selectors) {
    if (SymbolsCollector::isSetter(sel)) continue;

    std::string mangled;
    bool found = false;

    for (uint32_t attempt = 0; attempt < maxRetries_; ++attempt) {
      // First char → a-z, remaining → a-zA-Z0-9
      mangled = randomName(sel.size(), state, 'l');

      // Restore colons at original positions —
      // ObjC multi-arg selectors like "foo:bar:" must keep
      // their colons at the same byte positions.
      for (size_t i = 0; i < sel.size(); ++i) {
        if (sel[i] == ':') mangled[i] = ':';
      }

      if (!usedSelectors.count(mangled)) {
        found = true;
        break;
      }
    }

    if (found) {
      nonSetterMap[sel] = mangled;
      result.selectors[sel] = mangled;
      usedSelectors.insert(mangled);
    }
    // If not found after maxRetries, silently skip.
    // Extremely unlikely for any reasonable symbol count.
  }

  // ── Step 2: derive setter mappings from getter mappings ───────
  //
  // For every whitelisted setter "setFoo:", find the getter "foo"
  // in nonSetterMap, then derive the mangled setter from the
  // mangled getter.
  //
  // Example:
  //   nonSetterMap["title"] = "xqrab"
  //   → result.selectors["setTitle:"] = "setXqrab:"
  for (const auto& sel : symbols.whitelist.selectors) {
    if (!SymbolsCollector::isSetter(sel)) continue;

    std::string getter = getterFromSetter(sel);
    if (getter.empty()) continue;

    auto it = nonSetterMap.find(getter);
    if (it == nonSetterMap.end()) continue;  // getter not in whitelist

    result.selectors[sel] = setterFromGetter(it->second);
  }

  // ── Step 3: mangle class names ────────────────────────────────
  // First char is forced to A-Z — ObjC class name convention.
  for (const auto& cls : symbols.whitelist.classes) {
    std::string mangled;
    bool found = false;

    for (uint32_t attempt = 0; attempt < maxRetries_; ++attempt) {
      // First char → A-Z, remaining → a-zA-Z0-9
      mangled = randomName(cls.size(), state, 'u');

      if (!usedClasses.count(mangled)) {
        found = true;
        break;
      }
    }

    if (found) {
      result.classNames[cls] = mangled;
      usedClasses.insert(mangled);
    }
  }

  return result;
}
