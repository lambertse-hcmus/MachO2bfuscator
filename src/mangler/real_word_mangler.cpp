#include <algorithm>
#include <cctype>
#include <random>
#include <stdexcept>
#include <unordered_map>

#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"
#include "english_top_1000.h"

// ══════════════════════���════════════════════════════════════════
//  RealWordsMangler — word list
//
//  Source: https://www.talkenglish.com/vocabulary/top-1000-words.aspx
// ═══════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════
//  RealWordsMangler — implementation
// ═══════════════════════════════════════════════════════════════

// ── Precomputed lookup tables (built once, shared across instances) ──
namespace {

// Built once on first use via static-local initialisation.
struct WordTables {
  // words grouped by their exact character length
  std::unordered_map<size_t, std::vector<std::string>> byLength;
  // all words with length >= 2
  std::vector<std::string> multiLetter;

  WordTables() {
    for (const auto& w : kEnglishTop1000) {
      byLength[w.size()].push_back(w);
      if (w.size() >= 2) multiLetter.push_back(w);
    }
  }
};

const WordTables& wordTables() {
  static const WordTables tables;
  return tables;
}

}  // namespace

// ── xorshift32 ────────────────────────────────────────────────────
uint32_t RealWordsMangler::advance(uint32_t s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

// ── randomMultiletterWord ──────────────────────────────────────────
// Picks a random word with length >= 2. Always succeeds (list is non-empty).
const std::string& RealWordsMangler::randomMultiletterWord(uint32_t& state) {
  const auto& words = wordTables().multiLetter;
  state = advance(state);
  return words[state % words.size()];
}

// ── randomWordOfLength ────────────────────────────────────────────
// Returns a random word of exactly `length` chars.
// Returns an empty static string if no word of that length exists.
const std::string& RealWordsMangler::randomWordOfLength(size_t length,
                                                        uint32_t& state) {
  static const std::string kEmpty;
  const auto& tables = wordTables();
  auto it = tables.byLength.find(length);
  if (it == tables.byLength.end() || it->second.empty()) return kEmpty;
  state = advance(state);
  return it->second[state % it->second.size()];
}

// ── generateSentence ──────────────────────────────────────────────
//
// Concatenates random English words until the total byte length
// exactly matches `length`.
//
//   - First word is kept as-is (lowercase start for selectors).
//   - Every subsequent word has its first letter capitalised
//     → produces camelCase output e.g. "turnMuchMean"
//   - If firstUpper=true, the very first letter is capitalised too
//     → produces PascalCase output e.g. "TurnMuchMean" (class names)
//
// Returns empty string if generation fails (no word of remaining length).
std::string RealWordsMangler::generateSentence(size_t length, uint32_t& state,
                                               bool firstUpper) const {
  if (length == 0) return "";

  std::vector<std::string> words;
  size_t remaining = length;

  while (remaining > 0) {
    // Try a multi-letter word first; if it's too long, fall back to
    const std::string& candidate = randomMultiletterWord(state);
    const std::string* chosen = &candidate;

    if (chosen->size() > remaining) {
      const std::string& exact = randomWordOfLength(remaining, state);
      if (exact.empty()) return "";  // no word fits — give up
      chosen = &exact;
    }

    words.push_back(*chosen);
    remaining -= chosen->size();
  }

  // Apply camelCase capitalisation:
  //   word[0]  → kept lowercase (or uppercased if firstUpper=true)
  //   word[1+] → first letter uppercased
  //   randomWords.prefix(1) + randomWords.suffix(from:1).map {
  //   capitalizedOnFirstLetter }
  std::string result;
  result.reserve(length);
  for (size_t i = 0; i < words.size(); ++i) {
    std::string w = words[i];
    if (i == 0) {
      if (firstUpper && !w.empty())
        w[0] = static_cast<char>(toupper(static_cast<unsigned char>(w[0])));
    } else {
      if (!w.empty())
        w[0] = static_cast<char>(toupper(static_cast<unsigned char>(w[0])));
    }
    result += w;
  }
  return result;
}

// ── helper: getterFromSetter / setterFromGetter ───────────────────
std::string RealWordsMangler::getterFromSetter(const std::string& setter) {
  if (!SymbolsCollector::isSetter(setter)) return "";
  std::string p = setter.substr(3, setter.size() - 4);
  if (!p.empty())
    p[0] = static_cast<char>(tolower(static_cast<unsigned char>(p[0])));
  return p;
}

std::string RealWordsMangler::setterFromGetter(const std::string& getter) {
  return SymbolsCollector::toSetterName(getter);
}

// ── RealWordsMangler::mangle ──────────────────────────────────────
//
// Three-step pipeline (identical structure to RandomMangler and Swift):
//   1. Mangle non-setter selectors with camelCase English sentences
//   2. Derive setter mappings from getter mappings
//   3. Mangle class names with PascalCase English sentences
RealWordsMangler::RealWordsMangler(uint32_t seed, uint32_t maxRetries)
    : seed_(seed), maxRetries_(maxRetries) {}

ManglingMap RealWordsMangler::mangle(const ObfuscationSymbols& symbols) const {
  ManglingMap result;

  // Seed the PRNG — same logic as RandomMangler
  uint32_t state = seed_;
  if (state == 0) {
    std::random_device rd;
    state = rd();
    if (state == 0) state = 0xDEADBEEFu;
  }

  // ── Build used-name sets for clash avoidance ──────────────────
  //   (Array(blacklist.selectors) + Array(whitelist.selectors)).uniq
  std::unordered_set<std::string> usedSelectors;
  for (const auto& s : symbols.blacklist.selectors) usedSelectors.insert(s);
  for (const auto& s : symbols.whitelist.selectors) usedSelectors.insert(s);

  std::unordered_set<std::string> usedClasses;
  for (const auto& c : symbols.blacklist.classes) usedClasses.insert(c);
  for (const auto& c : symbols.whitelist.classes) usedClasses.insert(c);

  // ── Step 1: mangle non-setter selectors ───────────────────────
  // firstUpper=false → camelCase ("turnMuchMean")
  std::unordered_map<std::string, std::string> nonSetterMap;

  for (const auto& sel : symbols.whitelist.selectors) {
    if (SymbolsCollector::isSetter(sel)) continue;

    // Selectors may end with ':' — strip it for length calculation,
    // then restore it after generation.
    // e.g. "viewDidLoad" (11) → generate 11-char sentence
    //      "foo:" (4) → generate 4-char sentence, then append ':'
    //
    // NOTE: multi-arg selectors like "foo:bar:" contain colons in
    // the middle. Those are rare in user-defined symbols but we handle
    // them by computing the non-colon length and re-inserting colons.
    // Count non-colon chars to determine the word-fill target.
    size_t colonCount = std::count(sel.begin(), sel.end(), ':');
    size_t wordLen = sel.size() - colonCount;

    if (wordLen == 0) {
      // Degenerate selector (all colons) — skip
      continue;
    }

    std::string mangled;
    bool found = false;

    for (uint32_t attempt = 0; attempt < maxRetries_; ++attempt) {
      std::string sentence = generateSentence(wordLen, state, false);
      if (sentence.empty()) break;  // word list exhausted for this length

      // Re-insert colons at their original positions
      mangled.clear();
      mangled.reserve(sel.size());
      size_t si = 0;  // index into sentence
      for (size_t i = 0; i < sel.size(); ++i) {
        if (sel[i] == ':') {
          mangled += ':';
        } else {
          mangled += (si < sentence.size() ? sentence[si++] : '?');
        }
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
    // Silent skip on exhaustion — same policy as RandomMangler
  }

  // ── Step 2: derive setter mappings from getter mappings ───────
  for (const auto& sel : symbols.whitelist.selectors) {
    if (!SymbolsCollector::isSetter(sel)) continue;
    std::string getter = getterFromSetter(sel);
    if (getter.empty()) continue;
    auto it = nonSetterMap.find(getter);
    if (it == nonSetterMap.end()) continue;
    result.selectors[sel] = setterFromGetter(it->second);
  }

  // ── Step 3: mangle class names ────────────────────────────────
  // firstUpper=true → PascalCase ("TurnMuchMean")
  for (const auto& cls : symbols.whitelist.classes) {
    std::string mangled;
    bool found = false;

    for (uint32_t attempt = 0; attempt < maxRetries_; ++attempt) {
      mangled = generateSentence(cls.size(), state, true);
      if (mangled.empty()) break;
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

ManglerType parseManglerType(const std::string& str) {
  if (str == "caesar") return ManglerType::Caesar;
  if (str == "random") return ManglerType::Random;
  if (str == "realwords") return ManglerType::RealWords;
  throw std::invalid_argument("Invalid mangler type: " + str);
}

std::string manglerTypeToString(ManglerType type) {
  switch (type) {
    case ManglerType::Caesar:
      return "caesar";
    case ManglerType::Random:
      return "random";
    case ManglerType::RealWords:
      return "realwords";
    default:
      return "unknown";
  }
}
