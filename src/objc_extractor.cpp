#include "MachO2bfuscator/objc_extractor.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

#include "MachO2bfuscator/objc_structs.h"
#include "logger.h"

// ═══════════════════════════════════════════════════════════════
//  Internal implementation
// ═══════════════════════════════════════════════════════════════
namespace {

// ── Safe struct reader ────────────────────────────────────────────
template <typename T>
const T* readStructAt(const MachOSlice& slice, uint64_t fileOffset) {
  if (fileOffset + sizeof(T) > slice.dataSize) {
    throw std::runtime_error("readStructAt: out-of-bounds at offset 0x" + [&] {
      char b[20];
      snprintf(b, sizeof(b), "%llX", (unsigned long long)fileOffset);
      return std::string(b);
    }());
  }
  return reinterpret_cast<const T*>(slice.data + fileOffset);
}

// ── Preferred load address ────────────────────────────────────────
// The vmAddr of the first non-PAGEZERO non-zero segment.
// For iOS arm64 apps this is always 0x100000000 (__TEXT).
uint64_t getPreferredLoadAddress(const MachOSlice& slice) {
  for (const auto& seg : slice.segments) {
    if (seg.name != "__PAGEZERO" && seg.vmAddr != 0) {
      return seg.vmAddr;
    }
  }
  return 0;
}

// ── Decode a chained fixup / PAC encoded pointer ──────────────────
// Converts a raw value stored in ObjC metadata to a usable VM address.
// For old binaries (no chained fixups), the raw value IS the VM address
// and passes through unchanged (default case).
uint64_t decodePointer(uint64_t rawValue, uint16_t pointerFormat,
                       uint64_t preferredLoadAddress) {
  if (rawValue == 0) return 0;

  switch (pointerFormat) {
    // ── DYLD_CHAINED_PTR_64 (format 2) ───────────────────────────
    // Absolute VM address encoded in the pointer.
    // struct dyld_chained_ptr_64_rebase:
    //   target : 36 bits  [35:0]
    //   high8  :  8 bits  [43:36]
    //   bind   :  1 bit   [63]
    case ObjC::DYLD_CHAINED_PTR_64: {
      uint64_t target = rawValue & 0x0000000FFFFFFFFFull;  // bits [35:0]
      uint64_t high8 = (rawValue >> 36) & 0xFF;            // bits [43:36]
      // Reconstruct: high8 goes to bits [39:32] of the final address
      return target | (high8 << 32);
    }

      // ── DYLD_CHAINED_PTR_64_OFFSET (format 6) ────────────────────
      // Target is an OFFSET from the preferred load address.
      // Same bit layout as format 2, but result is relative.
    case ObjC::DYLD_CHAINED_PTR_64_OFFSET: {
      // target = offset from preferredLoadAddress
      // high8 encodes bits [39:32] of the offset for large binaries,
      // but for standard iOS arm64 binaries all offsets fit in 32 bits
      uint64_t target = rawValue & 0x0000000FFFFFFFFFull;
      return target + preferredLoadAddress;
    }

    // ── DYLD_CHAINED_PTR_ARM64E (format 1) ───────────────────────
    // arm64e with pointer authentication.
    case ObjC::DYLD_CHAINED_PTR_ARM64E:
    case ObjC::DYLD_CHAINED_PTR_ARM64E_USERLAND24: {
      bool isBind = (rawValue >> 62) & 1;
      bool isAuth = (rawValue >> 63) & 1;
      if (!isBind && !isAuth) {
        // Plain rebase: target in low 32 bits
        return (rawValue & 0xFFFFFFFFull) + preferredLoadAddress;
      }
      if (!isBind && isAuth) {
        // Authenticated rebase: target in low 32 bits
        return (rawValue & 0xFFFFFFFFull) + preferredLoadAddress;
      }
      return 0;  // bind entry — not a rebase
    }

    // ── No chained fixups — plain VM address ──────────────────────
    default:
      return rawValue;
  }
}

// ── Resolve a raw pointer value to a plain VM address ────────────
uint64_t resolveVmPtr(const MachOSlice& slice, uint64_t rawValue,
                      uint64_t preferredLoadAddr) {
  if (rawValue == 0) return 0;
  uint16_t fmt = slice.dyldInfo ? slice.dyldInfo->pointerFormat : 0;
  return decodePointer(rawValue, fmt, preferredLoadAddr);
}

// ── C-string reader (by file offset) ─────────────────────────────
StringInData readStringAtFile(const MachOSlice& slice, uint64_t fileOffset) {
  if (fileOffset >= slice.dataSize) return {};
  const char* ptr = reinterpret_cast<const char*>(slice.data + fileOffset);
  size_t maxLen = static_cast<size_t>(slice.dataSize - fileOffset);
  size_t len = strnlen(ptr, maxLen);

  StringInData s;
  s.value = std::string(ptr, len);
  s.fileOffset = fileOffset;
  s.length = len;
  return s;
}

// ── C-string reader (by raw pointer value from metadata) ─────────
// Always call this — never call fileOffsetFromVmOffset directly
// on a value read from ObjC metadata, because it may be encoded.
StringInData readStringAtVm(const MachOSlice& slice, uint64_t rawVmValue,
                            uint64_t preferredLoadAddr) {
  uint64_t vmAddr = resolveVmPtr(slice, rawVmValue, preferredLoadAddr);
  if (vmAddr == 0) return {};
  return readStringAtFile(slice, slice.fileOffsetFromVmOffset(vmAddr));
}

// ── Method list reader ────────────────────────────────────────────
template <typename PointerType>
std::vector<ObjcMethod> readMethodList(const MachOSlice& slice,
                                       uint64_t listVmOffset,
                                       uint64_t preferredLoadAddr) {
  if (listVmOffset == 0) return {};

  uint64_t resolvedVm = resolveVmPtr(slice, listVmOffset, preferredLoadAddr);
  if (resolvedVm == 0) return {};

  uint64_t listFileOff = slice.fileOffsetFromVmOffset(resolvedVm);
  const auto* hdr = readStructAt<ObjC::entsize_list_tt>(slice, listFileOff);

  std::vector<ObjcMethod> methods;
  methods.reserve(hdr->count);

  bool isRelative =
      (hdr->entsizeAndFlags & ObjC::METHOD_LIST_IS_UNIQUED_STUBS) != 0;

  if (isRelative) {
    uint64_t cursor = listFileOff + sizeof(ObjC::entsize_list_tt);
    for (uint32_t i = 0; i < hdr->count; ++i) {
      const auto* rel = readStructAt<ObjC::relative_method_t>(slice, cursor);

      ObjcMethod m;

      // ── Resolve name ─────────────���────────────────────────────
      for (const auto& seg : slice.segments) {
        if (cursor >= seg.fileOff && cursor < seg.fileOff + seg.fileSize) {
          uint64_t nameFieldVm = seg.vmAddr + (cursor - seg.fileOff);
          uint64_t nameTargetVm = static_cast<uint64_t>(
              static_cast<int64_t>(nameFieldVm) + rel->nameOffset);

          // nameTargetVm is a PLAIN VM address — use fileOffsetFromVmOffset
          // directly, no decoding needed
          uint64_t nameTargetFile = slice.fileOffsetFromVmOffset(nameTargetVm);

          // BUT: *selPtrPtr IS an encoded pointer (in __objc_selrefs)
          // so decode it via resolveVmPtr
          const auto* selPtrPtr =
              readStructAt<PointerType>(slice, nameTargetFile);
          uint64_t selVm = resolveVmPtr(
              slice, static_cast<uint64_t>(*selPtrPtr), preferredLoadAddr);
          if (selVm != 0) {
            m.name =
                readStringAtFile(slice, slice.fileOffsetFromVmOffset(selVm));
          }
          break;
        }
      }

      // ── Resolve types ─────────────────────────────────────────
      uint64_t typesFieldCursor = cursor + sizeof(int32_t);
      for (const auto& seg : slice.segments) {
        if (typesFieldCursor >= seg.fileOff &&
            typesFieldCursor < seg.fileOff + seg.fileSize) {
          uint64_t typesFieldVm = seg.vmAddr + (typesFieldCursor - seg.fileOff);
          uint64_t typesTargetVm = static_cast<uint64_t>(
              static_cast<int64_t>(typesFieldVm) + rel->typesOffset);

          // typesTargetVm is ALSO a plain VM address — use
          // readStringAtFile directly, no decoding
          m.methType = readStringAtFile(
              slice, slice.fileOffsetFromVmOffset(typesTargetVm));
          break;
        }
      }

      methods.push_back(std::move(m));
      cursor += sizeof(ObjC::relative_method_t);
    }
  } else {
    // ── Classic absolute pointer method list ──────────────────
    uint32_t flagMask = ObjC::METHOD_LIST_FLAG_MASK;
    uint32_t elementSize = hdr->entsizeAndFlags & ~flagMask;

    if (elementSize != sizeof(ObjC::method_t<PointerType>)) return {};

    uint64_t cursor = listFileOff + sizeof(ObjC::entsize_list_tt);
    for (uint32_t i = 0; i < hdr->count; ++i) {
      const auto* raw =
          readStructAt<ObjC::method_t<PointerType>>(slice, cursor);

      ObjcMethod m;
      if (raw->name != 0)
        m.name = readStringAtVm(slice, raw->name, preferredLoadAddr);
      if (raw->types != 0)
        m.methType = readStringAtVm(slice, raw->types, preferredLoadAddr);
      methods.push_back(std::move(m));

      cursor += elementSize;
    }
  }

  return methods;
}

// ── Ivar list reader ──────────────────────────────────────────────
template <typename PointerType>
std::vector<ObjcIvar> readIvarList(const MachOSlice& slice,
                                   uint64_t listVmOffset,
                                   uint64_t preferredLoadAddr) {
  if (listVmOffset == 0) return {};

  uint64_t resolvedVm = resolveVmPtr(slice, listVmOffset, preferredLoadAddr);
  if (resolvedVm == 0) return {};

  uint64_t listFileOff = slice.fileOffsetFromVmOffset(resolvedVm);
  const auto* hdr = readStructAt<ObjC::entsize_list_tt>(slice, listFileOff);

  std::vector<ObjcIvar> ivars;
  ivars.reserve(hdr->count);

  uint32_t elementSize = hdr->entsizeAndFlags & ~0u;
  if (elementSize != sizeof(ObjC::ivar_t<PointerType>)) return {};

  uint64_t cursor = listFileOff + sizeof(ObjC::entsize_list_tt);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    const auto* raw = readStructAt<ObjC::ivar_t<PointerType>>(slice, cursor);

    ObjcIvar iv;
    if (raw->name != 0)
      iv.name = readStringAtVm(slice, raw->name, preferredLoadAddr);
    if (raw->type != 0)
      iv.type = readStringAtVm(slice, raw->type, preferredLoadAddr);
    ivars.push_back(std::move(iv));

    cursor += elementSize;
  }
  return ivars;
}

