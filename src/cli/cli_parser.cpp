#include "cli_parser.h"

#include <cxxopts.hpp>
#include <memory>
#include <iostream>

#include "../logger.h"
#include "cli_parser.h"

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
            "Mangler to use: 'caesar' or 'random' (default: random)",
            cxxopts::value<std::string>()->default_value("random"))

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
        ("v,verbose",  "Print detailed progress and statistics")
        ("h,help",     "Print this help message and exit")

        // ── Positional ────────────────────────────────────────────────
        ("inputs",
            "One or more Mach-O binaries to obfuscate",
            cxxopts::value<std::vector<std::string>>());
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

  const std::string manglerType = result["mangler"].as<std::string>();
  if (manglerType != "caesar" && manglerType != "random") {
    LOGGER_ERROR("--mangler must be 'caesar' or 'random', got '{}'",
                 manglerType);
    std::exit(1);
  }

  const uint8_t caesarKey = result["caesar-key"].as<uint8_t>();
  if (caesarKey == 0 || caesarKey >= 26) {
    LOGGER_ERROR("--caesar-key must be 1–25, got {}",
                 static_cast<int>(caesarKey));
    std::exit(1);
  }

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

  // Mangler spec — mangler ptr stays null; pipeline builds it via
  // ensureMangler()
  cfg.manglerType = manglerType;
  cfg.caesarKey = caesarKey;
  cfg.randomSeed = result["seed"].as<uint32_t>();

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

  // Misc
  cfg.dryRun = result.count("dry-run") > 0;
  cfg.verbose = result.count("verbose") > 0;

  return cfg;
}
