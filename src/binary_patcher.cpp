#include "MachO2fuscator/binary_patcher.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

// ═══════════════════════════════════════════════════════════════
//  MethTypeObfuscator
// ═══════════════════════════════════════════════════════════════

MethTypeObfuscator::MethTypeObfuscator(
    const std::unordered_map<std::string, std::string>& classNamesMap)
    : classNamesMap_(classNamesMap) {}

// ── replaceDelimited ─────────────────────────────────────────────
// Mirrors Swift: String.replacing(of:precededBy:followedBy:with:)
//
// Replaces all occurrences of open+oldName+close with open+newName+close.
// Example: replaceDelimited(s, "MyClass", "ObfClass", '"', '"')
//   changes @"MyClass" → @"ObfClass"
std::string MethTypeObfuscator::replaceDelimited(const std::string& input,
                                                 const std::string& oldName,
                                                 const std::string& newName,
                                                 char open, char close) {
  std::string result = input;

  std::string pattern;
  pattern += open;
  pattern += oldName;
  pattern += close;

  std::string replacement;
  replacement += open;
  replacement += newName;
  replacement += close;

  size_t pos = 0;
  while ((pos = result.find(pattern, pos)) != std::string::npos) {
    result.replace(pos, pattern.size(), replacement);
    pos += replacement.size();
  }
  return result;
}

// ── MethTypeObfuscator::obfuscate ────────────────────────────────
// Mirrors Swift: MethTypeObfuscator.generateObfuscatedMethType(methType:)
//
// For each class name in the map, replace all occurrences surrounded
// by any of the 5 delimiter pairs. Returns input unchanged if no
// substitutions were made.
std::string MethTypeObfuscator::obfuscate(const std::string& methType) const {
  std::string result = methType;

  for (const auto& [oldName, newName] : classNamesMap_) {
    // Fast early-exit — mirrors Swift:
    // guard curResult.contains(mapping.key) else { return curResult }
    if (result.find(oldName) == std::string::npos) continue;

    // Try all 5 delimiter pairs — mirrors Swift:
    // surroundedByAny: [("\"","\""),("(",")"),("[","]"),("<",">"),("{","}")]
    result = replaceDelimited(result, oldName, newName, '"', '"');
    result = replaceDelimited(result, oldName, newName, '(', ')');
    result = replaceDelimited(result, oldName, newName, '[', ']');
    result = replaceDelimited(result, oldName, newName, '<', '>');
    result = replaceDelimited(result, oldName, newName, '{', '}');
  }

  return result;
}

// ═══════════════════════════════════════════════════════════════
//  BinaryPatcher helpers
// ═════════════════════════════���═════════════════════════════════

// ── replaceStringInPlace ─────────────────────────────────────────
// Mirrors Swift: data.replaceRangeWithPadding(range, with: newValue)
//
// Writes newValue bytes at fileOffset in slice.data, then NUL-pads
// up to origLen bytes of content + 1 NUL terminator.
// If newValue.size() > origLen: silently truncates to origLen.
void BinaryPatcher::replaceStringInPlace(MachOSlice& slice, uint64_t fileOffset,
                                         const std::string& newValue,
                                         size_t origLen) {
  if (fileOffset + origLen + 1 > slice.dataSize) return;

  uint8_t* dest = slice.data + fileOffset;
  size_t writeLen = std::min(newValue.size(), origLen);

  // Write new content
  std::memcpy(dest, newValue.data(), writeLen);

  // NUL-pad the rest — covers terminator and any gap when
  // newValue is shorter than origLen
  std::memset(dest + writeLen, 0, origLen + 1 - writeLen);
}

// ═══════════════════════════════════════════════════════════════
//  BinaryPatcher — patching steps
// ═══════════════════════════════════════════════════════════════

// ── patchMethTypes ────────────────────────────────────────────────
// Mirrors Swift:
//   data.replaceStrings(inRange: methTypeSection...,
//       withMapping: MethTypeObfuscator(withMap: map)
//                       .generateObfuscatedMethType(methType:))
uint32_t BinaryPatcher::patchMethTypes(MachOSlice& slice,
                                       const ManglingMap& map) {
  const MachSection* sec = slice.objcMethTypeSection();
  if (!sec || sec->size == 0) return 0;

  MethTypeObfuscator obfuscator(map.classNames);
  uint32_t count = 0;

  uint64_t cursor = sec->fileOffset;
  uint64_t end = sec->fileOffset + sec->size;

  while (cursor < end) {
    const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
    size_t maxLen = static_cast<size_t>(end - cursor);
    size_t origLen = strnlen(ptr, maxLen);

    if (origLen > 0) {
      std::string original(ptr, origLen);
      std::string obfuscated = obfuscator.obfuscate(original);

      if (obfuscated != original) {
        replaceStringInPlace(slice, cursor, obfuscated, origLen);
        ++count;
      }
    }

    cursor += origLen + 1;
  }

  return count;
}

