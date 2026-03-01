#include <iostream>
#include <stdexcept>

#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"

static int g_passed = 0, g_failed = 0;
#define RUN(name)                                \
  do {                                           \
    std::cout << "  running " #name " ... ";     \
    try {                                        \
      name();                                    \
      std::cout << "PASS\n";                     \
      ++g_passed;                                \
    } catch (const std::exception& e) {          \
      std::cout << "FAIL: " << e.what() << "\n"; \
      ++g_failed;                                \
    }                                            \
  } while (0)
#define ASSERT(c)                                                    \
  do {                                                               \
    if (!(c))                                                        \
      throw std::runtime_error("Assertion failed: " #c " at line " + \
                               std::to_string(__LINE__));            \
  } while (0)
#define ASSERT_EQ(a, b)                                          \
  do {                                                           \
    if ((a) != (b))                                              \
      throw std::runtime_error(std::string("Expected: ") + (b) + \
                               " got: " + (a));                  \
  } while (0)

const std::string objCPath =
    "/Users/tri.le/src/opensource/lambertse/MachO2bfuscator/assets/"
    "testckey_objc";
// ── CaesarMangler unit tests ──────────────────────────────────────

void test_caesar_plain_string() {
  CaesarMangler m(13);
  std::string input = "viewDidLoad";
  std::string output = m.mangleString(input);
  ASSERT(output.size() == input.size());
  ASSERT(output != input);
}

void test_caesar_preserves_colon() {
  CaesarMangler m(13);
  std::string input = "foo:bar:";
  std::string output = m.mangleString(input);
  ASSERT(output.size() == input.size());
  ASSERT(output[3] == ':');
  ASSERT(output[7] == ':');
}

void test_caesar_setter_prefix_preserved() {
  CaesarMangler m(13);
  std::string input = "setTitle:";
  std::string output = m.mangleString(input);
  ASSERT(output.substr(0, 3) == "set");
  ASSERT(output.back() == ':');
  ASSERT(output.size() == input.size());
}

void test_caesar_same_length() {
  CaesarMangler m(13);
  for (const std::string& s : {"init", "dealloc", "MyViewController",
                               "setFrame:", "applicationDidFinishLaunching:"}) {
    ASSERT(m.mangleString(s).size() == s.size());
  }
}

void test_caesar_deterministic() {
  CaesarMangler m(13);
  ASSERT(m.mangleString("viewDidLoad") == m.mangleString("viewDidLoad"));
  ASSERT(m.mangleString("MyClass") == m.mangleString("MyClass"));
}

