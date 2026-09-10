// Tests for ps1Runtime -- Memory Subsystem
// Validates RAM, scratchpad, BIOS, address mirroring, and byte order

#include <gtest/gtest.h>
#include <runtime/memory.h>

#include <vector>

using namespace ps1;

// Basic Read/Write

TEST(Memory, ReadWrite8) {
  Memory mem;
  mem.write8(0x00000000, 0x42);
  EXPECT_EQ(mem.read8(0x00000000), 0x42u);

  mem.write8(0x001FFFFF, 0xFF);
  EXPECT_EQ(mem.read8(0x001FFFFF), 0xFFu);
}

TEST(Memory, ReadWrite16LittleEndian) {
  Memory mem;
  mem.write16(0x00001000, 0xBEEF);
  EXPECT_EQ(mem.read16(0x00001000), 0xBEEFu);

  // Verify byte order (little-endian)
  EXPECT_EQ(mem.read8(0x00001000), 0xEF); // low byte
  EXPECT_EQ(mem.read8(0x00001001), 0xBE); // high byte
}

TEST(Memory, ReadWrite32LittleEndian) {
  Memory mem;
  mem.write32(0x00002000, 0xDEADBEEF);
  EXPECT_EQ(mem.read32(0x00002000), 0xDEADBEEFu);

  // Verify byte order (LE: 0xDEADBEEF -> EF BE AD DE)
  EXPECT_EQ(mem.read8(0x00002000), 0xEF);
  EXPECT_EQ(mem.read8(0x00002001), 0xBE);
  EXPECT_EQ(mem.read8(0x00002002), 0xAD);
  EXPECT_EQ(mem.read8(0x00002003), 0xDE);
}

// Address Mirroring (KSEG0 / KSEG1)

TEST(Memory, KSEG0Mirror) {
  Memory mem;
  // Write via KUSEG (physical)
  mem.write32(0x00010000, 0xCAFEBABE);

  // Read via KSEG0 (0x80000000 mirror)
  EXPECT_EQ(mem.read32(0x80010000), 0xCAFEBABEu);
}

TEST(Memory, KSEG1Mirror) {
  Memory mem;
  // Write via KSEG1 (0xA0000000 mirror)
  mem.write32(0xA0020000, 0x12345678);

  // Read via KUSEG and KSEG0
  EXPECT_EQ(mem.read32(0x00020000), 0x12345678u);
  EXPECT_EQ(mem.read32(0x80020000), 0x12345678u);
}

TEST(Memory, CrossMirrorWrite) {
  Memory mem;
  // Write via KSEG0, read via KSEG1
  mem.write32(0x80030000, 0xAAAABBBB);
  EXPECT_EQ(mem.read32(0xA0030000), 0xAAAABBBBu);
}

// Scratchpad (Data Cache)

TEST(Memory, ScratchpadReadWrite) {
  Memory mem;
  mem.write32(0x1F800000, 0x11223344);
  EXPECT_EQ(mem.read32(0x1F800000), 0x11223344u);

  mem.write8(0x1F8003FF, 0xAB);
  EXPECT_EQ(mem.read8(0x1F8003FF), 0xABu);
}

TEST(Memory, ScratchpadIsolated) {
  Memory mem;
  // Scratchpad and RAM are separate
  mem.write32(0x1F800000, 0xAAAAAAAA);
  mem.write32(0x00000000, 0xBBBBBBBB);

  EXPECT_EQ(mem.read32(0x1F800000), 0xAAAAAAAAu);
  EXPECT_EQ(mem.read32(0x00000000), 0xBBBBBBBBu);
}

TEST(Memory, ScratchpadWrite32FiresWatchpoint) {
  Memory mem;
  uint32_t observed = 0;
  uint32_t calls = 0;
  mem.setWriteWatchpoint(0x1F80000C, [&](uint32_t value) {
    observed = value;
    ++calls;
  });

  mem.write32(0x1F80000C, 0x80123456);

  EXPECT_EQ(mem.read32(0x1F80000C), 0x80123456u);
  EXPECT_EQ(observed, 0x80123456u);
  EXPECT_EQ(calls, 1u);
}

