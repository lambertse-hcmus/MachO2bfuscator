#include <gtest/gtest.h>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/symbol_sets.h"

#ifdef TEST_OBJC_ABSOLUTE_PATH
static const std::string kBinaryPath = "assets/testckey_objc";
#else
#warning \
    "TEST_OBJC_ABSOLUTE_PATH is not defined. Some tests will be skipped that require a real binary."
static const std::string kBinaryPath = "";
#endif

// ════════════════════════════════════════════════════════════════════
//  Unit tests
// ════════════════════════════════════════════════════════════════════

TEST(SymbolSets, SetterNameGeneration) {
  EXPECT_EQ(SymbolsCollector::toSetterName("title"), "setTitle:");
  EXPECT_EQ(SymbolsCollector::toSetterName("isHidden"), "setIsHidden:");
  EXPECT_EQ(SymbolsCollector::toSetterName("x"), "setX:");
  EXPECT_EQ(SymbolsCollector::toSetterName(""), "");
}

TEST(SymbolSets, IsSetterDetection) {
  EXPECT_TRUE(SymbolsCollector::isSetter("setTitle:"));
  EXPECT_TRUE(SymbolsCollector::isSetter("setIsHidden:"));
  EXPECT_FALSE(SymbolsCollector::isSetter("title"));
  EXPECT_FALSE(SymbolsCollector::isSetter("settitle:"));  // lowercase after set
  EXPECT_FALSE(SymbolsCollector::isSetter("setTitle"));  // missing trailing ':'
  EXPECT_FALSE(SymbolsCollector::isSetter("set:"));  // no uppercase after 'set'
  EXPECT_FALSE(SymbolsCollector::isSetter(""));
}

TEST(SymbolSets, ObjCSymbolSetsMerge) {
  ObjCSymbolSets a, b;
  a.selectors = {"viewDidLoad", "init"};
  a.classes = {"MyViewController"};
  b.selectors = {"dealloc", "init"};  // "init" duplicate
  b.classes = {"AppDelegate"};

  a.mergeFrom(b);

  EXPECT_EQ(a.selectors.size(), 3u);
  EXPECT_EQ(a.classes.size(), 2u);
  EXPECT_EQ(a.selectors.count("viewDidLoad"), 1u);
  EXPECT_EQ(a.selectors.count("init"), 1u);
  EXPECT_EQ(a.selectors.count("dealloc"), 1u);
  EXPECT_EQ(a.classes.count("MyViewController"), 1u);
  EXPECT_EQ(a.classes.count("AppDelegate"), 1u);
}

TEST(SymbolSets, LibobjcSelectorsAlwaysBlacklisted) {
  SymbolsCollector::Config config;  // empty — no binaries
  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  EXPECT_EQ(symbols.blacklist.selectors.count("retain"), 1u);
  EXPECT_EQ(symbols.blacklist.selectors.count("release"), 1u);
  EXPECT_EQ(symbols.blacklist.selectors.count("dealloc"), 1u);
  EXPECT_EQ(symbols.blacklist.selectors.count("alloc"), 1u);

  EXPECT_TRUE(symbols.whitelist.selectors.empty());
  EXPECT_TRUE(symbols.whitelist.classes.empty());
}

TEST(SymbolSets, ManualBlacklistRespected) {
  SymbolsCollector::Config config;
  config.manualClassBlacklist = {"ForbiddenClass"};
  config.manualSelectorBlacklist = {"forbiddenMethod"};

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  EXPECT_EQ(symbols.blacklist.classes.count("ForbiddenClass"), 1u);
  EXPECT_EQ(symbols.blacklist.selectors.count("forbiddenMethod"), 1u);
}

TEST(SymbolSets, WhitelistExcludesBlacklistedSymbols) {
  ObjCSymbolSets userSymbols;
  userSymbols.classes = {"ClassA", "ClassB", "ClassC"};
  userSymbols.selectors = {"methodA", "methodB", "methodC"};

  ObjCSymbolSets systemSymbols;
  systemSymbols.classes = {"ClassB"};
  systemSymbols.selectors = {"methodB"};

  ObjCSymbolSets blacklist;
  blacklist.mergeFrom(systemSymbols);

  ObjCSymbolSets whitelist, removedList;
  for (const auto& c : userSymbols.classes) {
    if (!blacklist.classes.count(c))
      whitelist.classes.insert(c);
    else
      removedList.classes.insert(c);
  }
  for (const auto& s : userSymbols.selectors) {
    if (!blacklist.selectors.count(s))
      whitelist.selectors.insert(s);
    else
      removedList.selectors.insert(s);
  }

  EXPECT_EQ(whitelist.classes.count("ClassA"), 1u);
  EXPECT_EQ(whitelist.classes.count("ClassC"), 1u);
  EXPECT_EQ(whitelist.classes.count("ClassB"), 0u);  // blacklisted
  EXPECT_EQ(removedList.classes.count("ClassB"), 1u);

  EXPECT_EQ(whitelist.selectors.count("methodA"), 1u);
  EXPECT_EQ(whitelist.selectors.count("methodC"), 1u);
  EXPECT_EQ(whitelist.selectors.count("methodB"), 0u);
  EXPECT_EQ(removedList.selectors.count("methodB"), 1u);
}

TEST(SymbolSets, SetterDerivationInBlacklist) {
  // If "title" is blacklisted, "setTitle:" must also be blacklisted.
  SymbolsCollector::Config config;
  config.manualSelectorBlacklist = {"title"};

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  EXPECT_EQ(symbols.blacklist.selectors.count("title"), 1u);
  EXPECT_EQ(symbols.blacklist.selectors.count("setTitle:"), 1u);
}

// ════════════════════════════════════════════════════════════════════
//  Integration tests
// ════════════════════════════════════════════════════════════════════

TEST(Integration, SymbolSetsCollectFromBinary) {
  SymbolsCollector::Config config;
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  config.obfuscablePaths = {kBinaryPath};

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  EXPECT_FALSE(symbols.whitelist.classes.empty());
  EXPECT_FALSE(symbols.whitelist.selectors.empty());

  EXPECT_EQ(symbols.blacklist.selectors.count("retain"), 1u);
  EXPECT_EQ(symbols.blacklist.selectors.count("dealloc"), 1u);

  // No whitelisted symbol may appear in the blacklist
  for (const auto& cls : symbols.whitelist.classes) {
    EXPECT_EQ(symbols.blacklist.classes.count(cls), 0u)
        << "Whitelisted class also in blacklist: " << cls;
  }
  for (const auto& sel : symbols.whitelist.selectors) {
    EXPECT_EQ(symbols.blacklist.selectors.count(sel), 0u)
        << "Whitelisted selector also in blacklist: " << sel;
  }

  RecordProperty("whitelist_classes",
                 std::to_string(symbols.whitelist.classes.size()));
  RecordProperty("whitelist_selectors",
                 std::to_string(symbols.whitelist.selectors.size()));
  RecordProperty("blacklist_selectors",
                 std::to_string(symbols.blacklist.selectors.size()));
}

TEST(Integration, SymbolSetsUnobfuscableEmptiesWhitelist) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  // Same binary as both obfuscable and unobfuscable → whitelist must be empty.
  SymbolsCollector::Config config;
  config.obfuscablePaths = {kBinaryPath};
  config.unobfuscablePaths = {kBinaryPath};

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  EXPECT_TRUE(symbols.whitelist.classes.empty());
  EXPECT_TRUE(symbols.whitelist.selectors.empty());
  EXPECT_FALSE(symbols.removedList.classes.empty());
}
