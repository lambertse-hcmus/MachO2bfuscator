#pragma once

#include <string>
#include <unordered_map>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/mangling_map.h"

// ═══════════════════════════════════════════════════════════════
//  MethTypeObfuscator
//
//  ObjC method type strings encode parameter and return types like:
//    "v16@0:8"          → void, id, SEL
//    "v32@0:8@\"MyClass\"16"  → void, id, SEL, MyClass*
//
//  When a class is renamed, any methtype string that contains that
//  class name surrounded by special delimiters must also be updated.
//
//  Delimiters (mirrors Swift exactly):
//    "ClassName"  →  between double quotes
//    (ClassName)  →  between parentheses
//    [ClassName]  →  between square brackets
//    <ClassName>  →  between angle brackets
//    {ClassName}  →  between curly braces
// ═══════════════════════════════════════════════════════════════
class MethTypeObfuscator {
 public:
  explicit MethTypeObfuscator(
      const std::unordered_map<std::string, std::string>& classNamesMap);

  // Returns the obfuscated methtype string, or the input unchanged
  // if no class names in the mapping appear in the string.
  std::string obfuscate(const std::string& methType) const;

 private:
  std::unordered_map<std::string, std::string> classNamesMap_;

  // Replace occurrences of oldName surrounded by (open, close)
  // with newName surrounded by the same delimiters.
  static std::string replaceDelimited(const std::string& input,
                                      const std::string& oldName,
                                      const std::string& newName, char open,
                                      char close);
};

// ═══════════════════════════════════════════════════════════════
//  PatchResult — statistics returned from a patch operation
// ═══════════════════════════════════════════════════════════════
struct PatchResult {
  uint32_t selectorPatches = 0;
  uint32_t classPatches = 0;
  uint32_t methTypePatches = 0;
};

// ═══════════════════════════════════════════════════════════════
//  BinaryPatcher
//
//  Takes a loaded MachOImage (with its file buffer), applies the
//  ManglingMap to patch all relevant sections in-place, then
//  writes the modified buffer back to disk.
//
//  Patching order (mirrors Swift comment:
//  "Obfuscate from more specific to less specific objects"):
//    1. __objc_methtype  — patch class names inside type encodings
//    2. __objc_methname  — patch selector strings
//    3. __objc_classname — patch class name strings
//
// ═══════════════════════════════════════════════════════════════
class BinaryPatcher {
 public:
  // ── Patch a single slice buffer in-place ──────────────────────
  static PatchResult patch(MachOSlice& slice, const ManglingMap& map);

  // ── Patch a binary file on disk ───────────────────────────────────
  static PatchResult patchFile(const std::string& srcPath,
                               const std::string& dstPath,
                               const ManglingMap& map);

 private:
  // ── Step 1: patch __objc_methtype ────────────────────────────
  static uint32_t patchMethTypes(MachOSlice& slice, const ManglingMap& map);

  // ── Step 2: patch __objc_methname ────────────────────────────
  static uint32_t patchSelectors(MachOSlice& slice, const ManglingMap& map);

  // ── Step 3: patch __objc_classname ───────────────────────────
  static uint32_t patchClassNames(MachOSlice& slice, const ManglingMap& map);

  // ── Helper: replace a NUL-terminated string at fileOffset ────
  static void replaceStringInPlace(MachOSlice& slice, uint64_t fileOffset,
                                   const std::string& newValue, size_t origLen);
};
