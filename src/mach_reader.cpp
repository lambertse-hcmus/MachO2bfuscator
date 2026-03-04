#include "MachO2bfuscator/mach_reader.h"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <utility>

// POSIX / macOS system headers
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Apple Mach-O headers — all struct do
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/swap.h>

//
#include "MachO2bfuscator/objc_structs.h"

#ifndef LC_DYLD_EXPORTS_TRIE
#define LC_DYLD_EXPORTS_TRIE 0x80000033u
#endif

#ifndef LC_DYLD_CHAINED_FIXUPS
#define LC_DYLD_CHAINED_FIXUPS 0x80000034u
#endif

namespace {

// ── Safe pointer-cast helper ─────────────────────────────────────
// Returns a const pointer to T at byte offset 'off' within 'base'.
// Performs a bounds check against 'totalSize'.
template <typename T>
const T* getStructAt(const BytePtr base, uint64_t off, uint64_t totalSize) {
  if (off + sizeof(T) > totalSize) {
    throw MachLoadError("getStructAt: out-of-bounds read at offset " +
                        std::to_string(off));
  }
  return reinterpret_cast<const T*>(base + off);
}

// Non-const overload for in-place editing (Phase 6 will use this heavily)
template <typename T>
T* getMutableStructAt(BytePtr base, uint64_t off, uint64_t totalSize) {
  if (off + sizeof(T) > totalSize) {
    throw MachLoadError("getMutableStructAt: out-of-bounds read at offset " +
                        std::to_string(off));
  }
  return reinterpret_cast<T*>(base + off);
}

// ── Null-terminated C-string reader ──────────────────────────────
std::string getCStringAt(const BytePtr base, uint64_t off, uint64_t totalSize) {
  if (off >= totalSize) {
    throw MachLoadError("getCStringAt: offset out of bounds");
  }
  // strnlen guards against running off the end of the buffer
  const char* ptr = reinterpret_cast<const char*>(base + off);
  size_t maxLen = static_cast<size_t>(totalSize - off);
  size_t len = strnlen(ptr, maxLen);
  return std::string(ptr, len);
}

// ── 16-byte name helper (segment/section names are char[16]) ─────
// The names in Mach-O structs are fixed-width char[16], NOT null-terminated
// if they are exactly 16 characters long. We trim to the first null byte.
std::string fixedName(const char* name, size_t maxLen = 16) {
  return std::string(name, strnlen(name, maxLen));
}

// ══════════════════════════════════════════════���════════════════
//  Slice parsers — one for 32-bit, one for 64-bit
// ═══════════════════════════════════════════════════════════════

// ── Common load-command walk ──────────────────────────────────────
// Both 32 and 64-bit binaries share the same load_command layout
// after their respective headers. This function does the walk and
// populates the slice fields.
// 'base'      — start of THIS slice's bytes
// 'sliceSize' — byte count of the slice
// 'cursor'    — starting offset (right after the mach_header / mach_header_64)
// 'ncmds'     — number of load commands from the header
void parseLoadCommands(MachOSlice& slice, BytePtr base, uint64_t sliceSize,
                       uint64_t cursor, uint32_t ncmds) {
  for (uint32_t i = 0; i < ncmds; ++i) {
    // Every load command starts with a load_command { cmd, cmdsize }
    const auto* lc = getStructAt<load_command>(base, cursor, sliceSize);

    switch (lc->cmd) {
      // ── 64-bit segment ───────────────────────────────────────
      case LC_SEGMENT_64: {
        const auto* seg =
            getStructAt<segment_command_64>(base, cursor, sliceSize);

        MachSegment ms;
        ms.name = fixedName(seg->segname);
        ms.vmAddr = seg->vmaddr;
        ms.vmSize = seg->vmsize;
        ms.fileOff = seg->fileoff;
        ms.fileSize = seg->filesize;

        // Sections immediately follow the segment_command_64
        uint64_t secCursor = cursor + sizeof(segment_command_64);
        for (uint32_t s = 0; s < seg->nsects; ++s) {
          const auto* sec = getStructAt<section_64>(base, secCursor, sliceSize);

          MachSection msc;
          msc.segmentName = fixedName(sec->segname);
          msc.sectionName = fixedName(sec->sectname);
          msc.fileOffset = sec->offset;
          msc.vmAddr = sec->addr;
          msc.size = sec->size;
          ms.sections.push_back(msc);

          secCursor += sizeof(section_64);
        }
        slice.segments.push_back(std::move(ms));
        break;
      }

      // ── 32-bit segment ───────────────────────────────────────
      case LC_SEGMENT: {
        const auto* seg = getStructAt<segment_command>(base, cursor, sliceSize);

        MachSegment ms;
        ms.name = fixedName(seg->segname);
        ms.vmAddr = seg->vmaddr;
        ms.vmSize = seg->vmsize;
        ms.fileOff = seg->fileoff;
        ms.fileSize = seg->filesize;

        uint64_t secCursor = cursor + sizeof(segment_command);
        for (uint32_t s = 0; s < seg->nsects; ++s) {
          const auto* sec = getStructAt<section>(base, secCursor, sliceSize);

          MachSection msc;
          msc.segmentName = fixedName(sec->segname);
          msc.sectionName = fixedName(sec->sectname);
          msc.fileOffset = sec->offset;
          msc.vmAddr = sec->addr;
          msc.size = sec->size;
          ms.sections.push_back(msc);

          secCursor += sizeof(section);
        }
        slice.segments.push_back(std::move(ms));
        break;
      }

      // ── Symbol table ─────────────────────────────────────────
      case LC_SYMTAB: {
        const auto* st = getStructAt<symtab_command>(base, cursor, sliceSize);
        MachSymtab symtab;
        symtab.symOff = st->symoff;
        symtab.nSyms = st->nsyms;
        symtab.strTable = {st->stroff, st->strsize};
        slice.symtab = symtab;
        break;
      }

      // ── Dyld info (export trie + binding opcodes) ─────────────
      case LC_DYLD_INFO_ONLY: {
        const auto* di =
            getStructAt<dyld_info_command>(base, cursor, sliceSize);
        MachDyldInfo info;
        info.bind = {di->bind_off, di->bind_size};
        info.weakBind = {di->weak_bind_off, di->weak_bind_size};
        info.lazyBind = {di->lazy_bind_off, di->lazy_bind_size};
        info.exportRange = {di->export_off, di->export_size};
        slice.dyldInfo = info;
        break;
      }
        // LC_DYLD_EXPORTS_TRIE — used by newer Xcode (13+) as replacement for
      // the export portion of LC_DYLD_INFO_ONLY
      case LC_DYLD_EXPORTS_TRIE: {
        const auto* cmd =
            getStructAt<linkedit_data_command>(base, cursor, sliceSize);

        MachDyldInfo info = slice.dyldInfo.value_or(MachDyldInfo{});
        info.exportRange = {cmd->dataoff, cmd->datasize};
        slice.dyldInfo = info;
        break;
      }

      // ── Dependent dylib paths ────────────────────────────────
      case LC_LOAD_DYLIB:
      case LC_LOAD_WEAK_DYLIB:
      case LC_LOAD_UPWARD_DYLIB:
      case LC_REEXPORT_DYLIB: {
        const auto* dl = getStructAt<dylib_command>(base, cursor, sliceSize);
        // The path string starts at cursor + dl->dylib.name.offset
        uint64_t nameOff = cursor + dl->dylib.name.offset;
        slice.dylibs.push_back(getCStringAt(base, nameOff, sliceSize));
        break;
      }

      // ── Rpath entries ────────────────────────────────────────
      case LC_RPATH: {
        const auto* rp = getStructAt<rpath_command>(base, cursor, sliceSize);
        uint64_t pathOff = cursor + rp->path.offset;
        slice.rpaths.push_back(getCStringAt(base, pathOff, sliceSize));
        break;
      }

      case LC_DYLD_CHAINED_FIXUPS: {
        const auto* cmd =
            getStructAt<linkedit_data_command>(base, cursor, sliceSize);

        MachDyldInfo info = slice.dyldInfo.value_or(MachDyldInfo{});
        info.chainedFixups = {cmd->dataoff, cmd->datasize};
        info.hasChainedFixups = true;

        // ── Parse pointer format from chained fixup data ──────────
        // Layout (all offsets relative to the start of the fixup data
        // i.e. base + cmd->dataoff):
        //
        //  [dyld_chained_fixups_header]
        //      .starts_offset → offset to dyld_chained_starts_in_image
        //                        (relative to header start)
        //  [dyld_chained_starts_in_image]
        //      .seg_count
        //      .seg_info_offset[i] → offset to dyld_chained_starts_in_segment
        //                            (relative to starts_in_image start)
        //  [dyld_chained_starts_in_segment]
        //      .pointer_format  ← what we want

        uint64_t fixupBase = cmd->dataoff;  // file offset of fixup data start

        if (cmd->datasize >= sizeof(ObjC::dyld_chained_fixups_header)) {
          const auto* hdr = getStructAt<ObjC::dyld_chained_fixups_header>(
              base, fixupBase, sliceSize);

          // starts_offset is relative to fixupBase
          uint64_t startsOff = fixupBase + hdr->starts_offset;

          if (startsOff + sizeof(ObjC::dyld_chained_starts_in_image) <=
              sliceSize) {
            const auto* starts =
                getStructAt<ObjC::dyld_chained_starts_in_image>(base, startsOff,
                                                                sliceSize);

            // Walk each segment entry looking for a non-zero pointer_format
            for (uint32_t s = 0; s < starts->seg_count; ++s) {
              uint32_t segInfoOff = starts->seg_info_offset[s];
              if (segInfoOff == 0) continue;  // no fixups in this segment

              // seg_info_offset[i] is relative to startsOff
              uint64_t segStartsOff = startsOff + segInfoOff;

              if (segStartsOff + sizeof(ObjC::dyld_chained_starts_in_segment) >
                  sliceSize)
                continue;

              const auto* segStarts =
                  getStructAt<ObjC::dyld_chained_starts_in_segment>(
                      base, segStartsOff, sliceSize);

              if (segStarts->pointer_format != 0) {
                info.pointerFormat = segStarts->pointer_format;
                break;  // all segments use the same format
              }
            }
          }
        }

        slice.dyldInfo = info;
        break;
      }
      default:
        // Ignore unrecognised load commands — same as Swift's 'default: break'
        break;
    }

    // Advance to the next load command
    if (lc->cmdsize == 0) {
      throw MachLoadError("load_command with cmdsize == 0 (malformed binary)");
    }
    cursor += lc->cmdsize;
  }
}

// ── Parse a 64-bit Mach-O slice ──────────────────────────────────
MachOSlice parseSlice64(BytePtr base, uint64_t sliceSize) {
  const auto* hdr = getStructAt<mach_header_64>(base, 0, sliceSize);

  MachOSlice slice;
  slice.data = base;
  slice.dataSize = sliceSize;
  slice.is64bit = true;
  slice.cpu = {hdr->cputype, hdr->cpusubtype};
  slice.fileType = (hdr->filetype == MH_EXECUTE)
                       ? MachOSlice::FileType::Executable
                       : MachOSlice::FileType::Other;

  uint64_t cursor = sizeof(mach_header_64);
  parseLoadCommands(slice, base, sliceSize, cursor, hdr->ncmds);
  return slice;
}

// ── Parse a 32-bit Mach-O slice ──────────────────────────────────
MachOSlice parseSlice32(BytePtr base, uint64_t sliceSize) {
  const auto* hdr = getStructAt<mach_header>(base, 0, sliceSize);

  MachOSlice slice;
  slice.data = base;
  slice.dataSize = sliceSize;
  slice.is64bit = false;
  slice.cpu = {hdr->cputype, hdr->cpusubtype};
  slice.fileType = (hdr->filetype == MH_EXECUTE)
                       ? MachOSlice::FileType::Executable
                       : MachOSlice::FileType::Other;

  uint64_t cursor = sizeof(mach_header);
  parseLoadCommands(slice, base, sliceSize, cursor, hdr->ncmds);
  return slice;
}

// ── Dispatch to 32 or 64 based on magic ──────────────────────────
MachOSlice parseSlice(BytePtr base, uint64_t sliceSize) {
  if (sliceSize < 4) {
    throw MachLoadError("Slice too small to contain a magic number");
  }
  uint32_t magic = *reinterpret_cast<const uint32_t*>(base);

  switch (magic) {
    case MH_MAGIC_64:
      return parseSlice64(base, sliceSize);
    case MH_MAGIC:
      return parseSlice32(base, sliceSize);
    default:
      throw MachLoadError("Unsupported Mach-O magic: 0x" + [&]() {
        char buf[12];
        snprintf(buf, sizeof(buf), "%08X", magic);
        return std::string(buf);
      }());
  }
}

// ── Parse a fat (universal) binary ───────────────────────────────
//
// Note: fat_header and fat_arch fields are stored in BIG-ENDIAN order
// regardless of host architecture. We must byte-swap them on little-endian
// hosts. OSSwapBigToHostInt32 handles this portably.
std::vector<MachOSlice> parseFatBinary(BytePtr base, uint64_t totalSize) {
  const auto* fatHdr = getStructAt<fat_header>(base, 0, totalSize);

  // fat_header.nfat_arch is big-endian
  uint32_t nArch = OSSwapBigToHostInt32(fatHdr->nfat_arch);

  std::vector<MachOSlice> slices;
  slices.reserve(nArch);

  uint64_t archCursor = sizeof(fat_header);
  for (uint32_t i = 0; i < nArch; ++i) {
    const auto* arch = getStructAt<fat_arch>(base, archCursor, totalSize);

    // fat_arch fields are big-endian
    uint32_t sliceOffset = OSSwapBigToHostInt32(arch->offset);
    uint32_t sliceSize = OSSwapBigToHostInt32(arch->size);

    if (static_cast<uint64_t>(sliceOffset) + sliceSize > totalSize) {
      throw MachLoadError("Fat arch slice extends beyond file bounds");
    }

    BytePtr sliceBase = base + sliceOffset;
    slices.push_back(parseSlice(sliceBase, sliceSize));

    archCursor += sizeof(fat_arch);
  }
  return slices;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
//  MachOSlice method implementations
// ═══════════════════════════════════════════════════════════════

const MachSection* MachOSlice::findSection(const std::string& segName,
                                           const std::string& secName) const {
  for (const auto& seg : segments) {
    if (seg.name == segName) {
      for (const auto& sec : seg.sections) {
        if (sec.sectionName == secName) {
          return &sec;
        }
      }
    }
  }
  return nullptr;
}

// Search a section in multiple possible segments.
// Returns the first match found.
const MachSection* MachOSlice::findSectionInAnySegment(
    const std::initializer_list<const char*>& segNames,
    const std::string& secName) const {
  for (const char* segName : segNames) {
    const MachSection* sec = findSection(segName, secName);
    if (sec) return sec;
  }
  return nullptr;
}

// Named section accessors
const MachSection* MachOSlice::objcClassNameSection() const {
  return findSection("__TEXT", "__objc_classname");
}
const MachSection* MachOSlice::objcMethNameSection() const {
  return findSection("__TEXT", "__objc_methname");
}
const MachSection* MachOSlice::objcMethTypeSection() const {
  return findSection("__TEXT", "__objc_methtype");
}
const MachSection* MachOSlice::objcCatlistSection() const {
  return findSectionInAnySegment({"__DATA_CONST", "__DATA"}, "__objc_catlist");
}
const MachSection* MachOSlice::cstringSection() const {
  return findSection("__TEXT", "__cstring");
}

// From iOS 14+ / Xcode 12+, Apple moved these sections to __DATA_CONST for
// performance. Both segment names must be tried.
const MachSection* MachOSlice::objcClasslistSection() const {
  return findSectionInAnySegment({"__DATA_CONST", "__DATA"},
                                 "__objc_classlist");
}
const MachSection* MachOSlice::objcProtocollistSection() const {
  return findSectionInAnySegment({"__DATA_CONST", "__DATA"},
                                 "__objc_protolist");
}

// ── VM address → file offset ─────────────────────────────────────
// Iterates segments to find which one contains the vmOffset,
// then computes: fileOff + (vmOffset - vmAddr)
uint64_t MachOSlice::fileOffsetFromVmOffset(uint64_t vmOffset) const {
  for (const auto& seg : segments) {
    if (vmOffset >= seg.vmAddr && vmOffset < (seg.vmAddr + seg.vmSize)) {
      return seg.fileOff + (vmOffset - seg.vmAddr);
    }
  }
  throw MachLoadError("vmOffset 0x" + [&]() {
    char buf[20];
    snprintf(buf, sizeof(buf), "%llX", (unsigned long long)vmOffset);
    return std::string(buf);
  }() + " does not fall within any segment");
}

BytePtr MachOSlice::pointerFromVmOffset(uint64_t vmOffset) const {
  return data + fileOffsetFromVmOffset(vmOffset);
}

BytePtr MachOSlice::pointerAtFileOffset(uint64_t fileOffset) const {
  if (fileOffset >= dataSize) {
    throw MachLoadError("fileOffset out of slice bounds");
  }
  return data + fileOffset;
}

// ═══════════════════════════════════════════════════════════════
//  MachOImage lifecycle
// ═══════════════════════════════════════════════════════════════

MachOImage::~MachOImage() {
  if (rawData) {
    if (isMmapped) {
      munmap(rawData, static_cast<size_t>(rawDataSize));
    } else {
      free(rawData);
    }
    rawData = nullptr;
  }
}

MachOImage::MachOImage(MachOImage&& o) noexcept
    : path(std::move(o.path)),
      slices(std::move(o.slices)),
      rawData(o.rawData),
      rawDataSize(o.rawDataSize),
      isMmapped(o.isMmapped) {
  o.rawData = nullptr;
  o.rawDataSize = 0;
}

MachOImage& MachOImage::operator=(MachOImage&& o) noexcept {
  if (this != &o) {
    if (rawData) {
      if (isMmapped)
        munmap(rawData, static_cast<size_t>(rawDataSize));
      else
        free(rawData);
    }
    path = std::move(o.path);
    slices = std::move(o.slices);
    rawData = o.rawData;
    rawDataSize = o.rawDataSize;
    isMmapped = o.isMmapped;
    o.rawData = nullptr;
    o.rawDataSize = 0;
  }
  return *this;
}

// ═══════════════════════════════════════════════════════════════
//  Public entry point: loadMachOImage
// ═══════════════════════════════════════════════════════════════

MachOImage loadMachOImage(const std::string& path) {
  // ── Open file ─────────────────────────────────────────────────
  int fd =
      open(path.c_str(), O_RDWR);  
  if (fd < 0) {
    throw MachLoadError("Cannot open file: " + path);
  }

  // ── Stat for size ─────────────────────────────────────────────
  struct stat st{};
  if (fstat(fd, &st) != 0) {
    close(fd);
    throw MachLoadError("Cannot stat file: " + path);
  }

  if (st.st_size == 0) {
    close(fd);
    throw MachLoadError("File is empty: " + path);
  }

  uint64_t fileSize = static_cast<uint64_t>(st.st_size);

  // ── mmap the whole file (read-write, shared) ──────────────────
  // MAP_PRIVATE to not touch the original file, infuture if want in-place edits
  // using MAP_SHARED means our in-place edits (Phase 6) will be written back to
  // the file when we msync + munmap.
  void* mapped = mmap(nullptr, static_cast<size_t>(fileSize),
                      PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);  

  if (mapped == MAP_FAILED) {
    throw MachLoadError("mmap failed for file: " + path);
  }

  BytePtr base = static_cast<BytePtr>(mapped);

  // ── Read magic ────────────────────────────────────────────────
  if (fileSize < 4) {
    munmap(mapped, static_cast<size_t>(fileSize));
    throw MachLoadError("File too small to be a Mach-O binary: " + path);
  }

  uint32_t magic = *reinterpret_cast<const uint32_t*>(base);

  // ── Build the MachOImage ──────────────────────────────────────
  MachOImage image;
  image.path = path;
  image.rawData = base;
  image.rawDataSize = fileSize;
  image.isMmapped = true;

  switch (magic) {
    // ── Fat (universal) binary ────────────────────────────────────
    // FAT_MAGIC is the big-endian magic; FAT_CIGAM is its byte-swapped form.
    // On a little-endian host (x86_64, arm64 running macOS), we always
    // read FAT_CIGAM because the fat header is stored big-endian.
    case FAT_MAGIC:    // 0xCAFEBABE  (big-endian host)
    case FAT_CIGAM: {  // 0xBEBAFECA  (little-endian host)
      image.slices = parseFatBinary(base, fileSize);
      break;
    }

    // ── Thin (single-arch) binary ─────────────────────────────────
    case MH_MAGIC:     // 32-bit native endian
    case MH_MAGIC_64:  // 64-bit native endian
      image.slices.push_back(parseSlice(base, fileSize));
      break;

    default:
      munmap(mapped, static_cast<size_t>(fileSize));
      image.rawData = nullptr;  // prevent double-free in destructor
      throw MachLoadError("Unsupported binary magic in file: " + path);
  }

  return image;
}
