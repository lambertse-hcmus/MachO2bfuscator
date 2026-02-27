#include <cstdio>
#include <iomanip>
#include <iostream>

#include "MachO2fuscator/mach_reader.h"
#include "MachO2fuscator/objc_extractor.h"

static void printSlice(const MachOSlice& slice, size_t index) {
  std::cout << "\n══ Slice #" << index << " ("
            << (slice.is64bit ? "64-bit" : "32-bit") << ")"
            << "  cpu=" << slice.cpu.type << "/" << slice.cpu.subtype
            << " ══\n";

  // Print all segments and their sections
  for (const auto& seg : slice.segments) {
    std::cout << "  SEGMENT  " << std::left << std::setw(18) << seg.name
              << "  fileOff=0x" << std::hex << seg.fileOff << "  vmAddr=0x"
              << seg.vmAddr << "  size=0x" << seg.fileSize << std::dec << "\n";

    for (const auto& sec : seg.sections) {
      std::cout << "    section  " << std::left << std::setw(24)
                << sec.sectionName << "  fileOff=0x" << std::hex
                << sec.fileOffset << "  vmAddr=0x" << sec.vmAddr << "  size=0x"
                << sec.size << std::dec << "\n";
    }
  }

  // Print key ObjC sections using the named accessors
  std::cout << "\n  ── Key ObjC sections ──\n";
  auto printNamedSec = [&](const char* label, const MachSection* sec) {
    if (sec) {
      std::cout << "  " << std::left << std::setw(28) << label << "fileOff=0x"
                << std::hex << sec->fileOffset << "  size=0x" << sec->size
                << std::dec << "\n";
    } else {
      std::cout << "  " << std::left << std::setw(28) << label
                << "(not present)\n";
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
    std::cout << "\n  SYMTAB  symOff=0x" << std::hex << slice.symtab->symOff
              << "  nSyms=" << std::dec << slice.symtab->nSyms
              << "  strTable=[0x" << std::hex << slice.symtab->strTable.offset
              << ", size=0x" << slice.symtab->strTable.size << "]\n"
              << std::dec;
  }

  // DyldInfo
  if (slice.dyldInfo) {
    std::cout << "\n  DYLD_INFO\n";
    std::cout << "    bind       [0x" << std::hex << slice.dyldInfo->bind.offset
              << ", size=0x" << slice.dyldInfo->bind.size << "]\n";
    std::cout << "    weakBind   [0x" << slice.dyldInfo->weakBind.offset
              << ", size=0x" << slice.dyldInfo->weakBind.size << "]\n";
    std::cout << "    lazyBind   [0x" << slice.dyldInfo->lazyBind.offset
              << ", size=0x" << slice.dyldInfo->lazyBind.size << "]\n";
    std::cout << "    export     [0x" << slice.dyldInfo->exportRange.offset
              << ", size=0x" << slice.dyldInfo->exportRange.size << "]\n";
    std::cout << std::dec;
  }

  // Dylibs
  if (!slice.dylibs.empty()) {
    std::cout << "\n  Dependent dylibs:\n";
    for (const auto& d : slice.dylibs) {
      std::cout << "    " << d << "\n";
    }
  }

  // Rpaths
  if (!slice.rpaths.empty()) {
    std::cout << "\n  RPaths:\n";
    for (const auto& r : slice.rpaths) {
      std::cout << "    " << r << "\n";
    }
  }
}

static void printMetadata(const MachOSlice& slice, size_t sliceIdx) {
  std::cout << "\n── ObjC Metadata (Slice #" << sliceIdx << ") ──\n";

  ObjcMetadata meta = ObjcExtractor::extractMetadata(slice);

  // ── Classes ───────────────────────────────────────────────────
  std::cout << "\nClasses (" << meta.classes.size() << "):\n";
  for (const auto& cls : meta.classes) {
    std::cout << "  class " << cls.name.value << "  [fileOff=0x" << std::hex
              << cls.name.fileOffset << ", len=" << std::dec << cls.name.length
              << "]\n";
    for (const auto& m : cls.methods) {
      std::cout << "    -" << m.name.value << "  " << m.methType.value << "\n";
    }
    for (const auto& iv : cls.ivars) {
      std::cout << "    ivar " << iv.name.value << "  " << iv.type.value
                << "\n";
    }
    for (const auto& p : cls.properties) {
      std::cout << "    prop " << p.name.value << "  " << p.attributes.value
                << "\n";
    }
  }

  // ── Categories ────────────────────────────────────────────────
  std::cout << "\nCategories (" << meta.categories.size() << "):\n";
  for (const auto& cat : meta.categories) {
    std::string clsName = cat.cls ? cat.cls->name.value : "(external)";
    std::cout << "  " << clsName << "+" << cat.name.value << "\n";
    for (const auto& m : cat.methods) {
      std::cout << "    -" << m.name.value << "  " << m.methType.value << "\n";
    }
  }

  // ── Protocols ─────────────────────────────────────────────────
  std::cout << "\nProtocols (" << meta.protocols.size() << "):\n";
  for (const auto& proto : meta.protocols) {
    std::cout << "  @protocol " << proto.name.value << "\n";
    for (const auto& m : proto.methods) {
      std::cout << "    -" << m.name.value << "\n";
    }
  }

  // ── Selectors ─────────────────────────────────────────────────
  auto selectors = ObjcExtractor::extractSelectors(slice);
  std::cout << "\nSelectors (" << selectors.size() << "):\n";
  for (const auto& sel : selectors) {
    std::cout << "  @selector(" << sel.value << ")"
              << "  [fileOff=0x" << std::hex << sel.fileOffset << "]\n";
  }
  std::cout << std::dec;

  // ── Obfuscatable class names ───────────────────────────────────
  auto classNames = ObjcExtractor::collectClassNames(slice, meta);
  std::cout << "\nObfuscatable class names (" << classNames.size() << "):\n";
  for (const auto& cn : classNames) {
    std::cout << "  " << cn.value << "  [fileOff=0x" << std::hex
              << cn.fileOffset << ", len=" << std::dec << cn.length << "]\n";
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: class_obfuscator <path-to-macho-binary>\n";
    return 1;
  }

  try {
    MachOImage image = loadMachOImage(argv[1]);
    std::cout << "Slices found: " << image.slices.size() << "\n";

    for (size_t i = 0; i < image.slices.size(); ++i) {
      printSlice(image.slices[i], i);
      printMetadata(image.slices[i], i);
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
