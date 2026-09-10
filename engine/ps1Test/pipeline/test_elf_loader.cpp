#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <ps1recomp/elf_parser.h>
#include <runtime/memory.h>
#include <vector>

using namespace ps1recomp;
using namespace ps1;

TEST(ElfLoader, LoadsSectionsCorrectly) {
  Memory memory;
  ElfParser parser;

  if (!parser.load("../test_roms/example08_spinningCube.elf")) {
    GTEST_SKIP() << "Test ROM not found. Skipping.";
  }

  auto sections = parser.getSections();
  int loaded = 0;

  for (const auto &sec : sections) {
    if ((sec.type == SectionType::Text || sec.type == SectionType::Data) &&
        sec.data != nullptr) {
      uint32_t phys = Memory::toPhysical(sec.vaddr);
      if (phys < Memory::RAM_SIZE) {
        std::memcpy(memory.ramPtr() + phys, sec.data, sec.size);
        loaded++;
      }
    }
  }

  EXPECT_GT(loaded, 0);
  EXPECT_EQ(loaded, 2);
}