// ── patchSelectors ────────────────────────────────────────────────
// Mirrors Swift:
//   data.replaceStrings(inRange: methNameSection...,
//       withMapping: map.selectors)
uint32_t BinaryPatcher::patchSelectors(MachOSlice& slice,
                                       const ManglingMap& map) {
  const MachSection* sec = slice.objcMethNameSection();
  if (!sec || sec->size == 0) return 0;

  uint32_t count = 0;

  uint64_t cursor = sec->fileOffset;
  uint64_t end = sec->fileOffset + sec->size;

  while (cursor < end) {
    const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
    size_t maxLen = static_cast<size_t>(end - cursor);
    size_t origLen = strnlen(ptr, maxLen);

    if (origLen > 0) {
      std::string original(ptr, origLen);
      auto it = map.selectors.find(original);
      if (it != map.selectors.end()) {
        replaceStringInPlace(slice, cursor, it->second, origLen);
        ++count;
      }
    }

    cursor += origLen + 1;
  }

  return count;
}

// ── patchClassNames ───────────────────────────────────────────────
// Mirrors Swift:
//   classNamesInData.forEach { classNameInData in
//       if let obfuscatedName = map.classNames[classNameInData.value] {
//           data.replaceRangeWithPadding(classNameInData.range, ...) } }
uint32_t BinaryPatcher::patchClassNames(MachOSlice& slice,
                                        const ManglingMap& map) {
  const MachSection* sec = slice.objcClassNameSection();
  if (!sec || sec->size == 0) return 0;

  uint32_t count = 0;

  uint64_t cursor = sec->fileOffset;
  uint64_t end = sec->fileOffset + sec->size;

  while (cursor < end) {
    const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
    size_t maxLen = static_cast<size_t>(end - cursor);
    size_t origLen = strnlen(ptr, maxLen);

    if (origLen > 0) {
      std::string original(ptr, origLen);
      auto it = map.classNames.find(original);
      if (it != map.classNames.end()) {
        replaceStringInPlace(slice, cursor, it->second, origLen);
        ++count;
      }
    }

    cursor += origLen + 1;
  }

  return count;
}

// ═══════════════════════════════════════════════════════════════
//  BinaryPatcher — public entry points
// ═══════════════════════════════════════════════════════════════

// ── patch ─────────────────────────────────────────────────────────
// Patches a single slice in-place.
// Mirrors Swift: Mach.replaceSymbols(withMap:...)
//
// Order: methtype → selectors → classnames
// Mirrors Swift comment: "Obfuscate from more specific to less specific"
PatchResult BinaryPatcher::patch(MachOSlice& slice, const ManglingMap& map) {
  PatchResult result;
  result.methTypePatches = patchMethTypes(slice, map);
  result.selectorPatches = patchSelectors(slice, map);
  result.classPatches = patchClassNames(slice, map);
  return result;
}

// ── patchFile ─────────────────────────────────────────────────────
// Loads a binary from disk, patches all slices in-place via the
// shared rawData buffer, then writes the whole buffer back to disk.
//
// Key insight: slice.data is a non-owning pointer INTO image.rawData.
// Patching slice.data therefore patches image.rawData directly.
// No copy-back step is needed — just write rawData after all patches
//
PatchResult BinaryPatcher::patchFile(const std::string& srcPath,
                                     const std::string& dstPath,
                                     const ManglingMap& map) {
  // ── Load from srcPath ─────────────────────────────────────────
  // Slices are non-owning views into image.rawData.
  // Patching slices patches rawData in-place.
  MachOImage image = loadMachOImage(srcPath);

  // ── Patch each slice in-place ─────────────────────────────────
  PatchResult total;
  for (auto& slice : image.slices) {
    PatchResult r = patch(slice, map);
    total.selectorPatches += r.selectorPatches;
    total.classPatches += r.classPatches;
    total.methTypePatches += r.methTypePatches;
  }

  // ── Write patched buffer to dstPath ───────────────────────────
  std::ofstream out(dstPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("patchFile: cannot open for writing '" + dstPath +
                             "'");
  }
  out.write(reinterpret_cast<const char*>(image.rawData),
            static_cast<std::streamsize>(image.rawDataSize));
  if (!out) {
    throw std::runtime_error("patchFile: write failed for '" + dstPath + "'");
  }

  return total;
}
