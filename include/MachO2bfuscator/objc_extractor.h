#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "mach_reader.h"
#include "objc_types.h"

// ═══════════════════════════════════════════════════════════════
//  ObjcExtractor
//
//  Reads raw ObjC metadata structs from a MachOSlice and produces
//  rich ObjcMetadata objects (Phase 2 types).
//
// ═══════════════════════════════════════════════════════════════
class ObjcExtractor {
 public:
  // ── Main entry point ─────────────────────────────────────────
  // Extract all ObjC classes, categories, and protocols from one slice.
  static ObjcMetadata extractMetadata(const MachOSlice& slice);

  // ── Selector extraction ───────────────────────────────────────
  // Reads all selector strings directly from __objc_methname section.
  // Returns pairs of (selectorName, fileOffset) so Phase 6 can patch them.
  static std::vector<StringInData> extractSelectors(const MachOSlice& slice);

  // ── Class name extraction (direct string scan) ────────────────
  // Reads all class/protocol name strings directly from __objc_classname.
  // This mirrors extractSelectors() exactly — no pointer resolution.
  // Works for BOTH app binaries AND framework dylibs from shared cache.
  // Use this instead of / in addition to collectClassNames() for dependencies.
  static std::vector<StringInData> extractClassNamesFromSection(
      const MachOSlice& slice);

  // ── Class name collection ─────────────────────────────────────
  // Collects all obfuscatable class names from already-extracted metadata.
  //
  // Rules (same as original):
  //  - Include class names from objcClasses
  //  - Include protocol names from objcProtocols
  //  - Include category names ONLY if they live in __objc_classname section
  //    (i.e. pure ObjC categories, not Swift extensions)
  //  - Exclude any name that starts with "_Tt" (Swift mangled names)
  static std::vector<StringInData> collectClassNames(
      const MachOSlice& slice, const ObjcMetadata& metadata);

  // ── libobjc built-in selectors ────────────────────────────────
  // These must never be obfuscated (they are used by the runtime itself).
  static const std::unordered_set<std::string>& libobjcSelectors();
  static const std::unordered_set<std::string>& libobjcClasses();
};
