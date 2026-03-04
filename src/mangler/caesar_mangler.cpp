#include <cctype>
#include <stdexcept>

#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"

CaesarMangler::CaesarMangler(uint8_t cypherKey) : cypherKey_(cypherKey) {}

// ── encryptByte ───────────────────────────────────────────────────
//
// Printable ASCII range: [33 ('!'), 126 ('~')] — 94 characters total.
// ':' (0x3A = 58) is always returned unchanged —
// it is the ObjC selector argument separator.
uint8_t CaesarMangler::encryptByte(uint8_t byte) const {
  // Preserve colon — ObjC selector argument separator
  if (byte == 0x3A) return byte;

  constexpr uint8_t rangeStart = 33;
  constexpr uint8_t rangeEnd = 126;
  constexpr uint8_t rangeCount = rangeEnd - rangeStart + 1;  // 94

  if (byte >= rangeStart && byte <= rangeEnd) {
    uint8_t shifted = byte + cypherKey_;
    return (shifted <= rangeEnd) ? shifted : shifted - rangeCount;
  }

  return byte;
}

// ── mangleString ────────────────────────────────────────────────
//
// Setter-aware: if the string starts with "set", preserve the
// "set" prefix and only encrypt the getter part.
//   "setTitle:" → "set" + encrypt("Title:")
//   "viewDidLoad" → encrypt("viewDidLoad")
std::string CaesarMangler::mangleString(const std::string& input) const {
  if (input.size() > 3 && input[0] == 's' && input[1] == 'e' &&
      input[2] == 't') {
    std::string suffix = input.substr(3);
    std::string mangledSuffix = mangleString(suffix);
    return "set" + mangledSuffix;
  }

  std::string result = input;
  for (auto& ch : result) {
    ch = static_cast<char>(encryptByte(static_cast<uint8_t>(ch)));
  }
  return result;
}

// ── CaesarMangler::mangle ─────────────────────────────────────────
//
ManglingMap CaesarMangler::mangle(const ObfuscationSymbols& symbols) const {
  ManglingMap result;

  // Mangle selectors
  for (const auto& sel : symbols.whitelist.selectors) {
    result.selectors[sel] = mangleString(sel);
  }

  // Mangle class names
  for (const auto& cls : symbols.whitelist.classes) {
    result.classNames[cls] = mangleString(cls);
  }

  for (const auto& [orig, mangled] : result.selectors) {
    if (symbols.blacklist.selectors.count(mangled)) {
      throw std::runtime_error("CaesarMangler: mangled selector '" + orig +
                               "' → '" + mangled + "' clashes with blacklist");
    }
  }
  for (const auto& [orig, mangled] : result.classNames) {
    if (symbols.blacklist.classes.count(mangled)) {
      throw std::runtime_error("CaesarMangler: mangled class '" + orig +
                               "' → '" + mangled + "' clashes with blacklist");
    }
  }

  return result;
}
