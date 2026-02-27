#include <iostream>
#include <stdexcept>

#include "MachO2fuscator/mach_reader.h"
#include "MachO2fuscator/symbol_sets.h"

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
#define ASSERT_EQ(a, b)                                                 \
  do {                                                                  \
    if ((a) != (b))                                                     \
      throw std::runtime_error("Expected equal: " + std::to_string(a) + \
                               " vs " + std::to_string(b));             \
  } while (0)

const std::string objCPath = "/Users/tri.le/src/opensource/lambertse/MachO2fuscation/assets/testckey_objc";
// ── Unit tests (no binary needed) ────────────────────────────────

void test_setter_name_generation() {
  ASSERT(SymbolsCollector::toSetterName("title") == "setTitle:");
  ASSERT(SymbolsCollector::toSetterName("isHidden") == "setIsHidden:");
  ASSERT(SymbolsCollector::toSetterName("x") == "setX:");
  ASSERT(SymbolsCollector::toSetterName("") == "");
}

void test_is_setter_detection() {
  ASSERT(SymbolsCollector::isSetter("setTitle:"));
  ASSERT(SymbolsCollector::isSetter("setIsHidden:"));
  ASSERT(!SymbolsCollector::isSetter("title"));
  ASSERT(!SymbolsCollector::isSetter("settitle:"));  // lowercase 't' after set
  ASSERT(!SymbolsCollector::isSetter("setTitle"));   // no trailing ':'
  ASSERT(!SymbolsCollector::isSetter("set:"));       // no uppercase after 'set'
  ASSERT(!SymbolsCollector::isSetter(""));
}

void test_objcsymbolsets_merge() {
  ObjCSymbolSets a, b;
  a.selectors = {"viewDidLoad", "init"};
  a.classes = {"MyViewController"};

  b.selectors = {"dealloc", "init"};  // "init" is duplicate
  b.classes = {"AppDelegate"};

  a.mergeFrom(b);

  // Union — no duplicates
  ASSERT(a.selectors.size() == 3);
  ASSERT(a.classes.size() == 2);
  ASSERT(a.selectors.count("viewDidLoad") == 1);
  ASSERT(a.selectors.count("init") == 1);
  ASSERT(a.selectors.count("dealloc") == 1);
  ASSERT(a.classes.count("MyViewController") == 1);
  ASSERT(a.classes.count("AppDelegate") == 1);
}

void test_libobjc_selectors_are_always_blacklisted() {
  // Even with no binaries, libobjc selectors must end up in blacklist
  SymbolsCollector::Config config;
  // No binaries — empty config
  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  // libobjc selectors must be in blacklist
  ASSERT(symbols.blacklist.selectors.count("retain") == 1);
  ASSERT(symbols.blacklist.selectors.count("release") == 1);
  ASSERT(symbols.blacklist.selectors.count("dealloc") == 1);
  ASSERT(symbols.blacklist.selectors.count("alloc") == 1);

  // Whitelist must be empty (no user binaries)
  ASSERT(symbols.whitelist.selectors.empty());
  ASSERT(symbols.whitelist.classes.empty());
}

void test_manual_blacklist_respected() {
  SymbolsCollector::Config config;
  config.manualClassBlacklist = {"ForbiddenClass"};
  config.manualSelectorBlacklist = {"forbiddenMethod"};

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  ASSERT(symbols.blacklist.classes.count("ForbiddenClass") == 1);
  ASSERT(symbols.blacklist.selectors.count("forbiddenMethod") == 1);
}

void test_whitelist_excludes_blacklisted_symbols() {
  // Simulate: user binary has symbols A, B, C
  // System binary has symbol B
  // Expected: whitelist = {A, C}, blacklist contains B, removedList = {B}

  // We test this logic directly via the set operations
  ObjCSymbolSets userSymbols;
  userSymbols.classes = {"ClassA", "ClassB", "ClassC"};
  userSymbols.selectors = {"methodA", "methodB", "methodC"};

  ObjCSymbolSets systemSymbols;
  systemSymbols.classes = {"ClassB"};
  systemSymbols.selectors = {"methodB"};

  // Build blacklist manually (mirrors collect() logic)
  ObjCSymbolSets blacklist;
  blacklist.mergeFrom(systemSymbols);

  // Build whitelist
  ObjCSymbolSets whitelist;
  for (const auto& c : userSymbols.classes) {
    if (!blacklist.classes.count(c)) whitelist.classes.insert(c);
  }
  for (const auto& s : userSymbols.selectors) {
    if (!blacklist.selectors.count(s)) whitelist.selectors.insert(s);
  }

  // Build removedList
  ObjCSymbolSets removedList;
  for (const auto& c : userSymbols.classes) {
    if (blacklist.classes.count(c)) removedList.classes.insert(c);
  }
  for (const auto& s : userSymbols.selectors) {
    if (blacklist.selectors.count(s)) removedList.selectors.insert(s);
  }

  ASSERT(whitelist.classes.count("ClassA") == 1);
  ASSERT(whitelist.classes.count("ClassC") == 1);
  ASSERT(whitelist.classes.count("ClassB") == 0);  // blacklisted
  ASSERT(removedList.classes.count("ClassB") == 1);
  ASSERT(whitelist.selectors.count("methodA") == 1);
  ASSERT(whitelist.selectors.count("methodC") == 1);
  ASSERT(whitelist.selectors.count("methodB") == 0);
  ASSERT(removedList.selectors.count("methodB") == 1);
}