TEST(Memory, ScratchpadPartialWriteFiresWordWatchpoint) {
  Memory mem;
  std::vector<uint32_t> observed;
  mem.write32(0x1F80000C, 0xAABBCCDD);
  mem.setWriteWatchpoint(0x1F80000C,
                         [&](uint32_t value) { observed.push_back(value); });

  mem.write16(0x1F80000D, 0x1122);

  ASSERT_EQ(observed.size(), 2u);
  EXPECT_EQ(observed[0], 0xAABB22DDu);
  EXPECT_EQ(observed[1], 0xAA1122DDu);
  EXPECT_EQ(mem.read32(0x1F80000C), 0xAA1122DDu);
}

// BIOS ROM

TEST(Memory, BiosReadOnly) {
  Memory mem;
  // BIOS initializes as 0xFF
  EXPECT_EQ(mem.read8(0x1FC00000), 0xFFu);

  // Writes to BIOS are ignored
  mem.write8(0x1FC00000, 0x42);
  EXPECT_EQ(mem.read8(0x1FC00000), 0xFFu);
}

TEST(Memory, BiosLoadViaDirect) {
  Memory mem;
  // Load BIOS data directly
  uint8_t *bios = mem.biosPtr();
  bios[0] = 0x50; // 'P'
  bios[1] = 0x53; // 'S'

  EXPECT_EQ(mem.read8(0x1FC00000), 0x50u);
  EXPECT_EQ(mem.read8(0x1FC00001), 0x53u);
}

// Reset

TEST(Memory, Reset) {
  Memory mem;
  mem.write32(0x00010000, 0xDEADBEEF);
  mem.write32(0x1F800000, 0xCAFEBABE);

  mem.reset();

  EXPECT_EQ(mem.read32(0x00010000), 0u);
  EXPECT_EQ(mem.read32(0x1F800000), 0u);
  EXPECT_EQ(mem.read8(0x1FC00000), 0xFFu); // BIOS stays 0xFF
}

// Unmapped Regions

TEST(Memory, UnmappedReturnsZero) {
  Memory mem;
  // I/O port range -- returns 0 for now
  EXPECT_EQ(mem.read32(0x1F801000), 0u);
}

// Direct Pointer Access

TEST(Memory, DirectRamAccess) {
  Memory mem;
  uint8_t *ram = mem.ramPtr();
  ram[0x100] = 0xAA;
  ram[0x101] = 0xBB;

  EXPECT_EQ(mem.read16(0x00000100), 0xBBAAu);
  EXPECT_EQ(mem.read16(0x80000100), 0xBBAAu); // KSEG0 mirror
}

namespace {

PGXPValue projectedValue(uint32_t packed, float x, float y, float z) {
  PGXPValue value;
  value.x = x;
  value.y = y;
  value.z = z;
  value.value = packed;
  value.flags = PGXPValue::VALID_XY | PGXPValue::VALID_Z;
  value.origin = PGXPOrigin::GteProjection;
  return value;
}

} // namespace

TEST(Memory, AlignedBulkCopyPreservesPgxpXYZ) {
  Memory mem;
  constexpr uint32_t packed = 0x00420021u;
  const PGXPValue projected = projectedValue(packed, 33.25f, 66.5f, 7.75f);
  mem.write32Tracked(0x80001000u, packed, &projected);

  mem.copyBytes(0xA0002000u, 0x80001000u, sizeof(packed));

  PGXPValue copied;
  ASSERT_TRUE(mem.lookupPrecision(0x00002000u, packed, copied));
  EXPECT_TRUE(copied.hasXYZ(packed));
  EXPECT_FLOAT_EQ(copied.x, projected.x);
  EXPECT_FLOAT_EQ(copied.y, projected.y);
  EXPECT_FLOAT_EQ(copied.z, projected.z);
}

