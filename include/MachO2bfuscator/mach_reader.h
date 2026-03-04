#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

// 'fileOffset' is the offset from the start of THIS slice's data.
// 'vmAddr' is the virtual address as stored in the binary.
// 'size' is the byte count of the section.
struct MachSection {
  std::string segmentName;  // e.g. "__TEXT"
  std::string sectionName;  // e.g. "__objc_classname"
  uint64_t fileOffset = 0;
  uint64_t vmAddr = 0;
  uint64_t size = 0;

  FileRange fileRange() const { return {fileOffset, size}; }
};

// ── Segment ──────────────────────────────────────────────────────
struct MachSegment {
  std::string name;
  uint64_t vmAddr = 0;
  uint64_t vmSize = 0;
  uint64_t fileOff = 0;
  uint64_t fileSize = 0;
  std::vector<MachSection> sections;

  FileRange fileRange() const { return {fileOff, fileSize}; }
  FileRange vmRange() const { return {vmAddr, vmSize}; }
};

// ── Symtab ───────────────────────────────────────────────────────
struct MachSymtab {
  uint64_t symOff = 0;  
  uint32_t nSyms = 0;   // number of symbol entries
  FileRange strTable;   // file range of the string table
};

// ── CPU identifier ───────────────────────────────────────────────
struct MachCpu {
  int32_t type = 0;
  int32_t subtype = 0;
};

// ── DyldInfo — extended for chained fixups ───────────────────────
struct MachDyldInfo {
  FileRange bind;
  FileRange weakBind;
  FileRange lazyBind;
  FileRange exportRange;

  // LC_DYLD_CHAINED_FIXUPS (Xcode 13+)
  FileRange chainedFixups;     // raw chained fixup data range
  uint16_t pointerFormat = 0;  // DYLD_CHAINED_PTR_* value, 0 = unknown
  bool hasChainedFixups = false;
};

// ── A single Mach-O slice ────────────────────────────────────────
struct MachOSlice {
  BytePtr data = nullptr;
  uint64_t dataSize = 0;

  bool is64bit = false;
  MachCpu cpu;

  enum class FileType { Executable, Other };
  FileType fileType = FileType::Other;

  std::vector<MachSegment> segments;
  std::optional<MachSymtab> symtab;
  std::optional<MachDyldInfo> dyldInfo;

  std::vector<std::string> dylibs;  // LC_LOAD_DYLIB paths
  std::vector<std::string> rpaths;  // LC_RPATH values

  // ── Convenience section lookup ──────────────────────────────
  const MachSection* findSection(const std::string& segName,
                                 const std::string& secName) const;
  const MachSection* findSectionInAnySegment(
      const std::initializer_list<const char*>& segNames,
      const std::string& secName) const;

  const MachSection* objcClassNameSection() const;
  const MachSection* objcMethNameSection() const;
  const MachSection* objcMethTypeSection() const;
  const MachSection* objcClasslistSection() const;
  const MachSection* objcCatlistSection() const;
  const MachSection* objcProtocollistSection() const;
  const MachSection* cstringSection() const;

  // ── VM address → file offset translation ────────────────────
  // Returns the byte offset within THIS slice's data buffer.
  uint64_t fileOffsetFromVmOffset(uint64_t vmOffset) const;

  // Convenience: pointer directly into the buffer at a VM address
  BytePtr pointerFromVmOffset(uint64_t vmOffset) const;

  // Convenience: pointer directly at a file offset within this slice
  BytePtr pointerAtFileOffset(uint64_t fileOffset) const;
};

// ── Top-level image (fat or thin) ───────────────────────────────
struct MachOImage {
  std::string path;

  // Fat binaries have one slice per architecture.
  std::vector<MachOSlice> slices;

  // The raw file bytes (owned, from mmap or malloc)
  BytePtr rawData = nullptr;
  uint64_t rawDataSize = 0;
  bool isMmapped = false;  // true → must munmap, false → must free

  MachOImage() = default;
  ~MachOImage();

  // Non-copyable (owns raw memory)
  MachOImage(const MachOImage&) = delete;
  MachOImage& operator=(const MachOImage&) = delete;

  // Movable
  MachOImage(MachOImage&&) noexcept;
  MachOImage& operator=(MachOImage&&) noexcept;
};

MachOImage loadMachOImage(const std::string& path);
