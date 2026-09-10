// Tests for emuptr<T> and EMUGLOBALVAR / EMUGLOBALARR.

#include <gtest/gtest.h>
#include <runtime/emu_globals.h>
#include <runtime/emuptr.h>

#include <array>
#include <cstdint>
#include <cstring>

using namespace ps1;

namespace {

// Mirror of nspage from c1c, used by the Phase 2 NS subsystem port.
struct nspage_test {
  uint16_t magic;
  uint16_t type;
  uint32_t pagenum;
  int32_t entrycount;
  uint32_t checksum;
  // entries[] follows
};

struct test_fixture {
  std::array<uint8_t, 2 * 1024 * 1024> ram{}; // 2 MB to match PS1

  test_fixture() {
    ram.fill(0);
    emuptr_set_ram(ram.data());
  }
  ~test_fixture() { emuptr_set_ram(nullptr); }
};

// EMUGLOBALVAR / EMUGLOBALARR can only declare at namespace scope, so put
// them outside the function bodies.
EMUGLOBALVAR(uint32_t, hash_table_base, 0x8005C530);
EMUGLOBALARR(uint32_t, dispatch_table, 0x80058000);

} // namespace

TEST(Emuptr, ReadWriteScalar) {
  test_fixture f;

  // Word at PS1 virtual 0x80000100 must land at host RAM offset 0x100.
  emuptr<uint32_t> p(0x80000100);
  *p = 0xDEADBEEF;

  uint32_t roundtrip;
  std::memcpy(&roundtrip, f.ram.data() + 0x100, sizeof(roundtrip));
  EXPECT_EQ(roundtrip, 0xDEADBEEFu);
  EXPECT_EQ(*p, 0xDEADBEEFu);
}

TEST(Emuptr, StructFieldAccess) {
  test_fixture f;

  emuptr<nspage_test> p(0x80001000);
  p->magic = 0x1234;
  p->type = 0x7;
  p->entrycount = 42;

  // Verify field-by-field via raw RAM (little-endian PS1).
  EXPECT_EQ(f.ram[0x1000], 0x34);
  EXPECT_EQ(f.ram[0x1001], 0x12);
  EXPECT_EQ(f.ram[0x1002], 0x07);
  EXPECT_EQ(f.ram[0x1003], 0x00);
  EXPECT_EQ(p->magic, 0x1234u);
  EXPECT_EQ(p->type, 0x0007u);
  EXPECT_EQ(p->entrycount, 42);
}

TEST(Emuptr, ArrayIndexing) {
  test_fixture f;

  emuptr<uint32_t> arr(0x80002000);
  for (int i = 0; i < 16; ++i) {
    arr[i] = 0x1000u + static_cast<uint32_t>(i);
  }

  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(arr[i], 0x1000u + static_cast<uint32_t>(i));
  }
}

TEST(Emuptr, AddressConversion) {
  emuptr<uint32_t> p(0x80012345);
  uint32_t addr = static_cast<uint32_t>(p);
  EXPECT_EQ(addr, 0x80012345u);
  EXPECT_EQ(p.addr(), 0x80012345u);
}

TEST(Emuptr, NullPredicate) {
  emuptr<uint32_t> n;
  EXPECT_TRUE(n.null());
  EXPECT_FALSE(static_cast<bool>(n));
  EXPECT_EQ(n.addr(), 0u);

  emuptr<uint32_t> p(0x80001000);
  EXPECT_FALSE(p.null());
  EXPECT_TRUE(static_cast<bool>(p));
}

TEST(Emuptr, PointerArithmetic) {
  emuptr<uint32_t> p(0x80001000);
  emuptr<uint32_t> p4 = p + 4;
  EXPECT_EQ(p4.addr(), 0x80001010u); // 4 elements * sizeof(uint32_t)

  emuptr<uint32_t> pm2 = p - 2;
  EXPECT_EQ(pm2.addr(), 0x80000FF8u);

  // Pre / post increment respect element stride.
  emuptr<uint32_t> q(0x80001000);
  ++q;
  EXPECT_EQ(q.addr(), 0x80001004u);
  emuptr<uint32_t> old = q++;
  EXPECT_EQ(old.addr(), 0x80001004u);
  EXPECT_EQ(q.addr(), 0x80001008u);
}

TEST(Emuptr, KsegMirroring) {
  test_fixture f;

  // Same physical RAM offset via KUSEG, KSEG0, KSEG1.
  emuptr<uint32_t> kuseg(0x00001234);
  emuptr<uint32_t> kseg0(0x80001234);
  emuptr<uint32_t> kseg1(0xA0001234);

  *kuseg = 0xCAFEBABE;
  EXPECT_EQ(*kseg0, 0xCAFEBABEu);
  EXPECT_EQ(*kseg1, 0xCAFEBABEu);

  *kseg1 = 0x55555555;
  EXPECT_EQ(*kuseg, 0x55555555u);
}

TEST(Emuptr, RamMirrorsBelow8MB) {
  test_fixture f;

  // PS1 mirrors the 2 MB RAM across the first 8 MB of the physical space.
  // 0x80200000 and 0x80000000 must alias the same RAM byte.
  emuptr<uint32_t> p_low(0x80000200);
  emuptr<uint32_t> p_mirror(0x80200200);
  emuptr<uint32_t> p_mirror2(0x80400200);

  *p_low = 0x11223344;
  EXPECT_EQ(*p_mirror, 0x11223344u);
  EXPECT_EQ(*p_mirror2, 0x11223344u);

  *p_mirror2 = 0xFEDCBA98;
  EXPECT_EQ(*p_low, 0xFEDCBA98u);
}

TEST(Emuptr, Equality) {
  emuptr<uint32_t> a(0x80001000);
  emuptr<uint32_t> b(0x80001000);
  emuptr<uint32_t> c(0x80001004);

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_LT(a, c);
}

TEST(EmuGlobals, ScalarAccessor) {
  test_fixture f;

  hash_table_base() = 0xABCD1234;

  // 0x8005C530 → phys 0x0005C530 (mask 0x1FFFFFFF, < 0x800000 so & 0x1FFFFF)
  uint32_t roundtrip;
  std::memcpy(&roundtrip, f.ram.data() + 0x5C530, sizeof(roundtrip));
  EXPECT_EQ(roundtrip, 0xABCD1234u);
  EXPECT_EQ(hash_table_base(), 0xABCD1234u);
}

TEST(EmuGlobals, ArrayAccessor) {
  test_fixture f;

  for (int i = 0; i < 8; ++i) {
    dispatch_table()[i] = 0x80010000u + static_cast<uint32_t>(i);
  }

  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(dispatch_table()[i], 0x80010000u + static_cast<uint32_t>(i));
  }
}