// ── Property list reader ──────────────────────────────────────────
template <typename PointerType>
std::vector<ObjcProperty> readPropertyList(const MachOSlice& slice,
                                           uint64_t listVmOffset,
                                           uint64_t preferredLoadAddr) {
  if (listVmOffset == 0) return {};

  uint64_t resolvedVm = resolveVmPtr(slice, listVmOffset, preferredLoadAddr);
  if (resolvedVm == 0) return {};

  uint64_t listFileOff = slice.fileOffsetFromVmOffset(resolvedVm);
  const auto* hdr = readStructAt<ObjC::entsize_list_tt>(slice, listFileOff);

  std::vector<ObjcProperty> props;
  props.reserve(hdr->count);

  uint32_t elementSize = hdr->entsizeAndFlags & ~0u;
  if (elementSize != sizeof(ObjC::property_t<PointerType>)) return {};

  uint64_t cursor = listFileOff + sizeof(ObjC::entsize_list_tt);
  for (uint32_t i = 0; i < hdr->count; ++i) {
    const auto* raw =
        readStructAt<ObjC::property_t<PointerType>>(slice, cursor);

    ObjcProperty p;
    if (raw->name != 0)
      p.name = readStringAtVm(slice, raw->name, preferredLoadAddr);
    if (raw->attributes != 0)
      p.attributes = readStringAtVm(slice, raw->attributes, preferredLoadAddr);
    props.push_back(std::move(p));

    cursor += elementSize;
  }
  return props;
}

