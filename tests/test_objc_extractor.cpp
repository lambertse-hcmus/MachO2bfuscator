#include <gtest/gtest.h>

#include "MachO2bfuscator/mach_reader.h"
#include "MachO2bfuscator/objc_extractor.h"

#ifdef TEST_OBJC_ABSOLUTE_PATH
static const std::string kBinaryPath = "assets/testckey_objc";
#else
#warning \
    "TEST_OBJC_ABSOLUTE_PATH is not defined. Some tests will be skipped that require a real binary."
static const std::string kBinaryPath = "";
#endif

// ════════════════════════════════════════════════════════════════════
//  Unit tests
// ══════════════���═════════════════════════════���═══════════════════════

TEST(ObjcExtractor, StringInDataHelpers) {
  StringInData s;
  s.value = "MyViewController";
  s.fileOffset = 0x100;
  s.length = 16;
  EXPECT_FALSE(s.isSwiftName());
  EXPECT_EQ(s.end(), 0x110u);

  StringInData swift;
  swift.value = "_TtC7MyApp16MyViewController";
  EXPECT_TRUE(swift.isSwiftName());
}

TEST(ObjcExtractor, PropertyAttributeValues) {
  ObjcProperty p;
  p.attributes.value = "T@\"NSString\",C,N,V_title";

  auto vals = p.attributeValues();
  ASSERT_EQ(vals.size(), 4u);
  EXPECT_EQ(vals[0], "T@\"NSString\"");
  EXPECT_EQ(vals[1], "C");
  EXPECT_EQ(vals[2], "N");
  EXPECT_EQ(vals[3], "V_title");
  EXPECT_EQ(p.typeAttribute(), "T@\"NSString\"");
}

TEST(ObjcExtractor, LibobjcSelectorsContainsExpectedEntries) {
  const auto& sels = ObjcExtractor::libobjcSelectors();
  EXPECT_EQ(sels.count("retain"), 1u);
  EXPECT_EQ(sels.count("release"), 1u);
  EXPECT_EQ(sels.count("dealloc"), 1u);
  EXPECT_EQ(sels.count("alloc"), 1u);
  EXPECT_EQ(sels.count("viewDidLoad"), 0u);  // not a libobjc selector
}

// ════════════════════════════════════════════════════════════════════
//  Integration tests
// ════════════════════════════════════════════════════════════════════

TEST(Integration, ObjcExtractorExtractsClasses) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  MachOImage image = loadMachOImage(kBinaryPath);
  ASSERT_FALSE(image.slices.empty());

  const MachOSlice& slice = image.slices[0];
  ObjcMetadata meta = ObjcExtractor::extractMetadata(slice);

  ASSERT_FALSE(meta.classes.empty()) << "Expected at least one ObjC class";

  for (const auto& cls : meta.classes) {
    EXPECT_FALSE(cls.name.value.empty()) << "Class name must not be empty";
    EXPECT_LT(cls.name.fileOffset, slice.dataSize)
        << "Class name fileOffset out of slice bounds";
  }
}

TEST(Integration, ObjcExtractorExtractsSelectors) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  MachOImage image = loadMachOImage(kBinaryPath);
  const MachOSlice& slice = image.slices[0];

  auto sels = ObjcExtractor::extractSelectors(slice);
  ASSERT_FALSE(sels.empty());

  for (const auto& s : sels) {
    EXPECT_FALSE(s.value.empty()) << "Selector value must not be empty";
    EXPECT_LT(s.fileOffset, slice.dataSize)
        << "Selector fileOffset out of bounds";
    EXPECT_EQ(s.length, s.value.size()) << "length must match value.size()";
  }
}

TEST(Integration, ObjcExtractorPointerFormat) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  MachOImage image = loadMachOImage(kBinaryPath);
  const MachOSlice& slice = image.slices[0];

  ASSERT_TRUE(slice.dyldInfo.has_value()) << "Expected dyldInfo to be present";
  EXPECT_TRUE(slice.dyldInfo->hasChainedFixups) << "Expected chained fixups";
  EXPECT_NE(slice.dyldInfo->pointerFormat, 0)
      << "pointerFormat must not be zero";

  RecordProperty("hasChainedFixups",
                 slice.dyldInfo->hasChainedFixups ? "YES" : "NO");
  RecordProperty("pointerFormat",
                 std::to_string(slice.dyldInfo->pointerFormat));
}

TEST(Integration, ObjcExtractorClasslistRaw) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  MachOImage image = loadMachOImage(kBinaryPath);
  const MachOSlice& slice = image.slices[0];

  const MachSection* classlistSec = slice.objcClasslistSection();
  ASSERT_NE(classlistSec, nullptr) << "No __objc_classlist section found";
  EXPECT_GT(classlistSec->size, 0u);

  // Walk and count entries — just validate the section is parseable
  uint64_t cursor = classlistSec->fileOffset;
  uint64_t end = classlistSec->fileOffset + classlistSec->size;
  int count = 0;
  while (cursor + 8 <= end) {
    cursor += 8;
    ++count;
  }
  EXPECT_GT(count, 0) << "Expected at least one classlist entry";
  RecordProperty("classlist_entries", std::to_string(count));
}
