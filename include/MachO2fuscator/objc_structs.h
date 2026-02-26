#pragma once

#include <cstdint>

// ═══════════════════════════════════════════════════════════════
//  Raw on-disk ObjC runtime structs
//
//  These mirror ObjcStructs.swift exactly.
//  Reference:
//  https://opensource.apple.com/source/objc4/objc4-750.1/runtime/objc-runtime-new.h
//
//  ALL fields are stored in NATIVE ENDIAN (little-endian on arm64/x86_64).
//  Pointer fields are VM addresses — use fileOffsetFromVmOffset() to resolve.
//
//  IMPORTANT: do NOT use <objc/runtime.h> structs here.
//  Those are in-memory layouts and differ from the on-disk format
//  (e.g. relative method lists, pointer authentication, etc.)
// ═══════════════════════════════════════════════════════════════

// Ensure no compiler padding is inserted between fields.
// These structs must match the binary layout byte-for-byte.
#pragma pack(push, 1)

namespace ObjC {

// ── entsize_list_tt ──────────────────────────────────────────────
// Architecture-independent header for ALL ObjC list types:
// method lists, ivar lists, property lists.
//
// Layout:
//   [entsizeAndFlags: uint32]  lower bits = element size, upper bits = flags
//   [count: uint32]            number of elements that follow immediately
//
// Elements follow immediately after this header in memory.
struct entsize_list_tt {
  uint32_t entsizeAndFlags;
  uint32_t count;
  // Elements start at: (uint8_t*)this + sizeof(entsize_list_tt)
};

// ── method_t ────────────────────────────────────────────────────
// One entry in a method list.
//
// CLASSIC (absolute) format — used in older binaries and 32-bit:
//   name:  VM pointer to a selector string  (the selector name, e.g.
//   "viewDidLoad") types: VM pointer to a methtype string  (e.g. "v16@0:8")
//   imp:   VM pointer to the implementation function
//
// NOTE: Xcode 14+ introduced RELATIVE method lists (stored in __objc_methlist).
// Those use int32_t relative offsets instead of pointer-sized VM addresses.
// We handle the classic format here; relative method lists are handled
// separately in Phase 3.
template <typename PointerType>
struct method_t {
  PointerType name;   // SEL — VM pointer to selector C-string
  PointerType types;  // VM pointer to methtype encoding C-string
  PointerType
      imp;  // VM pointer to implementation (we never need to patch this)
};

// ── ivar_t ──────────────────────────────────────────────────────
// One entry in an ivar (instance variable) list.
template <typename PointerType>
struct ivar_t {
  // offset is always 32-bit even in 64-bit binaries
  // (some platforms over-allocate to 64-bit, but we only read 32 bits)
  uint32_t offset;
  PointerType name;  // VM pointer to ivar name C-string
  PointerType type;  // VM pointer to type encoding C-string
  uint32_t alignment_raw;
  uint32_t size;
};

// ── property_t ──────────────────────────────────────────────────
// One entry in a property list.
template <typename PointerType>
struct property_t {
  PointerType name;        // VM pointer to property name C-string
  PointerType attributes;  // VM pointer to property attribute string
                           // e.g. "T@\"NSString\",C,N,V_title"
};

// ── class_ro_t — 64-bit ─────────────────────────────────────────
// The read-only class data. Contains the class name pointer and
// pointers to its method/ivar/property lists.
//
// CRITICAL: the 64-bit version has an extra 'reserved' uint32 field
// after instanceSize that is NOT present in the 32-bit version.
// This is why we need two separate structs (not a template).
struct class_ro_64 {
  uint32_t flags;
  uint32_t instanceStart;
  uint32_t instanceSize;
  uint32_t reserved;    // ← 64-bit ONLY padding field
  uint64_t ivarLayout;  // VM pointer to ivar layout bitmap
  uint64_t name;  // VM pointer to class name C-string  ← WE CARE ABOUT THIS
  uint64_t baseMethodList;  // VM pointer to method_list_t
  uint64_t baseProtocols;   // VM pointer to protocol_list_t
  uint64_t ivars;           // VM pointer to ivar_list_t
  uint64_t weakIvarLayout;  // VM pointer to weak ivar layout bitmap
  uint64_t baseProperties;  // VM pointer to property_list_t
};

// ── class_ro_t — 32-bit ─────────────────────────────────────────
// Same as class_ro_64 but WITHOUT the reserved field,
// and all pointers are 32-bit.
struct class_ro_32 {
  uint32_t flags;
  uint32_t instanceStart;
  uint32_t instanceSize;
  // NO reserved field here — this is the key difference
  uint32_t ivarLayout;
  uint32_t name;  // VM pointer to class name C-string
  uint32_t baseMethodList;
  uint32_t baseProtocols;
  uint32_t ivars;
  uint32_t weakIvarLayout;
  uint32_t baseProperties;
};

// ── class_t — 64-bit ────────────────────────────────────────────
// The top-level class object stored in __objc_classlist.
//
// We navigate: class_t → class_ro_t → name
//
// The 'bits' field encodes the pointer to class_ro_t with some
// flag bits mixed in. We must mask them off to get the real pointer:
//   class_ro_ptr = bits & 0x0000_7FFF_FFFF_FFF8   (64-bit)
struct class_t_64 {
  uint64_t isa;
  uint64_t superclass;
  uint64_t cacheBuckets;
  uint32_t cacheMask;
  uint32_t cacheOccupied;
  uint64_t bits;  // data pointer with flag bits