// ── Single class reader ───────────────────────────────────────────
// Now takes preferredLoadAddr as a third parameter.
template <typename PointerType, typename ClassT, typename ClassROT>
ObjcClass readClass(const MachOSlice& slice, uint64_t classFileOffset,
                    uint64_t preferredLoadAddr)  // ← added
{
  const auto* cls = readStructAt<ClassT>(slice, classFileOffset);

  // bits field may also be an encoded pointer — decode it
  uint64_t rawDataPtr = static_cast<uint64_t>(cls->dataPtr());
  uint64_t roVmAddr = resolveVmPtr(slice, rawDataPtr, preferredLoadAddr);
  uint64_t roFileOff = slice.fileOffsetFromVmOffset(roVmAddr);
  const auto* ro = readStructAt<ClassROT>(slice, roFileOff);

  ObjcClass result;

  if (ro->name != 0) {
    result.name = readStringAtVm(slice, static_cast<uint64_t>(ro->name),
                                 preferredLoadAddr);
  }

  if (ro->ivarLayout != 0) {
    result.ivarLayout = readStringAtVm(
        slice, static_cast<uint64_t>(ro->ivarLayout), preferredLoadAddr);
  }

  if (ro->baseMethodList != 0) {
    result.methods = readMethodList<PointerType>(
        slice, static_cast<uint64_t>(ro->baseMethodList), preferredLoadAddr);
  }

  if (ro->ivars != 0) {
    result.ivars = readIvarList<PointerType>(
        slice, static_cast<uint64_t>(ro->ivars), preferredLoadAddr);
  }
  //
  if (ro->baseProperties != 0) {
    result.properties = readPropertyList<PointerType>(
        slice, static_cast<uint64_t>(ro->baseProperties), preferredLoadAddr);
  }

  return result;
}

