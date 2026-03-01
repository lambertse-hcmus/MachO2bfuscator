#include <gtest/gtest.h>

#include <cctype>

#include "MachO2bfuscator/mangler.h"
#include "MachO2bfuscator/symbol_sets.h"

#ifdef TEST_OBJC_ABSOLUTE_PATH
static const std::string kBinaryPath = "assets/testckey_objc";
#else
#warning \
    "TEST_OBJC_ABSOLUTE_PATH is not defined. Some tests will be skipped that require a real binary."
static const std::string kBinaryPath = "";
#endif

// ── Helpers ──────────────────────────────────────────────────────────
static ObfuscationSymbols makeSymbols(
    std::unordered_set<std::string> selectors,
    std::unordered_set<std::string> classes,
    std::unordered_set<std::string> blacklistSels = {},
    std::unordered_set<std::string> blacklistCls = {}) {
  ObfuscationSymbols sym;
  sym.whitelist.selectors = std::move(selectors);
  sym.whitelist.classes = std::move(classes);
  sym.blacklist.selectors = std::move(blacklistSels);
  sym.blacklist.classes = std::move(blacklistCls);
  return sym;
}

// ════════════════════════════════════════════════════════════════════
//  CaesarMangler
// ════════════════════════════════════════════════════════════════════

TEST(CaesarMangler, PlainStringChanges) {
  CaesarMangler m(13);
  std::string input = "viewDidLoad";
  std::string output = m.mangleString(input);
  EXPECT_EQ(output.size(), input.size());
  EXPECT_NE(output, input);
}

TEST(CaesarMangler, PreservesColon) {
  CaesarMangler m(13);
  std::string output = m.mangleString("foo:bar:");
  EXPECT_EQ(output.size(), 8u);
  EXPECT_EQ(output[3], ':');
  EXPECT_EQ(output[7], ':');
}

TEST(CaesarMangler, SetterPrefixPreserved) {
  CaesarMangler m(13);
  std::string output = m.mangleString("setTitle:");
  EXPECT_EQ(output.substr(0, 3), "set");
  EXPECT_EQ(output.back(), ':');
  EXPECT_EQ(output.size(), 9u);
}

TEST(CaesarMangler, SameLengthForVariousInputs) {
  CaesarMangler m(13);
  for (const std::string& s : {"init", "dealloc", "MyViewController",
                               "setFrame:", "applicationDidFinishLaunching:"}) {
    EXPECT_EQ(m.mangleString(s).size(), s.size())
        << "Length mismatch for: " << s;
  }
}

TEST(CaesarMangler, Deterministic) {
  CaesarMangler m(13);
  EXPECT_EQ(m.mangleString("viewDidLoad"), m.mangleString("viewDidLoad"));
  EXPECT_EQ(m.mangleString("MyClass"), m.mangleString("MyClass"));
}

TEST(CaesarMangler, ManglingMapContainsAllEntries) {
  auto symbols =
      makeSymbols({"viewDidLoad", "setTitle:"}, {"MyViewController"});
  CaesarMangler m(13);
  ManglingMap map = m.mangle(symbols);

  EXPECT_EQ(map.selectors.count("viewDidLoad"), 1u);
  EXPECT_EQ(map.selectors.count("setTitle:"), 1u);
  EXPECT_EQ(map.classNames.count("MyViewController"), 1u);

  const auto& mangledSetter = map.selectors.at("setTitle:");
  EXPECT_EQ(mangledSetter.substr(0, 3), "set");
  EXPECT_EQ(mangledSetter.back(), ':');
}

// ════════════════════════════════════════════════════════════════════
//  RandomMangler
// ════════════════════════════════════════════════════════════════════

TEST(RandomMangler, SameLength) {
  auto symbols = makeSymbols({"viewDidLoad", "setTitle:", "init"},
                             {"MyViewController", "AppDelegate"});
  ManglingMap map = RandomMangler(42).mangle(symbols);

  for (const auto& [orig, mangled] : map.selectors) {
    EXPECT_EQ(mangled.size(), orig.size())
        << "Length mismatch for selector: " << orig;
  }
  for (const auto& [orig, mangled] : map.classNames) {
    EXPECT_EQ(mangled.size(), orig.size())
        << "Length mismatch for class: " << orig;
  }
}

TEST(RandomMangler, NoBlacklistClashes) {
  auto symbols = makeSymbols({"viewDidLoad", "myMethod"}, {"MyClass"},
                             {"retain", "release", "dealloc"}, {"NSObject"});
  ManglingMap map = RandomMangler(42).mangle(symbols);

  for (const auto& [orig, mangled] : map.selectors) {
    EXPECT_EQ(symbols.blacklist.selectors.count(mangled), 0u)
        << "Mangled selector clashes with blacklist: " << mangled;
  }
  for (const auto& [orig, mangled] : map.classNames) {
    EXPECT_EQ(symbols.blacklist.classes.count(mangled), 0u)
        << "Mangled class clashes with blacklist: " << mangled;
  }
}

