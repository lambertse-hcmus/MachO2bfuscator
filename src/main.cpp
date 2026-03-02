#include <iostream>

#include "MachO2bfuscator/obfuscator.h"
#include "cli/cli_parser.h"
#include "logger.h"

// ── printStats ───────────────────────────────────────────────────────
static void printStats(const ObfuscatorStats& s) {
  LOGGER_INFO(
      "[✓] Obfuscation complete\n"
      "    images processed   : {}\n"
      "    whitelist selectors: {}\n"
      "    whitelist classes  : {}\n"
      "    blacklist selectors: {}\n"
      "    mangled selectors  : {}\n"
      "    mangled classes    : {}\n"
      "    selector patches   : {}\n"
      "    class patches      : {}\n"
      "    methtype patches   : {}",
      s.imagesProcessed, s.whitelistSelectors, s.whitelistClasses,
      s.blacklistSelectors, s.mangledSelectors, s.mangledClasses,
      s.selectorPatches, s.classPatches, s.methTypePatches);
}

// ── main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
  logger::init();
  ObfuscatorConfig cfg = parseArgs(argc, argv);

  if (cfg.verbose) {
    LOGGER_INFO("Mangler type: {}", cfg.manglerType);
    if (cfg.manglerType == "caesar") {
      LOGGER_INFO("Caesar key: {}", static_cast<int>(cfg.caesarKey));
    } else if (cfg.manglerType == "random") {
      LOGGER_INFO("Random seed: {}", cfg.randomSeed);
    } else if (cfg.manglerType == "realwords") {
      LOGGER_INFO("Using real words mangler");
    } else {
      LOGGER_INFO("Hold on, unknown mangler type: {}", cfg.manglerType);
    }
    LOGGER_INFO("[*] Images : {}", cfg.images.size());
    if (cfg.dryRun) LOGGER_INFO("[*] Dry-run mode — no files will be written");
  }

  try {
    ObfuscatorPipeline pipeline(cfg);
    ObfuscatorStats stats = pipeline.run();

    if (cfg.verbose) {
      printStats(stats);
    } else if (!cfg.dryRun) {
      LOGGER_INFO(
          "[✓] Done — {} selector(s), {} class(es) patched across {} image(s).",
          stats.selectorPatches, stats.classPatches, stats.imagesProcessed);
    } else {
      LOGGER_INFO(
          "[✓] Dry run — {} selector(s), {} class(es) would be obfuscated.",
          stats.mangledSelectors, stats.mangledClasses);
    }
  } catch (const std::exception& e) {
    LOGGER_ERROR("Error: {}", e.what());
    return 1;
  }

  return 0;
}
