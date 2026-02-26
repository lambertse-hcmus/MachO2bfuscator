#include "objc_types.h"

#include <sstream>
#include <stdexcept>

// ── ObjcProperty::attributeValues ───────────────────────────────
//     return attributes.value.split(separator: ",").map { String($0) }
// }
//
// Example: "T@\"NSString\",C,N,V_title"
// Result:  ["T@\"NSString\"", "C", "N", "V_title"]
std::vector<std::string> ObjcProperty::attributeValues() const {
  std::vector<std::string> result;
  std::stringstream ss(attributes.value);
  std::string token;
  while (std::getline(ss, token, ',')) {
    if (!token.empty()) {
      result.push_back(token);
    }
  }
  return result;
}

// ── ObjcProperty::typeAttribute ─────────────────────────────────
//     guard let typeattr = attributeValues.first, typeattr.starts(with: "T")
//     else {
//         fatalError(...)
//     }
//     return typeattr
// }
//
// The type attribute is always the first component and starts with 'T'.
// e.g. "T@\"NSString\""  means: type = pointer to NSString
//      "Ti"              means: type = int
//      "T@\"<UITableViewDelegate>\""  means: type = pointer to protocol
std::string ObjcProperty::typeAttribute() const {
  auto values = attributeValues();
  if (values.empty() || values[0].empty() || values[0][0] != 'T') {
    throw std::runtime_error(
        "Type attribute missing or in unexpected format for property: " +
        name.value);
  }
  return values[0];
}
