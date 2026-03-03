#pragma once

#include <cstdint>
#include <string>

#include "MachO2bfuscator/mangling_map.h"
#include "MachO2bfuscator/symbol_sets.h"

enum ManglerType { Caesar = 0, Random, RealWords };

class IMangler {
 public:
  virtual ~IMangler() = default;
  virtual ManglingMap mangle(const ObfuscationSymbols& symbols) const = 0;
};

class CaesarMangler : public IMangler {
 public:
  // cypherKey=13 matches the Swift original
  explicit CaesarMangler(uint8_t cypherKey = 13);

  ManglingMap mangle(const ObfuscationSymbols& symbols) const override;
  std::string mangleString(const std::string& input) const;

 private:
  uint8_t cypherKey_;
  uint8_t encryptByte(uint8_t byte) const;
};

class RandomMangler : public IMangler {
 public:
  // seed=0 → use std::random_device
  explicit RandomMangler(uint32_t seed = 0, uint32_t maxRetries = 1000);

  ManglingMap mangle(const ObfuscationSymbols& symbols) const override;
  std::string randomName(size_t byteLen, uint32_t& state,
                         char firstCharMode = 'a') const;

 private:
  uint32_t seed_;
  uint32_t maxRetries_;

  // "setTitle:" → "title"
  static std::string getterFromSetter(const std::string& setter);
  // "title" → "setTitle:"
  static std::string setterFromGetter(const std::string& getter);
};

class RealWordsMangler : public IMangler {
 public:
  // seed=0 → use std::random_device (non-deterministic each run)
  explicit RealWordsMangler(uint32_t seed = 0, uint32_t maxRetries = 20);

  ManglingMap mangle(const ObfuscationSymbols& symbols) const override;
  std::string generateSentence(size_t length, uint32_t& state,
                               bool firstUpper) const;

 private:
  uint32_t seed_;
  uint32_t maxRetries_;

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

ManglerType parseManglerType(const std::string& str);
std::string manglerTypeToString(ManglerType type);