TEST(RandomMangler, ClassNamesStartUppercase) {
  auto symbols = makeSymbols({}, {"MyClass", "AppDelegate", "TableViewCell"});
  ManglingMap map = RandomMangler(42).mangle(symbols);

  for (const auto& [orig, mangled] : map.classNames) {
    ASSERT_FALSE(mangled.empty());
    EXPECT_TRUE(std::isupper(static_cast<unsigned char>(mangled[0])))
        << "Class name does not start uppercase: " << mangled;
  }
}

TEST(RandomMangler, SelectorsStartLowercase) {
  auto symbols =
      makeSymbols({"viewDidLoad", "init", "myMethod", "foo:bar:"}, {});
  ManglingMap map = RandomMangler(42).mangle(symbols);

  for (const auto& [orig, mangled] : map.selectors) {
    ASSERT_FALSE(mangled.empty());
    EXPECT_TRUE(std::islower(static_cast<unsigned char>(mangled[0])))
        << "Selector does not start lowercase: " << mangled;
  }
}

TEST(RandomMangler, SetterGetterConsistency) {
  auto symbols = makeSymbols({"title", "setTitle:"}, {});
  ManglingMap map = RandomMangler(42).mangle(symbols);

  ASSERT_EQ(map.selectors.count("title"), 1u);
  ASSERT_EQ(map.selectors.count("setTitle:"), 1u);

  const std::string& mangledGetter = map.selectors.at("title");
  const std::string& mangledSetter = map.selectors.at("setTitle:");
  std::string expectedSetter = SymbolsCollector::toSetterName(mangledGetter);
  EXPECT_EQ(mangledSetter, expectedSetter)
      << "Setter '" << mangledSetter << "' does not derive from getter '"
      << mangledGetter << "'";
}

TEST(RandomMangler, DeterministicWithSameSeed) {
  auto symbols = makeSymbols({"viewDidLoad", "init"}, {"MyClass"});
  ManglingMap map1 = RandomMangler(12345).mangle(symbols);
  ManglingMap map2 = RandomMangler(12345).mangle(symbols);

  EXPECT_EQ(map1.selectors, map2.selectors);
  EXPECT_EQ(map1.classNames, map2.classNames);
}

TEST(RandomMangler, DifferentSeedsDiffer) {
  auto symbols = makeSymbols({"viewDidLoad"}, {});
  ManglingMap map1 = RandomMangler(1).mangle(symbols);
  ManglingMap map2 = RandomMangler(2).mangle(symbols);

  EXPECT_NE(map1.selectors.at("viewDidLoad"), map2.selectors.at("viewDidLoad"));
}

// ════════════════════════════════════════════════════════════════════
//  Integration
// ════════════════════════════════════════════════════════════════════

TEST(Integration, ManglerCaesarOnRealBinary) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  SymbolsCollector::Config cfg;
  cfg.obfuscablePaths = {kBinaryPath};
  ObfuscationSymbols symbols = SymbolsCollector::collect(cfg);

  CaesarMangler m(13);
  ManglingMap map = m.mangle(symbols);

  EXPECT_FALSE(map.selectors.empty());
  EXPECT_FALSE(map.classNames.empty());

  for (const auto& [orig, mangled] : map.selectors) {
    EXPECT_EQ(mangled.size(), orig.size()) << orig;
  }
  for (const auto& [orig, mangled] : map.classNames) {
    EXPECT_EQ(mangled.size(), orig.size()) << orig;
  }

  RecordProperty("caesar_selectors", std::to_string(map.selectors.size()));
  RecordProperty("caesar_classes", std::to_string(map.classNames.size()));
}

TEST(Integration, ManglerRandomOnRealBinary) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  SymbolsCollector::Config cfg;
  cfg.obfuscablePaths = {kBinaryPath};
  ObfuscationSymbols symbols = SymbolsCollector::collect(cfg);

  RandomMangler m(42);
  ManglingMap map = m.mangle(symbols);

  EXPECT_FALSE(map.selectors.empty());
  EXPECT_FALSE(map.classNames.empty());

  for (const auto& [orig, mangled] : map.classNames) {
    EXPECT_EQ(mangled.size(), orig.size());
    ASSERT_FALSE(mangled.empty());
    EXPECT_TRUE(std::isupper(static_cast<unsigned char>(mangled[0])));
  }
  for (const auto& [orig, mangled] : map.selectors) {
    EXPECT_EQ(mangled.size(), orig.size());
    if (!SymbolsCollector::isSetter(orig)) {
      ASSERT_FALSE(mangled.empty());
      EXPECT_TRUE(std::islower(static_cast<unsigned char>(mangled[0])));
    }
    EXPECT_EQ(symbols.blacklist.selectors.count(mangled), 0u)
        << "Mangled selector in blacklist: " << mangled;
  }
  for (const auto& [orig, mangled] : map.classNames) {
    EXPECT_EQ(symbols.blacklist.classes.count(mangled), 0u)
        << "Mangled class in blacklist: " << mangled;
  }

  RecordProperty("random_selectors", std::to_string(map.selectors.size()));
  RecordProperty("random_classes", std::to_string(map.classNames.size()));
}