TEST(Memory, OverlappingBulkCopySnapshotsPgxpLikeMemmove) {
  Memory mem;
  constexpr uint32_t first = 0x00200010u;
  constexpr uint32_t second = 0x00400030u;
  const PGXPValue firstProjection = projectedValue(first, 16.5f, 32.5f, 3.0f);
  const PGXPValue secondProjection = projectedValue(second, 48.5f, 64.5f, 6.0f);
  mem.write32Tracked(0x1000u, first, &firstProjection);
  mem.write32Tracked(0x1004u, second, &secondProjection);

  mem.copyBytes(0x1004u, 0x1000u, 8u);

  PGXPValue copiedFirst;
  PGXPValue copiedSecond;
  ASSERT_TRUE(mem.lookupPrecision(0x1004u, first, copiedFirst));
  ASSERT_TRUE(mem.lookupPrecision(0x1008u, second, copiedSecond));
  EXPECT_FLOAT_EQ(copiedFirst.z, firstProjection.z);
  EXPECT_FLOAT_EQ(copiedSecond.z, secondProjection.z);
}

TEST(Memory, PartialBulkCopyInvalidatesDestinationPgxpWord) {
  Memory mem;
  constexpr uint32_t source = 0x44332211u;
  constexpr uint32_t destination = 0x88776655u;
  const PGXPValue sourceProjection =
      projectedValue(source, 17.25f, 34.5f, 2.0f);
  const PGXPValue destinationProjection =
      projectedValue(destination, 85.25f, 102.5f, 4.0f);
  mem.write32Tracked(0x1000u, source, &sourceProjection);
  mem.write32Tracked(0x2000u, destination, &destinationProjection);

  mem.copyBytes(0x2001u, 0x1001u, 2u);

  PGXPValue copied;
  EXPECT_FALSE(mem.lookupPrecision(0x2000u, mem.read32(0x2000u), copied));
}

TEST(Memory, MisalignedBulkCopyDoesNotInventPgxpWord) {
  Memory mem;
  constexpr uint32_t source = 0x44332211u;
  const PGXPValue sourceProjection =
      projectedValue(source, 17.25f, 34.5f, 2.0f);
  mem.write32Tracked(0x1000u, source, &sourceProjection);

  mem.copyBytes(0x2001u, 0x1000u, sizeof(source));

  PGXPValue copied;
  EXPECT_FALSE(mem.lookupPrecision(0x2000u, mem.read32(0x2000u), copied));
}

TEST(Memory, SamePhysicalAliasBulkCopyIsANoop) {
  Memory mem;
  constexpr uint32_t packed = 0x00420021u;
  const PGXPValue projected = projectedValue(packed, 33.25f, 66.5f, 7.75f);
  mem.write32Tracked(0x80001000u, packed, &projected);

  mem.copyBytes(0xA0001000u, 0x80001000u, sizeof(packed));

  PGXPValue unchanged;
  ASSERT_TRUE(mem.lookupPrecision(0x1000u, packed, unchanged));
  EXPECT_TRUE(unchanged.hasXYZ(packed));
  EXPECT_FLOAT_EQ(unchanged.z, projected.z);
}

TEST(Memory, BulkCopyPreservesInteriorWordWithEqualUnalignedOffsets) {
  Memory mem;
  constexpr uint32_t packed = 0x00420021u;
  const PGXPValue projected = projectedValue(packed, 33.25f, 66.5f, 7.75f);
  mem.write32Tracked(0x1004u, packed, &projected);

  mem.copyBytes(0x2001u, 0x1001u, 7u);

  PGXPValue copied;
  ASSERT_TRUE(mem.lookupPrecision(0x2004u, packed, copied));
  EXPECT_TRUE(copied.hasXYZ(packed));
}

