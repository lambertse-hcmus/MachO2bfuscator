#include <cstdio>
#include <iomanip>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/objc_extractor.h"
#include "logger.h"

static void printSlice(const MachOSlice& slice, size_t index) {
  LOGGER_INFO("=== Slice #{} ({}-bit) cpu={}/{} ===", index,
              slice.is64bit ? 64 : 32, slice.cpu.type, slice.cpu.subtype);
  // Print all segments and their sections
  for (const auto& seg : slice.segments) {
    LOGGER_INFO("  SEGMENT  {}  fileOff=0x{:X}  vmAddr=0x{:X}  size=0x{:X}",
                seg.name, seg.fileOff, seg.vmAddr, seg.fileSize);

    for (const auto& sec : seg.sections) {
      LOGGER_INFO("    section  {}  fileOff=0x{:X}  vmAddr=0x{:X}  size=0x{:X}",
                  sec.sectionName, sec.fileOffset, sec.vmAddr, sec.size);
    }
  }

  // Print key ObjC sections using the named accessors
  LOGGER_INFO("  === Key ObjC sections ===");
  auto printNamedSec = [&](const char* label, const MachSection* sec) {
    if (sec) {
      LOGGER_INFO("  {}  fileOff=0x{:X}  size=0x{:X}", label, sec->fileOffset,
                  sec->size);
    } else {
      LOGGER_INFO("  {}  (not present)", label);
    }
  };
  printNamedSec("__objc_classname", slice.objcClassNameSection());
  printNamedSec("__objc_methname", slice.objcMethNameSection());
  printNamedSec("__objc_methtype", slice.objcMethTypeSection());
  printNamedSec("__objc_classlist", slice.objcClasslistSection());
  printNamedSec("__objc_catlist", slice.objcCatlistSection());
  printNamedSec("__objc_protolist", slice.objcProtocollistSection());
  printNamedSec("__cstring", slice.cstringSection());

  // Symtab
  if (slice.symtab) {
    LOGGER_INFO(
        "  SYMTAB symOff=0x{:X}  nSyms={}  strTable=[0x{:X}, size=0x{:X}]",
        slice.symtab->symOff, slice.symtab->nSyms,
        slice.symtab->strTable.offset, slice.symtab->strTable.size);
  }

  // DyldInfo
  if (slice.dyldInfo) {
    LOGGER_INFO("  DYLD_INFO");
    LOGGER_INFO("    bind       [0x{:X}, size=0x{:X}]",
                slice.dyldInfo->bind.offset, slice.dyldInfo->bind.size);
    LOGGER_INFO("    weakBind   [0x{:X}, size=0x{:X}]",
                slice.dyldInfo->weakBind.offset, slice.dyldInfo->weakBind.size);
    LOGGER_INFO("    lazyBind   [0x{:X}, size=0x{:X}]",
                slice.dyldInfo->lazyBind.offset, slice.dyldInfo->lazyBind.size);
    LOGGER_INFO("    export     [0x{:X}, size=0x{:X}]",
                slice.dyldInfo->exportRange.offset,
                slice.dyldInfo->exportRange.size);
  }

  // Dylibs
  if (!slice.dylibs.empty()) {
    LOGGER_INFO("  Dependent dylibs:");
    for (const auto& d : slice.dylibs) {
      LOGGER_INFO("    {}", d);
    }
  }

  // Rpaths
  if (!slice.rpaths.empty()) {
    LOGGER_INFO("  RPaths:");
    for (const auto& r : slice.rpaths) {
      LOGGER_INFO("    {}", r);
    }
  }
}

static void printMetadata(const MachOSlice& slice, size_t sliceIdx) {
  LOGGER_INFO("--- ObjC Metadata (Slice #{}) ---", sliceIdx);
  ObjcMetadata meta = ObjcExtractor::extractMetadata(slice);

  // ── Classes ───────────────────────────────────────────────────
  LOGGER_INFO("Classes ({}):", meta.classes.size());
  for (const auto& cls : meta.classes) {
    LOGGER_INFO(
        "  class {}  [fileOff=0x{:X}, len={}]  ivars={}  methods={}  "
        "properties={}",
        cls.name.value, cls.name.fileOffset, cls.name.length, cls.ivars.size(),
        cls.methods.size(), cls.properties.size());
    for (const auto& m : cls.methods) {
      LOGGER_INFO("    -{}  {}", m.name.value, m.methType.value);
    }
    for (const auto& iv : cls.ivars) {
      LOGGER_INFO("    ivar {}  {}", iv.name.value, iv.type.value);
    }
    for (const auto& p : cls.properties) {
      LOGGER_INFO("    prop {}  {}", p.name.value, p.attributes.value);
    }
  }

  // ── Categories ────────────────────────────────────────────────
  LOGGER_INFO("Categories ({}):", meta.categories.size());
  for (const auto& cat : meta.categories) {
    std::string clsName = cat.cls ? cat.cls->name.value : "(external)";
    LOGGER_INFO("  {}+{}  [fileOff=0x{:X}, len={}]  methods={}  properties={}",
                clsName, cat.name.value, cat.name.fileOffset, cat.name.length,
                cat.methods.size(), cat.properties.size());
    for (const auto& m : cat.methods) {
      LOGGER_INFO("    -{}  {}", m.name.value, m.methType.value);
    }
  }

  // ── Protocols ─────────────────────────────────────────────────
  LOGGER_INFO("Protocols ({}):", meta.protocols.size());
  for (const auto& proto : meta.protocols) {
    LOGGER_INFO("  @protocol {}", proto.name.value);
    for (const auto& m : proto.methods) {
      LOGGER_INFO("    -{}", m.name.value);
    }
  }

  // ── Selectors ─────────────────────────────────────────────────
  auto selectors = ObjcExtractor::extractSelectors(slice);
  LOGGER_INFO("Selectors ({}):", selectors.size());
  for (const auto& sel : selectors) {
    LOGGER_INFO("  @selector({})  [fileOff=0x{:X}]", sel.value, sel.fileOffset);
  }

  // ── Obfuscatable class names ───────────────────────────────────
  auto classNames = ObjcExtractor::collectClassNames(slice, meta);
  LOGGER_INFO("Obfuscatable class names ({}):", classNames.size());
  for (const auto& cn : classNames) {
    LOGGER_INFO("  {}  [fileOff=0x{:X}, len={}]", cn.value, cn.fileOffset,
                cn.length);
  }
}

int main(int argc, char* argv[]) {
  logger::init(LOG_LEVEL_INFO);
  if (argc < 2) {
    LOGGER_INFO("Usage: {} <path-to-macho-binary>", argv[0]);
    return 1;
  }

  try {
    MachOImage image = loadMachOImage(argv[1]);
    LOGGER_INFO("Slices found: {}", image.slices.size());

    for (size_t i = 0; i < image.slices.size(); ++i) {
      printSlice(image.slices[i], i);
      printMetadata(image.slices[i], i);
    }
  } catch (const std::exception& e) {
    LOGGER_ERROR("Failed to load MachO image '{}': {}", argv[1], e.what());
    return 1;
  }
  return 0;
}
