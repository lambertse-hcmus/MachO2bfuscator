#pragma once

// CliArgs is gone. parseArgs() produces an ObfuscatorConfig directly.
// toConfig() is gone — there is nothing to convert.

#include "MachO2bfuscator/obfuscator.h"

// Parse argc/argv using cxxopts.
// Prints --help and exits 0 if --help is passed.
// Prints an error and exits 1 on bad input.
//
// The returned config has mangler == nullptr and manglerType/caesarKey/
// randomSeed populated from the CLI flags. ObfuscatorPipeline::run()
// calls ensureMangler() to materialise the concrete mangler on first use.
ObfuscatorConfig parseArgs(int argc, char* argv[]);
