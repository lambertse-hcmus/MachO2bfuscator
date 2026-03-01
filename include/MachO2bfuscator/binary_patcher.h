#pragma once

#include <string>
#include <unordered_map>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/mangling_map.h"

// ═══════════════════════════════════════════════════════════════
//  MethTypeObfuscator
//
//  Mirrors Swift: struct MethTypeObfuscator in Mach+Replacing.swift
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
  // Build from a ManglingMap — uses classNames mapping only
  explicit MethTypeObfuscator(
      const std::unordered_map<std::string, std::string>& classNamesMap);

  // Mirrors Swift: generateObfuscatedMethType(methType:)
  // Returns the obfuscated methtype string, or the input unchanged
  // if no class names in the mapping appear in the string.
  std::string obfuscate(const std::string& methType) const;

 private:
  std::unordered_map<std::string, std::string> classNamesMap_;

  // Replace occurrences of oldName surrounded by (open, close)
  // with newName surrounded by the same delimiters.
  // Mirrors Swift: String.replacing(of:precededBy:followedBy:with:)
  static std::string replaceDelimited(const std::string& input,
                                      const std::string& oldName,
                                      const std::string& newName, char open,
                                      char close);
};

// ═══════════════════════════════════════════════════════════════
//  PatchResult — statistics returned from a patch operation
// ═══════════════════════════════════════════════════════════════
struct PatchResult {
  uint32_t selectorPatches = 0;  // number of selector strings patched
  uint32_t classPatches = 0;     // number of class name strings patched
  uint32_t methTypePatches = 0;  // number of methtype strings patched
};

// ═══════════════════════════════════════════════════════════════
//  BinaryPatcher
//
//  Mirrors Swift: Mach.replaceSymbols(withMap:imageURL:paths:)
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
//  Same-length invariant:
//    Every replacement must be the same byte length as the original.
//    If a mangled name is shorter, it is NUL-padded to fill the gap.
//    If a mangled name is longer, it is silently skipped (should
//    never happen if Phase 5 enforced the length constraint).
// ═══════════════════════════════════════════════════════════════
class BinaryPatcher {
 public:
  // ── Patch a single slice buffer in-place ──────────────────────
  // Mirrors Swift: Mach.replaceSymbols(withMap:...)
  //
  // Modifies the bytes pointed to by slice.data directly.
  // The caller must ensure slice.data is writable (i.e. the
  // buffer was allocated by us, not mmap'd read-only).
  static PatchResult patch(MachOSlice& slice, const ManglingMap& map);

  // ── Patch a binary file on disk ───────────────────────────────────
  // Reads from srcPath, patches all slices, writes result to dstPath.
  // srcPath and dstPath may be the same (in-place patch) but it is
  // strongly recommended to use different paths so the original is
  // preserved if anything goes wrong.
  static PatchResult patchFile(const std::string& srcPath,
                               const std::string& dstPath,
                               const ManglingMap& map);

 private:
  // ── Step 1: patch __objc_methtype ────────────────────────────
  // Mirrors Swift:
  //   data.replaceStrings(inRange: methTypeSection...,
  //       withMapping: MethTypeObfuscator(...).generateObfuscatedMethType)
  //
  // Class names embedded in methtype strings are replaced using
  // MethTypeObfuscator. Because methtype strings contain type
  // encodings that may be longer or shorter than the class name
  // alone, we use NUL-padding to fill any size difference.
  static uint32_t patchMethTypes(MachOSlice& slice, const ManglingMap& map);

  // ── Step 2: patch __objc_methname ────────────────────────────
  // Mirrors Swift:
  //   data.replaceStrings(inRange: methNameSection...,
  //       withMapping: map.selectors)
  //
  // Each NUL-terminated selector string in __objc_methname is
  // compared against the selectors map. If found, the bytes are
  // overwritten in-place. The replacement is always the same
  // length (enforced by Phase 5), so no padding is needed.
  static uint32_t patchSelectors(MachOSlice& slice, const ManglingMap& map);

  // ── Step 3: patch __objc_classname ───────────────────────────
  // Mirrors Swift:
  //   classNamesInData.forEach { classNameInData in
  //       data.replaceRangeWithPadding(classNameInData.range, ...) }
  //
  // Class names are NUL-terminated strings in __objc_classname.
  // We walk the section and replace each matching name in-place,
  // NUL-padding if the new name is shorter.
  static uint32_t patchClassNames(MachOSlice& slice, const ManglingMap& map);

  // ── Helper: replace a NUL-terminated string at fileOffset ────
  // Writes newValue over the bytes at fileOffset in slice.data,
  // followed by a NUL terminator, padding any remaining bytes
  // up to origLen with NUL bytes.
  static void replaceStringInPlace(MachOSlice& slice, uint64_t fileOffset,
                                   const std::string& newValue, size_t origLen);
};