void test_setter_blacklisting() {
  // If "title" is blacklisted (from a system binary),
  // then "setTitle:" must also be automatically blacklisted.
  SymbolsCollector::Config config;
  config.manualSelectorBlacklist = {"title"};

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  ASSERT(symbols.blacklist.selectors.count("title") == 1);
  ASSERT(symbols.blacklist.selectors.count("setTitle:") == 1);
}

// ── Integration tests (use YOUR test binary) ──────────────���───────

void test_collect_from_test_binary() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  SymbolsCollector::Config config;
  config.obfuscablePaths = {path};
  // No unobfuscable paths — we just want to see user symbols

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  // Must have found some obfuscatable symbols
  ASSERT(!symbols.whitelist.classes.empty());
  ASSERT(!symbols.whitelist.selectors.empty());

  // libobjc selectors must always be blacklisted
  ASSERT(symbols.blacklist.selectors.count("retain") == 1);
  ASSERT(symbols.blacklist.selectors.count("dealloc") == 1);

  // No whitelisted symbol should also appear in the blacklist
  for (const auto& cls : symbols.whitelist.classes) {
    ASSERT(symbols.blacklist.classes.count(cls) == 0);
  }
  for (const auto& sel : symbols.whitelist.selectors) {
    ASSERT(symbols.blacklist.selectors.count(sel) == 0);
  }

  std::cout << "\n    whitelist:   " << symbols.whitelist.classes.size()
            << " classes, " << symbols.whitelist.selectors.size()
            << " selectors\n";
  std::cout << "    blacklist:   " << symbols.blacklist.classes.size()
            << " classes, " << symbols.blacklist.selectors.size()
            << " selectors\n";
  std::cout << "    removedList: " << symbols.removedList.classes.size()
            << " classes, " << symbols.removedList.selectors.size()
            << " selectors\n";
}

void test_unobfuscable_symbols_removed_from_whitelist() {
  const std::string path = objCPath;
  if (path.empty()) throw std::runtime_error("objCPath not set");

  // Use the same binary as BOTH obfuscable and unobfuscable.
  // Result: ALL user symbols become blacklisted → whitelist must be empty.
  // where blacklist ⊇ systemSymbols = userSymbols → whitelist = ∅
  SymbolsCollector::Config config;
  config.obfuscablePaths = {path};
  config.unobfuscablePaths = {path};  // same binary = fully blacklisted

  ObfuscationSymbols symbols = SymbolsCollector::collect(config);

  // Whitelist must be empty because all user symbols are also system symbols
  ASSERT(symbols.whitelist.classes.empty());
  ASSERT(symbols.whitelist.selectors.empty());

  // removedList must contain everything that was in the user binary
  ASSERT(!symbols.removedList.classes.empty());
}

// ─────────────────────────────────────────────────────────────────
int main() {
  std::cout << "=== Phase 4 Tests: Symbol Whitelist & Blacklist ===\n\n";

  std::cout << "── Unit tests ──\n";
  RUN(test_setter_name_generation);
  RUN(test_is_setter_detection);
  RUN(test_objcsymbolsets_merge);
  RUN(test_libobjc_selectors_are_always_blacklisted);
  RUN(test_manual_blacklist_respected);
  RUN(test_whitelist_excludes_blacklisted_symbols);
  RUN(test_setter_blacklisting);

  std::cout << "\n── Integration tests (binary: " << objCPath
            << ") ──\n";
  RUN(test_collect_from_test_binary);
  RUN(test_unobfuscable_symbols_removed_from_whitelist);

  std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed
            << " failed ===\n";
  return g_failed > 0 ? 1 : 0;
}
