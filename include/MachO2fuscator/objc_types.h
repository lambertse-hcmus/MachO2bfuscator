#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

// ═══════════════════════════════════════════════════════════════
//  StringInData
//
//  Holds a string value AND remembers exactly where in the
//  binary file it lives (fileOffset + length).
//
//  This is the key design insight from the original project:
//  we don't just extract the string — we remember its location
//  so Phase 6 can patch it in-place without re-scanning.
// ═══════════════════════════════════════════════════════════════
struct StringInData {
  std::string value;    // the actual string content
  uint64_t fileOffset;  // byte offset from start of this slice's data
  uint64_t length;      // byte length (== value.size() for ASCII)

  // Convenience: end offset (exclusive)
  uint64_t end() const { return fileOffset + length; }

  // Swift class names exposed to ObjC always start with "_Tt"
  bool isSwiftName() const {
    return value.size() >= 3 && value[0] == '_' && value[1] == 'T' &&
           value[2] == 't';
  }
};

// ═══════════════════════════════════════════════════════════════
//  ObjcMethod
// ═══════════════════════════════════════════════════════════════
struct ObjcMethod {
  StringInData name;      // selector name, e.g. "viewDidLoad"
  StringInData methType;  // type encoding, e.g. "v16@0:8"
};

// ═══════════════════════════════════════════════════════════════
//  ObjcIvar
// ═══════════════════════════════════════════════════════════════
struct ObjcIvar {
  StringInData name;  // ivar name, e.g. "_title"
  StringInData type;  // type encoding, e.g. "@\"NSString\""
};

// ═══════════════════════════════════════════════════════════════
//  ObjcProperty
// ═══════════════════════════════════════════════════════════════
struct ObjcProperty {
  StringInData name;  // property name, e.g. "title"
  StringInData
      attributes;  // attribute string, e.g. "T@\"NSString\",C,N,V_title"

  // Splits the attribute string on ',' separators
  std::vector<std::string> attributeValues() const;

  // Returns the "T..." component (first attribute, starts with 'T')
  // e.g. for "T@\"NSString\",C,N" returns "T@\"NSString\""
  std::string typeAttribute() const;
};

// ═══════════════════════════════════════════════════════════════
//  ObjcClass
// ═══════════════════════════════════════════════════════════════
struct ObjcClass {
  StringInData name;  // class name, e.g. "MyViewController"
  std::optional<StringInData> ivarLayout;  // ivar layout bitmap (often null)
  std::vector<ObjcMethod> methods;
  std::vector<ObjcIvar> ivars;
  std::vector<ObjcProperty> properties;
};

// ═══════════════════════════════════════════════════════════════
//  ObjcCategory
// ═══════════════════════════════════════════════════════════════
struct ObjcCategory {
  StringInData name;  // category name (stored in __objc_classname if pure ObjC)
  std::optional<ObjcClass>
      cls;  // the class this category extends (may be null for external class)
  std::vector<ObjcMethod> methods;
  std::vector<ObjcProperty> properties;
};

// ═══════════════════════════════════════════════════════════════
//  ObjcProtocol
// ═══════════════════════════════════════════════════════════════
struct ObjcProtocol {
  StringInData name;
  std::vector<ObjcMethod> methods;
  std::vector<ObjcProperty> properties;
};

// ═══════════════════════════════════════════════════════════════
//  ObjcMetadata
//
//  The complete extracted ObjC metadata from one MachOSlice.
//  This is what Phase 3 produces and what Phase 4 consumes.
// ═══════════════════════════════════════════════════════════════
struct ObjcMetadata {
  std::vector<ObjcClass> classes;
  std::vector<ObjcCategory> categories;
  std::vector<ObjcProtocol> protocols;
};