void test_caesar_mangle_map() {
  ObjCSymbolSets whitelist, blacklist;
  whitelist.selectors = {"viewDidLoad", "setTitle:"};
  whitelist.classes = {"MyViewController"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  CaesarMangler m(13);
  ManglingMap map = m.mangle(symbols);

  ASSERT(map.selectors.count("viewDidLoad") == 1);
  ASSERT(map.selectors.count("setTitle:") == 1);
  ASSERT(map.classNames.count("MyViewController") == 1);

  // Mangled setter must still start with "set" and end with ":"
  const auto& mangledSetter = map.selectors.at("setTitle:");
  ASSERT(mangledSetter.substr(0, 3) == "set");
  ASSERT(mangledSetter.back() == ':');
}

// ── RandomMangler unit tests ──────────────────────────────────────

void test_random_same_length() {
  RandomMangler m(42);
  ObjCSymbolSets whitelist, blacklist;
  whitelist.selectors = {"viewDidLoad", "setTitle:", "init"};
  whitelist.classes = {"MyViewController", "AppDelegate"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  ManglingMap map = m.mangle(symbols);

  for (const auto& [orig, mangled] : map.selectors) {
    ASSERT(mangled.size() == orig.size());
  }
  for (const auto& [orig, mangled] : map.classNames) {
    ASSERT(mangled.size() == orig.size());
  }
}

void test_random_no_blacklist_clashes() {
  RandomMangler m(42);
  ObjCSymbolSets whitelist, blacklist;
  whitelist.selectors = {"viewDidLoad", "myMethod"};
  whitelist.classes = {"MyClass"};
  blacklist.selectors = {"retain", "release", "dealloc"};
  blacklist.classes = {"NSObject"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  ManglingMap map = m.mangle(symbols);

  for (const auto& [orig, mangled] : map.selectors) {
    ASSERT(blacklist.selectors.count(mangled) == 0);
  }
  for (const auto& [orig, mangled] : map.classNames) {
    ASSERT(blacklist.classes.count(mangled) == 0);
  }
}

void test_random_class_names_start_uppercase() {
  // First char of every mangled class name must be A-Z
  // Mirrors Swift: .capitalizedOnFirstLetter
  RandomMangler m(42);
  ObjCSymbolSets whitelist, blacklist;
  whitelist.classes = {"MyClass", "AppDelegate", "TableViewCell"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  ManglingMap map = m.mangle(symbols);

  for (const auto& [orig, mangled] : map.classNames) {
    ASSERT(!mangled.empty());
    ASSERT(isupper(static_cast<unsigned char>(mangled[0])));
  }
}

void test_random_selectors_start_lowercase() {
  // First char of every mangled non-setter selector must be a-z
  // Mirrors ObjC selector naming convention
  RandomMangler m(42);
  ObjCSymbolSets whitelist, blacklist;
  whitelist.selectors = {"viewDidLoad", "init", "myMethod", "foo:bar:"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  ManglingMap map = m.mangle(symbols);

  for (const auto& [orig, mangled] : map.selectors) {
    ASSERT(!mangled.empty());
    // First char must be a-z (lower) — never a digit or uppercase
    ASSERT(islower(static_cast<unsigned char>(mangled[0])));
  }
}

void test_random_setter_getter_consistency() {
  // Mirrors Swift: test_mangleSymbols_shouldMangleSettersAndGettersCoherently
  // If "title" → "xyzab" then "setTitle:" → "setXyzab:"
  RandomMangler m(42);
  ObjCSymbolSets whitelist, blacklist;
  whitelist.selectors = {"title", "setTitle:"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  ManglingMap map = m.mangle(symbols);

  ASSERT(map.selectors.count("title") == 1);
  ASSERT(map.selectors.count("setTitle:") == 1);

  const std::string& mangledGetter = map.selectors.at("title");
  const std::string& mangledSetter = map.selectors.at("setTitle:");

  // Setter must be "set" + capitalised(mangledGetter) + ":"
  std::string expectedSetter = SymbolsCollector::toSetterName(mangledGetter);
  ASSERT_EQ(mangledSetter, expectedSetter);
}

void test_random_deterministic_with_seed() {
  ObjCSymbolSets whitelist, blacklist;
  whitelist.selectors = {"viewDidLoad", "init"};
  whitelist.classes = {"MyClass"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  ManglingMap map1 = RandomMangler(12345).mangle(symbols);
  ManglingMap map2 = RandomMangler(12345).mangle(symbols);

  ASSERT(map1.selectors == map2.selectors);
  ASSERT(map1.classNames == map2.classNames);
}

void test_random_different_seeds_differ() {
  ObjCSymbolSets whitelist, blacklist;
  whitelist.selectors = {"viewDidLoad"};

  ObfuscationSymbols symbols;
  symbols.whitelist = whitelist;
  symbols.blacklist = blacklist;

  ManglingMap map1 = RandomMangler(1).mangle(symbols);
  ManglingMap map2 = RandomMangler(2).mangle(symbols);

  ASSERT(map1.selectors.at("viewDidLoad") != map2.selectors.at("viewDidLoad"));
}

// ── Integration test ──────────────────────────────────────────────

void test_mangle_real_binary() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  SymbolsCollector::Config config;
  config.obfuscablePaths = {path};

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  // ── CaesarMangler ─────────────────────────────────────────────
  {
    CaesarMangler m(13);
    ManglingMap map = m.mangle(symbols);

    ASSERT(!map.selectors.empty());
    ASSERT(!map.classNames.empty());

    // Every entry must preserve byte length
    for (const auto& [orig, mangled] : map.selectors) {
      ASSERT(mangled.size() == orig.size());
    }
    for (const auto& [orig, mangled] : map.classNames) {
      ASSERT(mangled.size() == orig.size());
    }

    std::cout << "\n    Caesar: " << map.selectors.size() << " selectors, "
              << map.classNames.size() << " classes mangled\n";
  }

  // ── RandomMangler ─────────────────────────────────────────────
  {
    RandomMangler m(42);
    ManglingMap map = m.mangle(symbols);

    ASSERT(!map.selectors.empty());
    ASSERT(!map.classNames.empty());

    // Every entry must preserve byte length
    for (const auto& [orig, mangled] : map.selectors) {
      ASSERT(mangled.size() == orig.size());
    }

    // Class names: first char must be A-Z
    for (const auto& [orig, mangled] : map.classNames) {
      ASSERT(mangled.size() == orig.size());
      ASSERT(!mangled.empty());
      ASSERT(isupper(static_cast<unsigned char>(mangled[0])));
    }

    // Non-setter selectors: first char must be a-z
    for (const auto& [orig, mangled] : map.selectors) {
      if (!SymbolsCollector::isSetter(orig)) {
        ASSERT(!mangled.empty());
        ASSERT(islower(static_cast<unsigned char>(mangled[0])));
      }
    }

    // No mangled name must appear in the blacklist
    for (const auto& [orig, mangled] : map.selectors) {
      ASSERT(symbols.blacklist.selectors.count(mangled) == 0);
    }
    for (const auto& [orig, mangled] : map.classNames) {
      ASSERT(symbols.blacklist.classes.count(mangled) == 0);
    }

    std::cout << "    Random: " << map.selectors.size() << " selectors, "
              << map.classNames.size() << " classes mangled\n";

    // Print a few examples so we can visually verify
    std::cout << "    Examples:\n";
    int shown = 0;
    for (const auto& [orig, mangled] : map.classNames) {
      std::cout << "      " << orig << " → " << mangled << "\n";
      if (++shown >= 3) break;
    }
    shown = 0;
    for (const auto& [orig, mangled] : map.selectors) {
      std::cout << "      " << orig << " → " << mangled << "\n";
      if (++shown >= 3) break;
    }
  }
}

// ─────────────────────────────────────────────────────────────────
int main() {
  std::cout << "=== Phase 5 Tests: Symbol Mangling ===\n\n";

  std::cout << "── CaesarMangler ──\n";
  RUN(test_caesar_plain_string);
  RUN(test_caesar_preserves_colon);
  RUN(test_caesar_setter_prefix_preserved);
  RUN(test_caesar_same_length);
  RUN(test_caesar_deterministic);
  RUN(test_caesar_mangle_map);

  std::cout << "\n── RandomMangler ──\n";
  RUN(test_random_same_length);
  RUN(test_random_no_blacklist_clashes);
  RUN(test_random_class_names_start_uppercase);
  RUN(test_random_selectors_start_lowercase);  // ← new
  RUN(test_random_setter_getter_consistency);
  RUN(test_random_deterministic_with_seed);
  RUN(test_random_different_seeds_differ);

  std::cout << "\n── Integration ──\n";
  RUN(test_mangle_real_binary);

  std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed
            << " failed ===\n";
  return g_failed > 0 ? 1 : 0;
}