  // Extract the clean VM pointer to class_ro_64
  uint64_t dataPtr() const { return bits & 0x0000'7FFF'FFFF'FFF8ULL; }
};

// ── class_t — 32-bit ────────────────────────────────────────────
//   class_ro_ptr = bits & 0xFFFF_FFFC   (32-bit)
struct class_t_32 {
  uint32_t isa;
  uint32_t superclass;
  uint32_t cacheBuckets;
  uint16_t cacheMask;
  uint16_t cacheOccupied;
  uint32_t bits;

  uint32_t dataPtr() const { return bits & 0xFFFF'FFFCu; }
};

// ── category_t ──────────────────────────────────────────────────
// Stored in __objc_catlist.
template <typename PointerType>
struct category_t {
  PointerType name;                // VM pointer to category name C-string
  PointerType cls;                 // VM pointer to the class_t this extends
  PointerType instanceMethods;     // VM pointer to method_list_t
  PointerType classMethods;        // VM pointer to method_list_t
  PointerType protocols;           // VM pointer to protocol_list_t
  PointerType instanceProperties;  // VM pointer to property_list_t
  PointerType
      _classProperties;  // VM pointer (may not always be present on disk)
};

// ── protocol_t ──────────────────────────────────────────────────
// Stored in __objc_protolist.
template <typename PointerType>
struct protocol_t {
  PointerType isa;
  PointerType name;       // VM pointer to protocol name C-string
  PointerType protocols;  // VM pointer to protocol_list_t
  PointerType instanceMethods;
  PointerType classMethods;
  PointerType optionalInstanceMethods;
  PointerType optionalClassMethods;
  PointerType instanceProperties;
};

// ── Relative method list entry (Xcode 14+) ──────────────────────
// When the method list flags have bit 31 set (0x80000000),
// the list uses RELATIVE offsets (int32_t) instead of VM pointers.
// This is the format used in __objc_methlist (which appeared in your binary).
// Each field is a relative offset from ITS OWN ADDRESS to the target.
//
// but we need it for your test binary which uses Xcode 14+ toolchain.
struct relative_method_t {
  int32_t nameOffset;   // relative offset to a pointer to the selector string
  int32_t typesOffset;  // relative offset to the methtype C-string
  int32_t impOffset;    // relative offset to the implementation
};

// Flag that indicates a method list uses relative offsets
static constexpr uint32_t METHOD_LIST_IS_UNIQUED_STUBS = 0x80000000u;
static constexpr uint32_t METHOD_LIST_FLAG_MASK = 0xFFFF0003u;
static constexpr uint32_t METHOD_LIST_ENTSIZE_MASK = ~METHOD_LIST_FLAG_MASK;

}  // namespace ObjC

#pragma pack(pop)