// ── Walk a pointer list section ───────────────────────────────────
// Each entry in the section is a PointerType VM value (possibly encoded).
// We decode it before calling the callback.
template <typename PointerType>
void walkPointerList(const MachOSlice& slice, const MachSection* section,
                     uint64_t preferredLoadAddr,  // ← added
                     std::function<void(uint64_t)> callback) {
  if (!section || section->size == 0) return;

  uint64_t cursor = section->fileOffset;
  uint64_t end = section->fileOffset + section->size;

  while (cursor + sizeof(PointerType) <= end) {
    const auto* rawPtr = readStructAt<PointerType>(slice, cursor);
    uint64_t rawValue = static_cast<uint64_t>(*rawPtr);

    // Decode the pointer before resolving to file offset
    uint64_t vmAddr = resolveVmPtr(slice, rawValue, preferredLoadAddr);
    if (vmAddr != 0) {
      try {
        uint64_t fileOff = slice.fileOffsetFromVmOffset(vmAddr);
        callback(fileOff);
      } catch (const std::exception&) {
        // Skip unresolvable entries silently
      }
    }
    cursor += sizeof(PointerType);
  }
}

// ── Architecture-dispatched extraction ───────────────────────────
template <typename PointerType, typename ClassT, typename ClassROT>
ObjcMetadata extractMetadataImpl(const MachOSlice& slice) {
  ObjcMetadata meta;

  // Compute once — passed to every function that reads a pointer
  uint64_t preferredLoadAddr = getPreferredLoadAddress(slice);

  // ── Classes ───────────────────────────────────────────────────
  walkPointerList<PointerType>(
      slice, slice.objcClasslistSection(), preferredLoadAddr,
      [&](uint64_t classFileOff) {
        try {
          auto cls = readClass<PointerType, ClassT, ClassROT>(
              slice, classFileOff, preferredLoadAddr);
          meta.classes.push_back(std::move(cls));
        } catch (const std::exception& e) {
        }
      });

  // ── Categories ────────────────────────────────────────────────
  walkPointerList<PointerType>(
      slice, slice.objcCatlistSection(), preferredLoadAddr,
      [&](uint64_t catFileOff) {
        try {
          const auto* raw =
              readStructAt<ObjC::category_t<PointerType>>(slice, catFileOff);

          ObjcCategory cat;

          if (raw->name != 0)
            cat.name = readStringAtVm(slice, static_cast<uint64_t>(raw->name),
                                      preferredLoadAddr);

          if (raw->cls != 0) {
            uint64_t clsVm = resolveVmPtr(
                slice, static_cast<uint64_t>(raw->cls), preferredLoadAddr);
            if (clsVm != 0) {
              uint64_t clsFileOff = slice.fileOffsetFromVmOffset(clsVm);
              cat.cls = readClass<PointerType, ClassT, ClassROT>(
                  slice, clsFileOff, preferredLoadAddr);
            }
          }

          if (raw->instanceMethods != 0)
            cat.methods = readMethodList<PointerType>(
                slice, static_cast<uint64_t>(raw->instanceMethods),
                preferredLoadAddr);

          if (raw->instanceProperties != 0)
            cat.properties = readPropertyList<PointerType>(
                slice, static_cast<uint64_t>(raw->instanceProperties),
                preferredLoadAddr);

          meta.categories.push_back(std::move(cat));
        } catch (const std::exception&) {
          LOGGER_WARN("Warning: Failed to read category at file offset 0x{}",
                      std::hex, catFileOff);
        }
      });

  // ── Protocols ─────────────────────────────────────────────────
  walkPointerList<PointerType>(
      slice, slice.objcProtocollistSection(), preferredLoadAddr,
      [&](uint64_t protoFileOff) {
        try {
          const auto* raw =
              readStructAt<ObjC::protocol_t<PointerType>>(slice, protoFileOff);

          ObjcProtocol proto;

          if (raw->name != 0)
            proto.name = readStringAtVm(slice, static_cast<uint64_t>(raw->name),
                                        preferredLoadAddr);

          if (raw->instanceMethods != 0)
            proto.methods = readMethodList<PointerType>(
                slice, static_cast<uint64_t>(raw->instanceMethods),
                preferredLoadAddr);

          if (raw->instanceProperties != 0)
            proto.properties = readPropertyList<PointerType>(
                slice, static_cast<uint64_t>(raw->instanceProperties),
                preferredLoadAddr);

          meta.protocols.push_back(std::move(proto));
        } catch (const std::exception&) {
          LOGGER_WARN("Warning: Failed to read protocol at file offset 0x{}",
                      std::hex, protoFileOff);
        }
      });

  return meta;
}

}  // anonymous namespace

