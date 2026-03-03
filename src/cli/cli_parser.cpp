#include "cli_parser.h"

#include <cxxopts.hpp>
#include <fstream>
#include <iostream>
#include <memory>

#include "../logger.h"
#include "MachO2bfuscator/mangler.h"
#include "cli_parser.h"

// ── loadFilterFile ────────────────────────────────────────────────
// Reads a plain-text file of symbol names, one per line.
// Blank lines and lines starting with '#' are ignored.
// Throws std::runtime_error if the file cannot be opened.
static std::unordered_set<std::string> loadFilterFile(const std::string& path) {
  std::unordered_set<std::string> result;
  std::ifstream f(path);
  if (!f) {
    throw std::runtime_error("Cannot open filter file: " + path);
  }
  std::string line;
  while (std::getline(f, line)) {
    // Trim trailing CR (Windows line endings)
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // Skip blank lines and comments
    if (line.empty() || line[0] == '#') continue;
    result.insert(line);
  }
  return result;
}

// ── parseArgs ────────────────────────────────────────────────────────
// Produces an ObfuscatorConfig directly — no intermediate CliArgs.
// The mangler field is left null; manglerType/caesarKey/randomSeed
// are set instead. ObfuscatorPipeline::ensureMangler() builds it.
ObfuscatorConfig parseArgs(int argc, char* argv[]) {
  cxxopts::Options options("MachO2fuscation",
                           "Obfuscate Objective-C class/selector metadata "
                           "inside a Mach-O binary.\n");

  // clang-format off
    options.add_options()
        // ── Mangler ──────────────────────────────────────────────────
        ("m,mangler",
            "Mangler: 'caesar', 'random', or 'realwords' (default: realwords)",
            cxxopts::value<std::string>()->default_value("realwords"))

        ("caesar-key",
            "Shift key for CaesarMangler, 1–25 (default: 13)",
            cxxopts::value<uint8_t>()->default_value("13"))

        ("seed",
            "PRNG seed for RandomMangler (0 = new random seed each run)",
            cxxopts::value<uint32_t>()->default_value("0"))

        // ── Output ───────────────────────────────────────────────────
        ("o,output",
            "Write obfuscated binary to this path (in-place if omitted)",
            cxxopts::value<std::string>()->default_value(""))

        // ── Erase options ─────────────────────────────────────────────
        ("erase-methtype",
            "Zero out the entire __objc_methtype section after patching")

        ("erase-symtab",
            "Erase symbol table entries for obfuscated symbols (default: ON)")

        ("preserve-symtab",
            "Keep symbol table entries unchanged (overrides --erase-symtab)")

        // ── Blacklists ────────────────────────────────────────────────
        ("blacklist-selector",
            "Selector name that must NOT be obfuscated (repeatable)",
            cxxopts::value<std::vector<std::string>>())

        ("blacklist-class",
            "Class name that must NOT be obfuscated (repeatable)",
            cxxopts::value<std::vector<std::string>>())

        // ── Dependencies ──────────────────────────────────────────────
        ("d,dependency",
            "Dependency binary whose symbols are blacklisted (repeatable)",
            cxxopts::value<std::vector<std::string>>())

        // ── Misc ──────────────────────────────────────────────────────
        ("n,dry-run",  "Analyse and report without writing any output file")
        ("log-level",
            "Logging level: 'info' (default), 'debug', or 'verbose'",
            cxxopts::value<std::string>()->default_value("info"))
        ("h,help",     "Print this help message and exit")

        // ── Positional ────────────────────────────────────────────────
        ("inputs",
            "One or more Mach-O binaries to obfuscate",
            cxxopts::value<std::vector<std::string>>())

          // ── Explicit obfuscation filter lists ─────────────────────────
        ("class-filter-file",
            "Path to a file listing class names to obfuscate (one per line). "
            "When provided, ONLY these classes are obfuscated (intersected "
            "with the whitelist). Blacklisted classes are still protected.",
            cxxopts::value<std::string>()->default_value(""))

        ("selector-filter-file",
            "Path to a file listing selector names to obfuscate (one per "
            "line). When provided, ONLY these selectors are obfuscated "
            "(intersected with the whitelist). Blacklisted selectors are "
            "still protected.",
            cxxopts::value<std::string>()->default_value(""));
  // clang-format on

  options.parse_positional({"inputs"});
  options.positional_help("<binary> [<binary> ...]");

  // ── Parse ─────────────────────────────────────────────────────────
  cxxopts::ParseResult result;
  try {
    result = options.parse(argc, argv);
  } catch (const cxxopts::exceptions::parsing& e) {
    LOGGER_ERROR("Error parsing options: {}", e.what());
    std::exit(1);
  }

  if (result.count("help")) {
    std::cout << options.help() << "\n";
    std::exit(0);
  }

  // ── Validate ──────────────────────────────────────────────────────
  if (!result.count("inputs")) {
    LOGGER_ERROR("At least one input binary is required.");
    std::exit(1);
  }

  const std::string manglerTypeStr = result["mangler"].as<std::string>();
  if (manglerTypeStr != "caesar" && manglerTypeStr != "random" &&
      manglerTypeStr != "realwords") {
    LOGGER_ERROR("Invalid mangler type: '{}'",
                 result["mangler"].as<std::string>());
    std::exit(1);
  }
  const ManglerType manglerType = parseManglerType(manglerTypeStr);

  const std::string output = result["output"].as<std::string>();
  const auto inputs = result["inputs"].as<std::vector<std::string>>();
  if (!output.empty() && inputs.size() > 1) {
    LOGGER_ERROR(
        "--output (-o) can only be used with a single input. Got {} inputs.",
        inputs.size());
    std::exit(1);
  }

  // ── Build ObfuscatorConfig directly ──────────────────────────────
  ObfuscatorConfig cfg;

  // Images
  if (!output.empty()) {
    cfg.images.push_back({inputs[0], output});
  } else {
    for (const auto& p : inputs) cfg.images.push_back({p, p});  // in-place
  }

  cfg.manglerType = manglerType;
  if (manglerType == ManglerType::Caesar) {
    const uint8_t caesarKey = result["caesar-key"].as<uint8_t>();
    if (caesarKey == 0 || caesarKey >= 26) {
      LOGGER_ERROR("--caesar-key must be 1–25, got {}",
                   static_cast<int>(caesarKey));
      std::exit(1);
    }
    cfg.manglerConfig.caesarKey = caesarKey;
    cfg.mangler = std::make_shared<CaesarMangler>();
  } else if (manglerType == ManglerType::Random) {
    cfg.manglerConfig.randomSeed = result["seed"].as<uint32_t>();
    cfg.mangler = std::make_shared<RandomMangler>();
  } else if (manglerType == ManglerType::RealWords) {
    cfg.mangler = std::make_shared<RealWordsMangler>();
  }

  // Dependencies
  if (result.count("dependency"))
    cfg.dependencyPaths = result["dependency"].as<std::vector<std::string>>();

  // Erase options
  cfg.eraseMethType = result.count("erase-methtype") > 0;
  cfg.eraseSymtab = !(result.count("preserve-symtab") > 0 &&
                      result.count("erase-symtab") == 0);

  // Blacklists — cxxopts gives us vector, unordered_set absorbs it directly
  if (result.count("blacklist-selector")) {
    const auto& v = result["blacklist-selector"].as<std::vector<std::string>>();
    cfg.manualSelectorBlacklist.insert(v.begin(), v.end());
  }
  if (result.count("blacklist-class")) {
    const auto& v = result["blacklist-class"].as<std::vector<std::string>>();
    cfg.manualClassBlacklist.insert(v.begin(), v.end());
  }

  // ── Class filter file ──────────────────────────────────────────
  {
    auto path = result["class-filter-file"].as<std::string>();
    if (!path.empty()) {
      cfg.classFilterList = loadFilterFile(path);
      LOGGER_INFO("class-filter-file: loaded {} entries from '{}'",
                  cfg.classFilterList.size(), path);
    }
  }

  // ── Selector filter file ───────────────────────────────────────
  {
    auto path = result["selector-filter-file"].as<std::string>();
    if (!path.empty()) {
      cfg.selectorFilterList = loadFilterFile(path);
      LOGGER_INFO("selector-filter-file: loaded {} entries from '{}'",
                  cfg.selectorFilterList.size(), path);
    }
  }

  // Misc
  cfg.dryRun = result.count("dry-run") > 0;

  const std::string logLevelStr = result["log-level"].as<std::string>();
  if (logLevelStr == "info") {
    cfg.logLevel = LOG_LEVEL_FROM_INFO;
  } else if (logLevelStr == "debug") {
    cfg.logLevel = LOG_LEVEL_FROM_DEBUG;
  } else if (logLevelStr == "verbose") {
    cfg.logLevel = LOG_LEVEL_VERBOSE;
  } else {
    LOGGER_ERROR("Invalid log level: '{}'", logLevelStr);
    cfg.logLevel = LOG_LEVEL_FROM_INFO;
  }
  return cfg;
}
