# 📋 C++ Clone Plan: ObjC Class Metadata Obfuscation

---

# Completed Phases
## Phase 1 — Binary Loading (mach_reader.h/cpp)

Loads a Mach-O or fat binary from disk into memory. Parses the load commands, segments, sections, CPU type, dyld info, and symtab. Produces a MachOImage containing one MachOSlice per architecture.
Code
```
disk file  →  MachOImage { slices[ MachOSlice { segments, sections, dyldInfo } ] }
```
## Phase 2 — ObjC Struct Definitions (objc_structs.h, objc_types.h)

Defines the C++ representations of the Objective-C runtime metadata structures as they exist on disk inside the binary. No logic — just data layout.
Code
```
C++ structs mirroring:
  class_t_64 / class_t_32
  class_ro_64 / class_ro_32
  method_t, ivar_t, property_t
  category_t, protocol_t
  relative_method_t
  entsize_list_tt
  DYLD_CHAINED_PTR_* constants
```

## Phase 3 — ObjC Metadata Extraction (objc_extractor.h/cpp)

Walks the ObjC metadata sections inside a slice and extracts all classes, categories, protocols, selectors, and class names. Handles chained fixup pointer decoding, relative method lists, and classic absolute pointer method lists.
Code
```
MachOSlice  →  ObjcMetadata { classes, categories, protocols }
            →  selectors    [ StringInData ]
            →  classNames   [ StringInData ]
```

## Phase 4 — Symbol Whitelist & Blacklist (symbol_sets.h/cpp)

Collects symbols from all obfuscable binaries (user symbols) and all dependency binaries (system symbols). Builds a whitelist of symbols safe to rename and a blacklist of symbols that must never be touched.
Code
```
obfuscable binaries  →  userSymbols
dependency binaries  →  systemSymbols
                              │
                              ▼
whitelist  =  userSymbols  -  blacklist
blacklist  =  systemSymbols  +  libobjcSelectors  +  setterDerivations  +  manualBlacklist
removedList = userSymbols  ∩  blacklist   (diagnostics only)

```
## Phase 5 — Symbol Mangling (mangler.h/cpp)

Takes the whitelist from Phase 4 and produces a flat ManglingMap — a dictionary of oldName → newName. Two manglers are available:


```
CaesarMangler — deterministic Caesar cipher, same length guaranteed, setter-aware
RandomMangler — random alphanumeric replacement, seeded PRNG, setter/getter consistency enforced

```
Code
```

ObfuscationSymbols.whitelist  →  ManglingMap { selectors{old→new}, classNames{old→new} }

```
## Phase 6 — Binary Patching (binary_patcher.h/cpp)

Takes the ManglingMap from Phase 5 and patches bytes in-place inside the binary file buffer. Three sections are patched in order:


```
__objc_methtype — class names inside method type encodings
__objc_methname — selector name strings
__objc_classname — class name strings

```
Code


```
MachOSlice (writable buffer)  +  ManglingMap  →  patched bytes in-place
patchFile(srcPath, dstPath, map)              →  patched binary written to dstPath

```

## Phase 7 — Pipeline Orchestrator (obfuscator.h/cpp)

Wires Phases 1–6 together into a single ObfuscatorPipeline::run() call. Accepts an ObfuscatorConfig describing what to obfuscate, what to blacklist, which mangler to use, and optional erase operations.
Code
```

ObfuscatorConfig
  { images[(src,dst)], dependencies, mangler, eraseMethType, eraseSymtab, dryRun }
        │
        ▼
Phase 4+5: SymbolsCollector → ManglingMap   (once, shared across all images)
        │
        └── for each image:
              Phase 1: loadMachOImage(src)
              Phase 6: BinaryPatcher::patch()
              optional: erase __objc_methtype
              optional: erase symtab
              write → dst

```

# Remaining Phases
## Phase 8 — CLI Entry Point (src/main.cpp)

Parse command-line arguments and build an ObfuscatorConfig, then call ObfuscatorPipeline::run(). 

```
argv  →  ObfuscatorConfig  →  ObfuscatorPipeline::run()  →  exit code

```
Key flags to support:
Flag	Maps to
-m caesar / -m random	config.mangler
--erase-methtype	config.eraseMethType
--erase-symtab / --preserve-symtab	config.eraseSymtab
--dry-run	config.dryRun
--blacklist-selector	config.manualSelectorBlacklist
--blacklist-class	config.manualClassBlacklist
--dependency	config.dependencyPaths
-v / --verbose	config.verbose
positional args	config.images (src=dst for in-place)
## Phase 9 — End-to-End Validation

A final integration test that runs the full CLI on a real binary, re-signs it with codesign, installs it on a simulator or device, and verifies it launches without crashing.
```
original binary
      │
      ▼
class_obfuscator -m caesar --erase-methtype binary -o binary.obf
      │
      ▼
codesign --force --sign - binary.obf
      │
      ▼
xcrun simctl install + launch → must not crash
```

# Full Picture

```
Phase 1   loadMachOImage()          disk → memory
Phase 2   objc_structs.h            struct definitions
Phase 3   ObjcExtractor             memory → ObjC metadata
Phase 4   SymbolsCollector          metadata → whitelist/blacklist
Phase 5   IMangler                  whitelist → ManglingMap
Phase 6   BinaryPatcher             ManglingMap → patched binary on disk
Phase 7   ObfuscatorPipeline        orchestrates 1–6
Phase 8   CLI (main.cpp)            argv → config → Phase 7    ← next
Phase 9   End-to-end validation     patched binary runs on device
```

