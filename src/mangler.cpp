#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"

#include <cctype>
#include <random>
#include <stdexcept>

// ═══════════════════════════════════════════════════════════════
//  CaesarMangler
// ═══════════════════════════════════════════════════════════════

CaesarMangler::CaesarMangler(uint8_t cypherKey)
    : cypherKey_(cypherKey) {}

// ── encryptByte ───────────────────────────────────────────────────
// Mirrors Swift: CaesarCypher.encrypt(element:key:)
//
// Printable ASCII range: [33 ('!'), 126 ('~')] — 94 characters total.
// ':' (0x3A = 58) is always returned unchanged —
// it is the ObjC selector argument separator.
uint8_t CaesarMangler::encryptByte(uint8_t byte) const {
    // Preserve colon — ObjC selector argument separator
    if (byte == 0x3A) return byte;

    constexpr uint8_t rangeStart = 33;
    constexpr uint8_t rangeEnd   = 126;
    constexpr uint8_t rangeCount = rangeEnd - rangeStart + 1; // 94

    if (byte >= rangeStart && byte <= rangeEnd) {
        uint8_t shifted = byte + cypherKey_;
        return (shifted <= rangeEnd) ? shifted
                                     : shifted - rangeCount;
    }

    // Outside printable range — leave unchanged
    return byte;
}

// ── mangleString ────────────────────────���────────────────────────
// Mirrors Swift: CaesarStringMangler.mangle(_:usingCypherKey:)
//
// Setter-aware: if the string starts with "set", preserve the
// "set" prefix and only encrypt the getter part.
//   "setTitle:" → "set" + encrypt("Title:")
//   "viewDidLoad" → encrypt("viewDidLoad")
std::string CaesarMangler::mangleString(const std::string& input) const {
    // Mirrors Swift:
    // if word.hasPrefix("set") {
    //     return "set" + mangle(wordSubstring, usingCypherKey: cypherKey)
    // }
    if (input.size() > 3 &&
        input[0] == 's' && input[1] == 'e' && input[2] == 't')
    {
        std::string suffix        = input.substr(3);
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
// Mirrors Swift: CaesarMangler.mangleSymbols(_:)
//
// Applies mangleString() to every whitelisted selector and class,
// then verifies no mangled name clashes with the blacklist.
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

    // Clash check — mirrors Swift: fatalError("ReverseMangler clashed on symbol")
    for (const auto& [orig, mangled] : result.selectors) {
        if (symbols.blacklist.selectors.count(mangled)) {
            throw std::runtime_error(
                "CaesarMangler: mangled selector '" + orig +
                "' → '" + mangled + "' clashes with blacklist");
        }
    }
    for (const auto& [orig, mangled] : result.classNames) {
        if (symbols.blacklist.classes.count(mangled)) {
            throw std::runtime_error(
                "CaesarMangler: mangled class '" + orig +
                "' → '" + mangled + "' clashes with blacklist");
        }
    }

    return result;
}

// ═══════════════════════════════════════════════════════════════
//  RandomMangler
// ═══════════════════════════════════════════════════════════════

RandomMangler::RandomMangler(uint32_t seed, uint32_t maxRetries)
    : seed_(seed), maxRetries_(maxRetries) {}

// ── randomName ────────────────────────────────────────────────────
// Generates a random name of exactly byteLen bytes using xorshift32.
//
// firstCharMode:
//   'u' → first char from A-Z  (26 chars) — for class names
//   'l' → first char from a-z  (26 chars) — for selectors
//   'a' → first char from a-zA-Z0-9 (62 chars) — unrestricted
//
// Remaining chars always from a-zA-Z0-9 (62 chars).
std::string RandomMangler::randomName(size_t    byteLen,
                                      uint32_t& state,
                                      char      firstCharMode) const {
    static constexpr char kAll[]   = "abcdefghijklmnopqrstuvwxyz"
                                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                     "0123456789";
    static constexpr char kLower[] = "abcdefghijklmnopqrstuvwxyz";
    static constexpr char kUpper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    static constexpr size_t kNAll   = 62;
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
                case 'u': result[i] = kUpper[state % kNUpper]; break;
                case 'l': result[i] = kLower[state % kNLower]; break;
                default:  result[i] = kAll  [state % kNAll];   break;
            }
        } else {
            result[i] = kAll[state % kNAll];
        }
    }
    return result;
}

// ── getterFromSetter ─────────────────────────────────────────────
// Mirrors Swift: String.getterFromSetter
// "setTitle:"   → "title"
// "setIsHidden:" → "isHidden"
std::string RandomMangler::getterFromSetter(const std::string& setter) {
    if (!SymbolsCollector::isSetter(setter)) return "";

    // Drop "set" prefix and trailing ":"
    std::string getterPart = setter.substr(3, setter.size() - 4);

    // Lowercase the first letter
    if (!getterPart.empty()) {
        getterPart[0] = static_cast<char>(
            tolower(static_cast<unsigned char>(getterPart[0])));
    }
    return getterPart;
}

// ── setterFromGetter ─────────────────────────────────────────────
// Mirrors Swift: String.setterFromGetter
// "title" → "setTitle:"
std::string RandomMangler::setterFromGetter(const std::string& getter) {
    return SymbolsCollector::toSetterName(getter);
}

// ── RandomMangler::mangle ─────────────────────────────────────────
// Mirrors Swift: RealWordsMangler.mangleSymbols(_:sentenceGenerator:)
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
        if (state == 0) state = 0xDEADBEEFu; // fallback if rd returns 0
    }

    // ── Build used-name sets for clash avoidance ──────────────────
    // Mirrors Swift: mangledSelectorsBlacklist =
    //     (Array(blacklist.selectors) + Array(whitelist.selectors)).uniq
    // We must not produce a mangled name that is:
    //   a) already in the blacklist (would break system symbols)
    //   b) already used as another mangled name (duplicate mapping)
    std::unordered_set<std::string> usedSelectors;
    for (const auto& s : symbols.blacklist.selectors)
        usedSelectors.insert(s);
    for (const auto& s : symbols.whitelist.selectors)
        usedSelectors.insert(s);

    std::unordered_set<std::string> usedClasses;
    for (const auto& c : symbols.blacklist.classes)
        usedClasses.insert(c);
    for (const auto& c : symbols.whitelist.classes)
        usedClasses.insert(c);

    // ── Step 1: mangle non-setter selectors ───────────────────────
    // Mirrors Swift: nonSettersManglingMap(sentenceGenerator:)
    //
    // Only non-setters are randomised here.
    // Setters are derived in Step 2 for getter/setter consistency.
    // First char is forced to a-z so selectors follow ObjC convention.
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
            nonSetterMap[sel]        = mangled;
            result.selectors[sel]    = mangled;
            usedSelectors.insert(mangled);
        }
        // If not found after maxRetries, silently skip.
        // Extremely unlikely for any reasonable symbol count.
    }

    // ── Step 2: derive setter mappings from getter mappings ───────
    // Mirrors Swift: settersManglingMap(matchingToNonSetterManglingMap:)
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
        if (it == nonSetterMap.end()) continue; // getter not in whitelist

        result.selectors[sel] = setterFromGetter(it->second);
    }

    // ── Step 3: mangle class names ────────────────────────────────
    // Mirrors Swift: classManglingMap(sentenceGenerator:)
    //
    // First char is forced to A-Z — ObjC class name convention.
    // Mirrors Swift: .capitalizedOnFirstLetter
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
