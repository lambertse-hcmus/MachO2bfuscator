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

// ═══════════════════════════════════════════════════════════════
//  RealWordsMangler
//
//  Mirrors Swift: RealWordsMangler (EnglishSentenceGenerator)
//
//  Produces human-readable, camelCase obfuscated names by
//  concatenating real English words from the top-1000 word list
//  to exactly match the byte-length of the original symbol.
//
//  Rules (identical to the Swift original):
//   - The generated sentence is formed from random words whose
//     combined length equals the original symbol's byte length.
//   - For selectors: first word is lowercase, subsequent words
//     are capitalised on their first letter → looksLikeThis
//   - For class names: same algorithm, then the whole result is
//     capitalised on the first letter → LooksLikeThis
//   - Setters are DERIVED from getter mappings (same as
//     RandomMangler) to preserve getter/setter symmetry.
//   - Clash avoidance: retry up to maxRetries times per symbol.
//
//  Example output:
//   "AppDelegate"      → "MoveBeforeSet"  (12 chars, class)
//   "viewDidLoad"      → "turnMuchMean"   (11 chars, selector)
//   "setTitle:"        → "setTurnMuch:"   (derived from "turnMuch")
// ═══════════════════════════════════════════════════════════════
class RealWordsMangler : public IMangler {
 public:
  // seed=0 → use std::random_device (non-deterministic each run)
  // seed>0 → deterministic, reproducible
  explicit RealWordsMangler(uint32_t seed = 0, uint32_t maxRetries = 20);

  ManglingMap mangle(const ObfuscationSymbols& symbols) const override;

  // Public for testing — generate one sentence of exactly `length` bytes.
  // firstUpper=true  → capitalise first letter (class name style)
  // firstUpper=false → leave first letter lowercase (selector style)
  // Returns empty string if generation fails after maxRetries attempts.
  std::string generateSentence(size_t length, uint32_t& state,
                               bool firstUpper) const;

 private:
  uint32_t seed_;
  uint32_t maxRetries_;

  // Mirrors Swift: String.getterFromSetter / setterFromGetter
  static std::string getterFromSetter(const std::string& setter);
  static std::string setterFromGetter(const std::string& getter);

  // Picks a random word of exactly `length` chars from the word list.
  // Returns empty string if no word of that length exists.
  static const std::string& randomWordOfLength(size_t length, uint32_t& state);

  // Returns a random multi-letter word (length >= 2).
  static const std::string& randomMultiletterWord(uint32_t& state);

  // xorshift32 — same PRNG used in RandomMangler for consistency
  static uint32_t advance(uint32_t state);
};
