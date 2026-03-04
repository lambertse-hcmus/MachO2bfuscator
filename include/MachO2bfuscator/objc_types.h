#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

struct StringInData {
  std::string value;
  uint64_t fileOffset;
  uint64_t length;

  uint64_t end() const { return fileOffset + length; }

  // Swift class names exposed to ObjC always start with "_Tt"
  bool isSwiftName() const {
    return value.size() >= 3 && value.find("_Tt") == 0;
  }
};

// ═══════════════════════════════════════════════════════════════
//  ObjcMethod
// ═══════════════════════════════════════════════════════════════
struct ObjcMethod {
  StringInData name;
  StringInData methType;
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
  StringInData name;  
  std::optional<StringInData> ivarLayout;  // ivar layout bitmap (often null)
  std::vector<ObjcMethod> methods;
  std::vector<ObjcIvar> ivars;
  std::vector<ObjcProperty> properties;
};

// ═══════════════════════════════════════════════════════════════
//  ObjcCategory
// ═══════════════════════════════════════════════════════════════
struct ObjcCategory {
  StringInData name;
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
// ═══════════════════════════════════════════════════════════════
struct ObjcMetadata {
  std::vector<ObjcClass> classes;
  std::vector<ObjcCategory> categories;
  std::vector<ObjcProtocol> protocols;
};
