# 📋 C++ Clone Plan: ObjC Class Metadata Obfuscation

---

## Phase 0 — Project Setup & Architecture Decision

**Goal:** Establish the C++ project skeleton and decide on key design choices before writing any logic.

**Ideas & Decisions to make:**

- Choose build system: **CMake** is recommended (cross-platform, widely supported)
- Choose C++ standard: **C++17 minimum** (for `std::string_view`, `std::optional`, structured bindings, `std::filesystem`)
- Decide on the two-layer architecture mirroring the original:
  - A **MachReader** layer (reads & parses binary)
  - An **Obfuscator** layer (orchestrates the pipeline)
- Handle 32-bit vs 64-bit: use C++ templates parameterized on `uint32_t` / `uint64_t` as the pointer type, mirroring Swift's `ArchitectureDependent` protocol
- Memory strategy: **mmap** the file (edit in place, write back)
- Third-party dependencies: **none required** for the core feature (all you need is in system headers `<mach-o/loader.h>`, `<mach-o/fat.h>`, `<mach-o/nlist.h>`)

**Deliverables:**

- `CMakeLists.txt`
- Directory structure mirroring the modules (e.g., `src/mach/`, `src/obfuscator/`, `src/mangler/`)
- A header for common types (`types.h`: typedefs, forward declarations)

---

## Phase 1 — Mach-O Binary Loading & Section Lookup

**Goal:** Load a Mach-O binary into memory and be able to find any section by name.

**Ideas:**

- Use `mmap` to map the file into memory — gives you a `uint8_t*` base pointer
- Handle Fat binaries first: check the magic number (`FAT_MAGIC` / `FAT_CIGAM`) and if so, iterate `fat_arch` entries to find the slices you care about
  - Each slice is itself a regular Mach-O starting at a given offset within the file
- For each Mach-O slice:
  - Read `mach_header` or `mach_header_64` (check magic: `MH_MAGIC` vs `MH_MAGIC_64`)
  - Walk the load commands linearly (each `load_command` has a `cmdsize` to advance by)
  - When you hit `LC_SEGMENT` or `LC_SEGMENT_64`, scan its sections
  - Build an index: `map<string, SectionInfo>` where `SectionInfo` holds the file offset, VM address, and size of each section

**Key sections to locate and store:**

- `__TEXT,__objc_classname`
- `__TEXT,__objc_methname`
- `__TEXT,__objc_methtype`
- `__TEXT,__objc_classlist`
- `__TEXT,__objc_catlist`
- `__TEXT,__objc_protolist`
- `__TEXT,__cstring`
- `LC_DYLD_INFO_ONLY` (for export trie / binding info)
- `LC_SYMTAB` (for symbol table)

Also record the **VM slide** (difference between VM address and file offset for each segment) — needed for resolving VM pointers to file offsets.

**Key concept:** VM address → file offset translation. Every pointer stored inside ObjC metadata is a VM address. To actually read the pointed-to data, you must convert it to a file offset using the segment's `vmaddr` and `fileoff`.

---

## Phase 2 — ObjC Metadata Struct Definitions

**Goal:** Define the C++ structs that mirror the ObjC runtime layout in the binary.

**Ideas:**

- Do **not** use the runtime headers (`<objc/runtime.h>`) — those reflect the in-memory layout which can differ from the on-disk binary layout
- Instead define your own `#pragma pack(1)` structs (or verify alignment is correct) for the on-disk format, same as the Swift code did by redeclaring them:
  - `class_t<PointerType>` (the class_64 / class_32 in the original)
  - `class_ro_t<PointerType>` — important: the 64-bit version has an extra reserved field after `instanceSize`. Handle this via template specialization for `uint64_t`
  - `method_t<PointerType>`
  - `ivar_t<PointerType>`
  - `property_t<PointerType>`
  - `entsize_list_tt` (architecture-independent header for all ObjC lists)
  - `category_t<PointerType>`
  - `protocol_t<PointerType>`
- Add a helper template: `class_t<P>::data_ptr()` that applies the bitmask to bits to get the pointer to `class_ro_t` (same as `bits & 0x0000_7FFF_FFFF_FFF8` for 64-bit, `bits & 0xFFFF_FFFC` for 32-bit) — handle via specialization or a `constexpr` helper
- Add a `getStruct<T>(uint8_t* base, size_t fileOffset) -> T*` utility function that just pointer-casts the base buffer — equivalent of Swift's `data.getStruct(atOffset:)`

---

## Phase 3 — ObjC Symbol Extraction