TEST(Memory, BulkCopyDoesNotAmplifyStaleSourceSidecar) {
  Memory mem;
  constexpr uint32_t oldPacked = 0x00420021u;
  constexpr uint32_t newPacked = 0x00640043u;
  const PGXPValue projected = projectedValue(oldPacked, 33.25f, 66.5f, 7.75f);
  mem.write32Tracked(0x1000u, oldPacked, &projected);
  std::memcpy(mem.ramPtr() + 0x1000u, &newPacked, sizeof(newPacked));

  mem.copyBytes(0x2000u, 0x1000u, sizeof(newPacked));

  PGXPValue copied;
  EXPECT_FALSE(mem.lookupPrecision(0x2000u, newPacked, copied));
}

TEST(Memory, ScratchpadStoresAndInvalidatesPgxpXYZ) {
  Memory mem;
  constexpr uint32_t packed = 0x00420021u;
  const PGXPValue projected = projectedValue(packed, 33.25f, 66.5f, 7.75f);
  mem.write32Tracked(0x1F800100u, packed, &projected);

  PGXPValue stored;
  ASSERT_TRUE(mem.lookupPrecision(0x1F800100u, packed, stored));
  EXPECT_TRUE(stored.hasXYZ(packed));

  mem.write8(0x1F800101u, 0xAAu);
  EXPECT_FALSE(
      mem.lookupPrecision(0x1F800100u, mem.read32(0x1F800100u), stored));
}

TEST(Memory, BulkCopyPreservesPgxpBetweenRamAndScratchpad) {
  Memory mem;
  constexpr uint32_t packed = 0x00420021u;
  const PGXPValue projected = projectedValue(packed, 33.25f, 66.5f, 7.75f);
  mem.write32Tracked(0x1000u, packed, &projected);

  mem.copyBytes(0x1F800100u, 0x1000u, sizeof(packed));
  mem.copyBytes(0x2000u, 0x1F800100u, sizeof(packed));

  PGXPValue scratch;
  PGXPValue roundTrip;
  ASSERT_TRUE(mem.lookupPrecision(0x1F800100u, packed, scratch));
  ASSERT_TRUE(mem.lookupPrecision(0x2000u, packed, roundTrip));
  EXPECT_TRUE(scratch.hasXYZ(packed));
  EXPECT_TRUE(roundTrip.hasXYZ(packed));
}

TEST(Memory, ScratchpadBulkCopyAllocatesFreshRamSidecar) {
  Memory mem;
  constexpr uint32_t packed = 0x00420021u;
  const PGXPValue projected = projectedValue(packed, 33.25f, 66.5f, 7.75f);
  mem.write32Tracked(0x1F800100u, packed, &projected);

  mem.copyBytes(0x2000u, 0x1F800100u, sizeof(packed));

  PGXPValue copied;
  ASSERT_TRUE(mem.lookupPrecision(0x2000u, packed, copied));
  EXPECT_TRUE(copied.hasXYZ(packed));
  EXPECT_FLOAT_EQ(copied.z, projected.z);
}

TEST(Memory, WrappedMirrorOverlapSnapshotsPayloadAndPgxp) {
  Memory mem;
  constexpr uint32_t lastWord = 0x00200010u;
  constexpr uint32_t firstWord = 0x00400030u;
  const PGXPValue lastProjection = projectedValue(lastWord, 16.5f, 32.5f, 3.0f);
  const PGXPValue firstProjection =
      projectedValue(firstWord, 48.5f, 64.5f, 6.0f);
  mem.write32Tracked(0x001FFFFCu, lastWord, &lastProjection);
  mem.write32Tracked(0x00000000u, firstWord, &firstProjection);

  mem.copyBytes(0x00000000u, 0x001FFFFCu, 8u);

  EXPECT_EQ(mem.read32(0x00000000u), lastWord);
  EXPECT_EQ(mem.read32(0x00000004u), firstWord);
  PGXPValue copiedLast;
  PGXPValue copiedFirst;
  ASSERT_TRUE(mem.lookupPrecision(0x00000000u, lastWord, copiedLast));
  ASSERT_TRUE(mem.lookupPrecision(0x00000004u, firstWord, copiedFirst));
  EXPECT_FLOAT_EQ(copiedLast.z, lastProjection.z);
  EXPECT_FLOAT_EQ(copiedFirst.z, firstProjection.z);
}
