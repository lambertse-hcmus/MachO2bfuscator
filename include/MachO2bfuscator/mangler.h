#pragma once

#include <cstdint>
#include <string>

#include "MachO2bfuscator/mangling_map.h"
#include "MachO2bfuscator/symbol_sets.h"

// ═══════════════════════════════════════════════════════════════
//  IMangler — interface
//
//  Mirrors Swift: protocol SymbolMangling
// ═══════════════════════════════════════════════════════════════
class IMangler {
 public:
  virtual ~IMangler() = default;
  virtual ManglingMap mangle(const ObfuscationSymbols& symbols) const = 0;
};

// ═══════════════════════════════════════════════════════════════
//  CaesarMangler
//
//  Mirrors Swift: CaesarMangler + CaesarStringMangler + CaesarCypher
//
//  Encrypts each symbol using a Caesar cipher with a fixed key.
//  Every byte in the printable ASCII range [33, 126] is shifted
//  by cypherKey, wrapping around within that range.
//
//  Special rules (matching the Swift original exactly):
//   1. The colon ':' (0x3A) is NEVER shifted — it is the ObjC
//      selector argument separator and must be preserved.
//   2. Setter selectors ("setFoo:") are handled by keeping "set"
//      as a prefix and only encrypting the getter part ("Foo:").
//      This preserves the setter/getter relationship.
//
//  Example with key=13:
//    "viewDidLoad"  → "ivrjQrqYbno"
//    "setTitle:"    → "setGrgyv:"   ("set" preserved, "Title:" shifted)
//    "MyClass"      → "ZlNynnf"
// ═══════════════════════════════════════════════════════════════
class CaesarMangler : public IMangler {
 public:
  // cypherKey=13 matches the Swift original
  explicit CaesarMangler(uint8_t cypherKey = 13);

  ManglingMap mangle(const ObfuscationSymbols& symbols) const override;

  // Public for testing — encrypts a single string
  std::string mangleString(const std::string& input) const;

 private:
  uint8_t cypherKey_;

  // Mirrors Swift: CaesarCypher.encrypt(element:key:)
  uint8_t encryptByte(uint8_t byte) const;
};

// ═══════════════════════════════════════════════════════════════
//  RandomMangler
//
//  Mirrors Swift: RealWordsMangler (simplified)
//
//  Generates a random replacement of the same byte-length as the
//  original symbol using a seeded xorshift32 PRNG.
//
//  Character set rules:
//   - Class names:  first char A-Z, remaining a-zA-Z0-9
//   - Selectors:    first char a-z, remaining a-zA-Z0-9
//   - Colons ':'    are always preserved at their original positions
//
//  Setter/getter consistency rule (mirrors Swift exactly):
//   - Non-setter selectors are randomised independently.
//   - Setter mappings are DERIVED from getter mappings:
//       getter "foo" → "xqr"  implies  setter "setFoo:" → "setXqr:"
//
//  Clash avoidance:
//   - A mangled name must not already exist in the blacklist
//     or in the set of other already-mangled names.
//   - If a random name clashes, we retry up to maxRetries times.
// ═══════════════════════════════════════════════════════════════
class RandomMangler : public IMangler {
 public:
  // seed=0 → use std::random_device
  explicit RandomMangler(uint32_t seed = 0, uint32_t maxRetries = 1000);

  ManglingMap mangle(const ObfuscationSymbols& symbols) const override;

  // Public for testing.
  // Generates a random name of exactly byteLen bytes.
  //  firstCharMode: 'u' → first char A-Z  (class names)
  //                 'l' → first char a-z  (selectors)
  //                 'a' → first char a-zA-Z0-9 (unrestricted)
  std::string randomName(size_t byteLen, uint32_t& state,
                         char firstCharMode = 'a') const;

 private:
  uint32_t seed_;
  uint32_t maxRetries_;

  // Mirrors Swift: String.getterFromSetter
  // "setTitle:" → "title"
  static std::string getterFromSetter(const std::string& setter);

  // Mirrors Swift: String.setterFromGetter
  // "title" → "setTitle:"
  static std::string setterFromGetter(const std::string& getter);
};
