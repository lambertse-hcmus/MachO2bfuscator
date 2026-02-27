#include <cassert>
#include <iostream>
#include <stdexcept>

#include "MachO2fuscator/mach_reader.h"
#include "MachO2fuscator/objc_extractor.h"

static int g_passed = 0, g_failed = 0;
#define RUN(name)                                \
  do {                                           \
    std::cout << "  running " #name " ... ";     \
    try {                                        \
      name();                                    \
      std::cout << "PASS\n";                     \
      ++g_passed;                                \
    } catch (const std::exception& e) {          \
      std::cout << "FAIL: " << e.what() << "\n"; \
      ++g_failed;                                \
    }                                            \
  } while (0)
#define ASSERT(c)                                                \
  do {                                                           \
    if (!(c)) throw std::runtime_error("Assertion failed: " #c); \
  } while (0)
#define ASSERT_EQ(a, b)                                          \
  do {                                                           \
    if ((a) != (b))                                              \
      throw std::runtime_error(std::string("Expected equal: ") + \
                               std::to_string(a) + " vs " +      \
                               std::to_string(b));               \
  } while (0)

// ── Test: StringInData helpers ────────────────────────────────────
void test_string_in_data() {
  StringInData s;
  s.value = "MyViewController";
  s.fileOffset = 0x100;
  s.length = 16;
  ASSERT(!s.isSwiftName());
  ASSERT_EQ(s.end(), 0x110u);

  StringInData swift;
  swift.value = "_TtC7MyApp16MyViewController";
  ASSERT(swift.isSwiftName());
}

// ── Test: ObjcProperty helpers ────────────────────────────────────
void test_property_attribute_values() {
  ObjcProperty p;
  p.attributes.value = "T@\"NSString\",C,N,V_title";

  auto vals = p.attributeValues();
  ASSERT_EQ(vals.size(), 4u);
  ASSERT(vals[0] == "T@\"NSString\"");
  ASSERT(vals[1] == "C");
  ASSERT(vals[2] == "N");
  ASSERT(vals[3] == "V_title");

  ASSERT(p.typeAttribute() == "T@\"NSString\"");
}

// ── Test: libobjcSelectors contains expected entries ──────────────
void test_libobjc_selectors() {
  const auto& sels = ObjcExtractor::libobjcSelectors();
  ASSERT(sels.count("retain") == 1);
  ASSERT(sels.count("release") == 1);
  ASSERT(sels.count("dealloc") == 1);
  ASSERT(sels.count("alloc") == 1);
  ASSERT(sels.count("viewDidLoad") == 0);  // not a libobjc selector
}
std::string objCPath =
    "/Users/tri.le/src/opensource/lambertse/MachO2fuscation/assets/"
    "testckey_objc";
// ── Integration test: extract from real binary ────────────────────
void test_extract_real_binary() {
  // /usr/lib/libobjc.A.dylib always has ObjC metadata
  MachOImage image = loadMachOImage(objCPath);
  ASSERT(!image.slices.empty());

  const MachOSlice& slice = image.slices[0];
  ObjcMetadata meta = ObjcExtractor::extractMetadata(slice);

  // libobjc always has classes and protocols
  ASSERT(!meta.classes.empty());

  // All extracted class names must be non-empty
  for (const auto& cls : meta.classes) {
    ASSERT(!cls.name.value.empty());
    // fileOffset must be within slice bounds
    ASSERT(cls.name.fileOffset < slice.dataSize);
  }
}

// ── Integration test: selector extraction ────────────────────────
void test_extract_selectors_real_binary() {
  MachOImage image = loadMachOImage(objCPath);
  const MachOSlice& slice = image.slices[0];

  auto sels = ObjcExtractor::extractSelectors(slice);
  ASSERT(!sels.empty());

  // Every selector must be non-empty and within bounds
  for (const auto& s : sels) {
    // std::cout << s.value << "\n";
    ASSERT(!s.value.empty());
    ASSERT(s.fileOffset < slice.dataSize);
    ASSERT(s.length == s.value.size());
  }
}

void test_debug_classlist_raw() {
  const std::string path = objCPath;
  MachOImage image = loadMachOImage(path);
  const MachOSlice& slice = image.slices[0];

  const MachSection* classlistSec = slice.objcClasslistSection();
  if (!classlistSec) {
    throw std::runtime_error("No __objc_classlist section found");
  }

  std::cout << "\n  __objc_classlist: fileOff=0x" << std::hex
            << classlistSec->fileOffset << " size=0x" << classlistSec->size
            << "\n";

  // Print raw pointer values stored in classlist
  uint64_t cursor = classlistSec->fileOffset;
  uint64_t end = classlistSec->fileOffset + classlistSec->size;
  int idx = 0;
  while (cursor + 8 <= end) {
    const uint64_t* rawPtr =
        reinterpret_cast<const uint64_t*>(slice.data + cursor);
    std::cout << "  entry[" << std::dec << idx << "] raw=0x" << std::hex
              << *rawPtr << "\n";

    // What does decodePointer produce for this value?
    uint16_t fmt = slice.dyldInfo ? slice.dyldInfo->pointerFormat : 0;
    std::cout << "    pointerFormat=" << std::dec << fmt << "\n";
    std::cout << "    preferredLoadAddr=0x" << std::hex;

    uint64_t pla = 0;
    for (const auto& seg : slice.segments) {
      if (seg.name != "__PAGEZERO" && seg.vmAddr != 0) {
        pla = seg.vmAddr;
        break;
      }
    }
    std::cout << pla << "\n";

    cursor += 8;
    idx++;
  }
  std::cout << std::dec;
}

void test_debug_pointer_format() {
  MachOImage image = loadMachOImage(objCPath);
  const MachOSlice& slice = image.slices[0];

  std::cout << "\n  hasChainedFixups: "
            << (slice.dyldInfo && slice.dyldInfo->hasChainedFixups ? "YES"
                                                                   : "NO")
            << "\n";
  std::cout << "  pointerFormat: "
            << (slice.dyldInfo ? slice.dyldInfo->pointerFormat : 0) << "\n";

  // Expected: pointerFormat=2 (DYLD_CHAINED_PTR_64)
  // for a standard arm64 non-PAC binary
  ASSERT(slice.dyldInfo.has_value());
  ASSERT(slice.dyldInfo->hasChainedFixups);
  ASSERT(slice.dyldInfo->pointerFormat != 0);
}

int main() {
  std::cout << "=== Phase 3 Tests: ObjC Symbol Extraction ===\n\n";
  RUN(test_string_in_data);
  RUN(test_property_attribute_values);
  RUN(test_libobjc_selectors);
  RUN(test_extract_real_binary);
  RUN(test_extract_selectors_real_binary);
  RUN(test_debug_classlist_raw);
  RUN(test_debug_pointer_format);

  std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed
            << " failed ===\n";
  return g_failed > 0 ? 1 : 0;
}
