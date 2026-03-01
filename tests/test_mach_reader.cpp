#include <gtest/gtest.h>

#include "MachO2bfuscator/mach_reader.h"

#ifdef TEST_OBJC_ABSOLUTE_PATH
static const std::string kBinaryPath = "assets/testckey_objc";
#else
#warning \
    "TEST_OBJC_ABSOLUTE_PATH is not defined. Some tests will be skipped that require a real binary."
static const std::string kBinaryPath = "";
#endif

// ════════════════════════════════════════════════════════════════════
//  Unit tests — no binary required
// ════════════════════════════════════════════════════════════════════

TEST(MachReader, LoadNonExistentThrows) {
  EXPECT_THROW(loadMachOImage("/nonexistent/path/to/binary"), MachLoadError);
}

TEST(MachReader, FileRangeHelper) {
  FileRange r{100, 50};
  EXPECT_EQ(r.offset, 100u);
  EXPECT_EQ(r.size, 50u);
  EXPECT_EQ(r.end(), 150u);
  EXPECT_FALSE(r.empty());

  FileRange empty{0, 0};
  EXPECT_TRUE(empty.empty());
}

TEST(MachReader, VmToFileOffsetBasic) {
  MachOSlice slice;
  MachSegment seg;
  seg.name = "__TEXT";
  seg.vmAddr = 0x100000000ULL;
  seg.vmSize = 0x1000;
  seg.fileOff = 0x0;
  seg.fileSize = 0x1000;
  slice.segments.push_back(seg);

  uint64_t off = slice.fileOffsetFromVmOffset(0x100000500ULL);
  EXPECT_EQ(off, 0x500u);
}

TEST(MachReader, VmToFileOffsetOutOfBoundsThrows) {
  MachOSlice slice;
  MachSegment seg;
  seg.vmAddr = 0x100000000ULL;
  seg.vmSize = 0x1000;
  seg.fileOff = 0x0;
  seg.fileSize = 0x1000;
  slice.segments.push_back(seg);

  EXPECT_THROW(slice.fileOffsetFromVmOffset(0x200000000ULL), MachLoadError);
}

TEST(MachReader, FindSection) {
  MachOSlice slice;
  MachSection sec;
  sec.segmentName = "__TEXT";
  sec.sectionName = "__objc_classname";
  sec.fileOffset = 0x1000;
  sec.size = 0x200;

  MachSegment seg;
  seg.name = "__TEXT";
  seg.sections.push_back(sec);
  slice.segments.push_back(seg);

  const MachSection* found = slice.findSection("__TEXT", "__objc_classname");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->fileOffset, 0x1000u);
  EXPECT_EQ(found->size, 0x200u);

  EXPECT_EQ(slice.findSection("__TEXT", "__objc_methname"), nullptr);
  EXPECT_EQ(slice.findSection("__DATA", "__objc_classname"), nullptr);
}

TEST(MachReader, NamedSectionAccessors) {
  MachOSlice slice;

  MachSegment textSeg;
  textSeg.name = "__TEXT";
  {
    MachSection s;
    s.segmentName = "__TEXT";
    s.sectionName = "__objc_classname";
    s.fileOffset = 0x100;
    s.size = 0x50;
    textSeg.sections.push_back(s);
  }
  {
    MachSection s;
    s.segmentName = "__TEXT";
    s.sectionName = "__objc_methname";
    s.fileOffset = 0x200;
    s.size = 0x80;
    textSeg.sections.push_back(s);
  }
  slice.segments.push_back(textSeg);

  MachSegment dataSeg;
  dataSeg.name = "__DATA";
  {
    MachSection s;
    s.segmentName = "__DATA";
    s.sectionName = "__objc_classlist";
    s.fileOffset = 0x500;
    s.size = 0x40;
    dataSeg.sections.push_back(s);
  }
  slice.segments.push_back(dataSeg);

  EXPECT_NE(slice.objcClassNameSection(), nullptr);
  EXPECT_NE(slice.objcMethNameSection(), nullptr);
  EXPECT_NE(slice.objcClasslistSection(), nullptr);
  EXPECT_EQ(slice.objcMethTypeSection(), nullptr);
  EXPECT_EQ(slice.objcCatlistSection(), nullptr);

  EXPECT_EQ(slice.objcClassNameSection()->fileOffset, 0x100u);
  EXPECT_EQ(slice.objcMethNameSection()->fileOffset, 0x200u);
  EXPECT_EQ(slice.objcClasslistSection()->fileOffset, 0x500u);
}

// ════════════════���════════════════════════��══════════════════════════
//  Integration tests — require asset binary on disk
//  Skip on CI: --gtest_filter=-Integration.*
// ════════════════════════════════════════════════════════════════════

TEST(Integration, MachReaderLoadsRealBinary) {
  if (kBinaryPath.empty()) {
    GTEST_SKIP() << "TEST_OBJC_ABSOLUTE_PATH is not defined, skipping test "
                    "that requires a real binary.";
  }
  MachOImage image = loadMachOImage(kBinaryPath);
  ASSERT_FALSE(image.slices.empty());

  for (const auto& slice : image.slices) {
    bool hasText = false;
    for (const auto& seg : slice.segments) {
      if (seg.name == "__TEXT") {
        hasText = true;
        break;
      }
    }
    EXPECT_TRUE(hasText) << "Slice missing __TEXT segment";
  }
}