// ═══════════════════════════════════════════════════════════════
//  ObjcExtractor public implementations
// ═══════════════════════════════════════════════════════════════

ObjcMetadata ObjcExtractor::extractMetadata(const MachOSlice& slice) {
  if (slice.is64bit) {
    return extractMetadataImpl<uint64_t, ObjC::class_t_64, ObjC::class_ro_64>(
        slice);
  } else {
    return extractMetadataImpl<uint32_t, ObjC::class_t_32, ObjC::class_ro_32>(
        slice);
  }
}

std::vector<StringInData> ObjcExtractor::extractSelectors(
    const MachOSlice& slice) {
  std::vector<StringInData> result;

  const MachSection* sec = slice.objcMethNameSection();
  if (!sec || sec->size == 0) return result;

  uint64_t cursor = sec->fileOffset;
  uint64_t end = sec->fileOffset + sec->size;

  while (cursor < end) {
    const char* ptr = reinterpret_cast<const char*>(slice.data + cursor);
    size_t maxLen = static_cast<size_t>(end - cursor);
    size_t len = strnlen(ptr, maxLen);

    if (len > 0) {
      StringInData s;
      s.value = std::string(ptr, len);
      s.fileOffset = cursor;
      s.length = len;
      result.push_back(std::move(s));
    }

    cursor += len + 1;
  }

  return result;
}

std::vector<StringInData> ObjcExtractor::collectClassNames(
    const MachOSlice& slice, const ObjcMetadata& metadata) {
  std::vector<StringInData> result;

  const MachSection* classNameSec = slice.objcClassNameSection();

  auto isInClassNameSection = [&](const StringInData& s) -> bool {
    if (!classNameSec) return false;
    return s.fileOffset >= classNameSec->fileOffset &&
           s.fileOffset < classNameSec->fileOffset + classNameSec->size;
  };

  for (const auto& cls : metadata.classes) {
    if (!cls.name.value.empty() && !cls.name.isSwiftName())
      result.push_back(cls.name);
  }

  for (const auto& proto : metadata.protocols) {
    if (!proto.name.value.empty() && !proto.name.isSwiftName())
      result.push_back(proto.name);
  }

  for (const auto& cat : metadata.categories) {
    if (!cat.name.value.empty() && !cat.name.isSwiftName() &&
        isInClassNameSection(cat.name)) {
      result.push_back(cat.name);
    }
  }

  return result;
}

const std::unordered_set<std::string>& ObjcExtractor::libobjcSelectors() {
  static const std::unordered_set<std::string> selectors = {
      "load",
      "initialize",
      "resolveInstanceMethod:",
      "resolveClassMethod:",
      ".cxx_construct",
      ".cxx_destruct",
      "retain",
      "release",
      "autorelease",
      "retainCount",
      "alloc",
      "allocWithZone:",
      "dealloc",
      "copy",
      "new",
      "forwardInvocation:",
      "_tryRetain",
      "_isDeallocating",
      "retainWeakReference",
      "allowsWeakReference",
  };
  return selectors;
}
