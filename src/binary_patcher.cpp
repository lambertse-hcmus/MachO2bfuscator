#include "MachO2bfuscator/binary_patcher.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "logger.h"

// ═══════════════════════════════════════════════════════════════
//  MethTypeObfuscator
// ═══════════════════════════════════════════════════════════════

MethTypeObfuscator::MethTypeObfuscator(
    const std::unordered_map<std::string, std::string>& classNamesMap)
    : classNamesMap_(classNamesMap) {}

// ── replaceDelimited ─────────────────────────────────────────────
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
//
std::string MethTypeObfuscator::obfuscate(const std::string& methType) const {
  std::string result = methType;

  for (const auto& [oldName, newName] : classNamesMap_) {
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
        LOGGER_INFO("map methType: '{}' → '{}'", original, obfuscated);
        replaceStringInPlace(slice, cursor, obfuscated, origLen);
        ++count;
      }
    }

    cursor += origLen + 1;
  }

  return count;
}

// ── patchSelectors ────────────────────────────────────────────────
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
PatchResult BinaryPatcher::patch(MachOSlice& slice, const ManglingMap& map) {
  PatchResult result;
  {
    for (const auto& [orig, mangled] : map.selectors) {
      LOGGER_INFO("map selector: '{}' → '{}'", orig, mangled);
    }
    for (const auto& [orig, mangled] : map.classNames) {
      LOGGER_INFO("map class: '{}' → '{}'", orig, mangled);
    }
  }
  result.methTypePatches = patchMethTypes(slice, map);
  result.selectorPatches = patchSelectors(slice, map);
  result.classPatches = patchClassNames(slice, map);
  return result;
}

// ── patchFile ─────────────────────────────────────────────────────
// Loads a binary from disk, patches all slices in-place via the
// shared rawData buffer, then writes the whole buffer back to disk.
PatchResult BinaryPatcher::patchFile(const std::string& srcPath,
                                     const std::string& dstPath,
                                     const ManglingMap& map) {
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