**Goal:** Walk the ObjC metadata and extract all class names, selector names, and category names from the binary.

**Ideas:**

- Write a templated `ObjcMetadataReader<PointerType>` class (equivalent to Swift's `MachArchitecture<PointerType>`)
- For classes (`__objc_classlist`):
  - The section is a list of `PointerType` values, each a VM pointer to a `class_t`
  - For each pointer: resolve to file offset → cast to `class_t<P>*` → extract data field → resolve to file offset → cast to `class_ro_t<P>*` → extract name field (another VM pointer) → resolve to file offset → read as null-terminated C-string
  - Store: `{ className, fileOffsetOfNameString }` — needed for replacement
- For categories (`__objc_catlist`): same pattern
- For protocols (`__objc_protolist`): same pattern
- For methods (inside each class / category):
  - Walk `baseMethodList`: `class_ro_t.baseMethodList` → resolve → `entsize_list_tt*` → iterate count elements of size `entsizeAndFlags & ~0x3` → each is `method_t<P>*` → read name (VM pointer to selector string) and types (VM pointer to methtype string)
- For properties (inside each class): walk `baseProperties` similarly
- Dispatch between 32 and 64-bit: read magic from header, instantiate `ObjcMetadataReader<uint32_t>` or `ObjcMetadataReader<uint64_t>` accordingly
- **Result:** a `SymbolSet` containing `std::unordered_set<std::string>` for classes and selectors

---

## Phase 4 — Symbol Whitelist & Blacklist

**Goal:** Decide which class names are safe to obfuscate (whitelist) and which must be left alone (blacklist).

**Ideas:**

- This directly mirrors `ObfuscationSymbols+Building.swift`
- **Whitelist:** all class names found in the obfuscable binary (the target app or framework you own)
- **Blacklist:** all class names found in any non-obfuscable dependencies (system frameworks, external dylibs outside your app bundle) — you must never rename these because you don't own those files
- How to build the blacklist:
  - Scan `LC_LOAD_DYLIB` entries in the Mach-O to find all dependent libraries
  - Determine which ones are inside the app bundle vs outside (system path check)
  - Run Phase 3 on the unobfuscable ones and add all their symbols to the blacklist
- Final whitelist = (all symbols from obfuscable files) - (blacklist)
- Optionally: support a manual blacklist from a text file (same as `--skip-symbols-from-list` option in the original)
- Data structure: two `struct ObjCSymbolSets { unordered_set<string> classes; unordered_set<string> selectors; }` — one for whitelist, one for blacklist

---

## Phase 5 — Symbol Mangler

**Goal:** Produce a `[oldName → newName]` mapping for all whitelisted class names and selectors.

**Ideas:**

- Define an abstract interface (pure virtual class or `std::function`-based):

```cpp
class SymbolMangler {
    virtual ManglingMap mangle(const SymbolSets& symbols) = 0;
};
struct ManglingMap {
    unordered_map<string, string> classNames;
    unordered_map<string, string> selectors;
};
```

- Implement at minimum one mangler — recommended: a random alphanumeric mangler (same spirit as `realWords` in the original):
  - For each class name in the whitelist, generate a random replacement string of the same byte length (**critical constraint** — you cannot change string length because you are editing in-place in the binary)
  - Ensure no two original names map to the same new name (collision check)
  - Ensure no new name collides with any blacklisted name
- The same-length constraint is the most important rule — it comes from `Data+Mapping.swift`'s `precondition(originalString.utf8.count == mappedString.utf8.count)`
- Optionally implement a simpler ROT13/Caesar mangler first as a sanity-check tool
- The mangler must also guarantee that generated names do not contain null bytes (they are C-strings)

---

## Phase 6 — In-Place Binary Patching

**Goal:** Apply the `ManglingMap` to the loaded binary bytes, replacing all occurrences of old names with new names in the correct sections.

**Ideas:**

- This mirrors `Mach+Replacing.swift` — the most important file in the original
- Patch `__TEXT,__objc_classname` section:
  - Scan the section byte-range for null-terminated strings
  - For each string found: look it up in `classNames` map → if found, `memcpy` the new name over it (same length guaranteed, null terminator stays at the same position)
- Patch `__TEXT,__objc_methname` section: same scan-and-replace approach, using the selectors map
- Patch `__TEXT,__objc_methtype` section:
  - This is trickier — methtype strings embed class names inside type encodings like `@"MyClassName"` or `@"<MyProtocol>"`
  - Implement a `patchMethType(char* methtype, const ManglingMap& map)` function that does substring search-and-replace of class names within the methtype string
  - Important: this must still preserve the overall string's byte length — only works if old and new class names have the same length
  - This mirrors the `MethTypeObfuscator` struct in the original
- Patch property attribute strings:
  - Walk `class_ro_t.baseProperties` for each class → for each `property_t` → resolve attributes VM pointer → apply methtype-style patching to the attribute string in-place
- Patch `LC_DYLD_INFO_ONLY` export trie:
  - Walk the trie data structure (it's a compressed prefix trie of exported symbol names)
  - Replace class name prefixes (ObjC exports are named `_OBJC_CLASS_$_ClassName`)
  - This is the hardest part — requires LEB128 reading/writing and trie traversal (see Phase 7)
- Erase `LC_SYMTAB`:
  - Just `memset` the string table referenced by `LC_SYMTAB` to zeros — same as `eraseSymtab()` in the original

---

## Phase 7 — Export Trie & Binding Info Patching

**Goal:** Patch the `LC_DYLD_INFO_ONLY` section so that the dynamic linker binding and export symbols also reflect the new class names.

**Ideas:**

- This is the most algorithmically complex part of the whole project
- **Export Trie structure:** it's a byte-encoded prefix trie where edges are labeled with string fragments. ObjC class exports appear as `_OBJC_CLASS_$_OriginalName` and `_OBJC_METACLASS_$_OriginalName`
- To patch it:
  - Walk the trie recursively, accumulating the current symbol name prefix at each node
  - When you reach a terminal node whose accumulated symbol contains a class name from the `classNames` map, replace that fragment with the new name
  - Challenge: the trie uses LEB128-encoded offsets and the trie is compactly packed — you may need to rebuild the trie from scratch rather than edit in-place, because replacing a longer name with a shorter one (or vice versa — wait, same length!) changes node offsets. Since names are same-length, this is actually manageable
- **Binding Info:** `LC_DYLD_INFO_ONLY` also contains bind opcodes that reference external symbols. Walk the opcode stream, decode LEB128 string references, and patch class names that appear there
- **Recommend:** implement LEB128 read/write utilities first as standalone functions, well-tested

---

## Phase 8 — File Saving

**Goal:** Write the patched binary back to disk safely.

**Ideas:**

- Since you used `mmap` with `PROT_READ | PROT_WRITE` and `MAP_SHARED`, all your in-place edits are already reflected in the mapped memory — calling `msync()` then `munmap()` is sufficient to flush changes to disk
- Alternatively (safer for debugging): write to a new output file first, validate it, then optionally overwrite the original — same as the `--dry-run` option in the original
- For Fat binaries: you only modified specific slices within the fat binary's byte range — the fat header and other slices remain untouched since all edits were confined to the correct slice's offset range

---

## Phase 9 — CLI Interface & Integration

**Goal:** Wire everything together into a usable command-line tool.

**Ideas:**

- Parse command-line arguments manually or use a lightweight library (`getopt`, or header-only CLI11, or argparse)
- Minimum required arguments:
  - Input binary path
  - Output path (or in-place flag)
  - Mangler selection (e.g., `--mangler random`)
- Optional:
  - `--blacklist-class NAME` for manual blacklisting
  - `--dry-run` (parse and show what would be renamed, don't write)
  - `--dump-metadata` (print all found ObjC symbols, mirrors `--xx-dump-metadata`)
- Wire the pipeline: Load → Extract Symbols → Build Whitelist/Blacklist → Mangle → Patch → Save
- Add logging at each stage (simple `fprintf(stderr, ...)` with verbosity levels is fine)

---

## Phase 10 — Testing & Validation

**Goal:** Verify correctness of the obfuscated binary.

**Ideas:**

- Unit tests for each phase independently:
  - Phase 1: given a known binary, can you find `__objc_classname` at the right offset?
  - Phase 3: given a test binary with known class names, does extraction return the correct set?
  - Phase 5: does the mangler always produce same-length strings? No collisions?
  - Phase 6: after patching `__objc_classname`, do the old names no longer appear?
  - Phase 7: after patching the export trie, does `nm` or `otool -l` on the output show the new names?
- Integration test:
  - Obfuscate a real test iOS app binary
  - Run `strings` on the output — old class names should not appear
  - Run `otool -oV` on the output — verify ObjC metadata still parses correctly with new names
  - Sign and run the app — it should still launch and function correctly
- Regression safety: always keep the original file backed up; diff the obfuscated binary's structure (not content) with the original to ensure no structural corruption

