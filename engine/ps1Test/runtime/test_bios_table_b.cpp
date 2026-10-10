#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/cpu_context.h"
#include "runtime/memory.h"
#include "runtime/input/input.h"
#include <gtest/gtest.h>

using namespace ps1;
using namespace ps1::bios;

class BiosTableBTest : public ::testing::Test {
protected:
  Memory mem;
  recomp_context ctx;
  cdrom::VirtualFs fs;
  std::unique_ptr<Bios> bios;

  void SetUp() override {
    ctx.reset();
    ctx.mem = &mem;
    bios = std::make_unique<Bios>(ctx, fs, mem);

    // Simulate setting up a basic file in VirtualFs for testing
    // Since VirtualFs requires actual files on disk, we will mock simple
    // behavior or just rely on the API returning fd if passing an existent
    // dummy file or fake.
  }

  void writeString(uint32_t addr, const char *str) {
    while (*str) {
      mem.write8(addr++, *str++);
    }
    mem.write8(addr, '\0');
  }
};

TEST_F(BiosTableBTest, OpenCloseRead) {
  // We can't easily mock the real filesystem inside the unit test without
  // creating a temp file. We will assume the VirtualFs behavior returns -1
  // (error) for non-existent files.
  writeString(0x1000, "cdrom:\\NONEXISTENT.TXT");
  ctx.r[A0] = 0x1000;
  ctx.r[A1] = 1;    // Read mode
  ctx.r[T1] = 0x32; // open
  bios->executeB0();

  // V0 should be a negative descriptor since it fails to open
  EXPECT_EQ(ctx.r[V0], 0xFFFFFFFF);
}

TEST_F(BiosTableBTest, EventSystemProxy) {
  // Test Event creation proxy
  ctx.r[A0] = 0x09;   // class
  ctx.r[A1] = 0x02;   // spec
  ctx.r[A2] = 0x1000; // mode
  ctx.r[A3] = 0x0000; // func

  ctx.r[T1] = 0x08; // openEvent
  bios->executeB0();

  uint32_t eventId = ctx.r[V0];
  EXPECT_NE(eventId, 0xFFFFFFFF);

  // enableEvent
  ctx.r[A0] = eventId;
  ctx.r[T1] = 0x0C; // enableEvent
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 1); // Success

  // testEvent
  ctx.r[A0] = eventId;
  ctx.r[T1] = 0x0B; // testEvent
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 0); // Not triggered yet

  // closeEvent
  ctx.r[A0] = eventId;
  ctx.r[T1] = 0x09; // closeEvent
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 1); // Success
}

TEST_F(BiosTableBTest, KernelTablesSurviveThreadRegisterChanges) {
  ctx.r[K0] = 0;
  ctx.r[K1] = 0;
  ctx.r[T1] = 0x56;
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 0x801FFF80u);
  ctx.r[T1] = 0x57;
  bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 0x801FFE00u);
}

TEST_F(BiosTableBTest, RetailDeviceDiscoveryReachesMemoryCardDirectory) {
  input::InputController input;
  bios->setInputController(&input);
  // A completed-game name must be visible through the same device lookup
  // used by the retail firstfile wrapper, before any B0:42 invocation.
  ASSERT_TRUE(input.getMemoryCard(0).createFile("BASLUS-00594G021qCOA", 1));
  const auto table = mem.read32(0x150), bytes = mem.read32(0x154);
  ASSERT_GT(bytes, 0u);
  ASSERT_EQ(bytes % 0x50, 0u);
  uint32_t cardDevice = 0;
  for (uint32_t offset = 0; offset < bytes; offset += 0x50) {
    const auto name = mem.read32(table + offset);
    if (name && mem.read8(name) == 'b' && mem.read8(name + 1) == 'u' &&
        mem.read8(name + 2) == 0) cardDevice = table + offset;
  }
  ASSERT_NE(cardDevice, 0u);
  mem.write32(cardDevice + 0x34, 0x8009C7C4); // retail wrapper installs its hook
  writeString(0x1000, "bu00:*");
  ctx.r[T1] = 0x42; ctx.r[A0] = 0x1000; ctx.r[A1] = 0x2000;
  bios->executeB0();
  ASSERT_EQ(ctx.r[V0], 0x2000u);
  EXPECT_EQ(mem.read8(0x2000 + 12), 'G');
  EXPECT_EQ(mem.read32(0x2000 + 24), 8192u);
  EXPECT_EQ(mem.read32(cardDevice + 0x34), 0x8009C7C4u);
  ctx.r[T1] = 0x43; ctx.r[A0] = 0x2000; bios->executeB0();
  EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(BiosTableBTest, KernelAllocatorHonorsArenaAndOverflow) {
  ctx.r[A0] = 0xA000E800;
  ctx.r[A1] = 32;
  ctx.r[T1] = 8;
  bios->executeC0();
  auto allocate = [&](uint32_t bytes) {
    ctx.r[A0] = bytes;
    ctx.r[T1] = 0;
    bios->executeB0();
    return ctx.r[V0];
  };
  EXPECT_EQ(allocate(1), 0xA000E800u);
  EXPECT_EQ(allocate(9), 0xA000E808u);
  EXPECT_EQ(allocate(0xFFFFFFFFu), 0u);
  EXPECT_EQ(allocate(16), 0u);
  EXPECT_EQ(allocate(8), 0xA000E818u);
}

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
extern void (*g_testRecompDispatchHook)(recomp_context *, uint32_t);
namespace {
KernelThreads *threadProbe = nullptr;
int threadProbePhase = 0;
void runThreadProbe(recomp_context *ctx, uint32_t address) {
  EXPECT_EQ(address, 0x80012340u);
  EXPECT_EQ(ctx->r[SP], 0x801e0000u);
  EXPECT_EQ(ctx->r[GP], 0x800abc00u);
  volatile uint32_t retainedLocal = 0x12345678;
  ctx->r[S0] = 0xfeed1234;
  threadProbePhase = 1;
  threadProbe->change(0xff000000);
  EXPECT_EQ(retainedLocal, 0x12345678u);
  EXPECT_EQ(ctx->r[S0], 0xfeed1234u);
  threadProbePhase = 2;
  threadProbe->change(0xff000000);
  ADD_FAILURE() << "Test worker must remain suspended";
}
}
TEST_F(BiosTableBTest, NativeThreadSwitchRetainsStackAndGuestRegisters) {
  mem.write32(0x108, 0x80001000);
  mem.write32(0x110, 0x80001100);
  mem.write32(0x114, 2 * 0xc0);
  mem.write32(0x1000, 0x80001100);
  mem.write32(0x1100, 0x4000);
  mem.write32(0x11c0, 0x1000);
  ctx.r[SP] = 0x801f0000;
  ctx.r[S0] = 0xabcd0001;
  KernelThreads threads(ctx);
  threadProbe = &threads;
  threadProbePhase = 0;
  struct HookReset {
    ~HookReset() { g_testRecompDispatchHook = nullptr; threadProbe = nullptr; }
  } reset;
  g_testRecompDispatchHook = runThreadProbe;
  const auto handle = threads.open(0x80012340, 0x801e0000, 0x800abc00);
  ASSERT_EQ(handle, 0xff000001u);
  threads.change(handle);
  EXPECT_EQ(threadProbePhase, 1);
  EXPECT_EQ(ctx.r[SP], 0x801f0000u);
  EXPECT_EQ(ctx.r[S0], 0xabcd0001u);
  EXPECT_EQ(mem.read32(0x1000), 0x80001100u);
  threads.change(handle);
  EXPECT_EQ(threadProbePhase, 2);
  EXPECT_EQ(ctx.r[S0], 0xabcd0001u);
}
#endif

#include "runtime/psyq/psyq_state.h"
extern void (*g_testRecompDispatchHook)(recomp_context *, uint32_t);
namespace {
unsigned cardHardwareCompletions = 0;
unsigned cardSoftwareCompletions = 0;
void countCardCompletion(recomp_context *, uint32_t address) {
  if (address == 0x80012300) ++cardHardwareCompletions;
  if (address == 0x80012304) ++cardSoftwareCompletions;
}
}
TEST_F(BiosTableBTest, FileWriteDeliversOneHardwareEventPerSectorThenSoftwareCompletion) {
  psyq::psyq_state().reset();
  input::InputController input;
  bios->setInputController(&input);
  auto registerEvent = [&](uint32_t eventClass, uint32_t callback) {
    ctx.r[T1] = 8; ctx.r[A0] = eventClass; ctx.r[A1] = 4;
    ctx.r[A2] = 0x1000; ctx.r[A3] = callback; bios->executeB0();
    ctx.r[A0] = ctx.r[V0]; ctx.r[T1] = 0x0c; bios->executeB0();
  };
  registerEvent(0xf0000011, 0x80012300);
  registerEvent(0xf4000001, 0x80012304);
  cardHardwareCompletions = cardSoftwareCompletions = 0;
  g_testRecompDispatchHook = countCardCompletion;
  struct ResetHook { ~ResetHook() { g_testRecompDispatchHook = nullptr; } } reset;
  writeString(0x1000, "bu00:TESTFILE");
  ctx.r[T1] = 0; ctx.r[A0] = 0x1000; ctx.r[A1] = 0x10202; bios->executeA0();
  const auto fd = ctx.r[V0];
  ASSERT_LT(fd, 16u);
  for (unsigned i = 0; i < 512; ++i) mem.write8(0x2000 + i, uint8_t(i));
  ctx.r[T1] = 3; ctx.r[A0] = fd; ctx.r[A1] = 0x2000; ctx.r[A2] = 512;
  bios->executeA0();
  ASSERT_EQ(ctx.r[V0], 512u);
  EXPECT_EQ(cardHardwareCompletions, 0u);
  for (unsigned i = 1; i <= 4; ++i) {
    bios->drainPendingCallbacks();
    EXPECT_EQ(cardHardwareCompletions, i);
    EXPECT_EQ(cardSoftwareCompletions, 0u);
  }
  bios->drainPendingCallbacks();
  EXPECT_EQ(cardSoftwareCompletions, 1u);
}

#if defined(__unix__) || defined(__APPLE__) || defined(_WIN32)
namespace {
void runPortableThreadProbe(recomp_context *ctx, uint32_t address) {
  if (address == 0x80012340u) {
    ctx->r[S0] = 0x1234abcd;
    ctx->r31 = 0x80012348;
    threadProbe->change(0xff000000);
    ADD_FAILURE() << "Old native stack was resumed instead of reconstructed";
  } else {
    EXPECT_EQ(address, 0x80012348u);
    EXPECT_EQ(ctx->r[S0], 0x1234abcdu);
    threadProbePhase = 2;
    threadProbe->change(0xff000000);
  }
}
}
TEST_F(BiosTableBTest, PortableThreadSnapshotRebuildsWithoutOldHostStacks) {
  mem.write32(0x108, 0x80001000);
  mem.write32(0x110, 0x80001100);
  mem.write32(0x114, 2 * 0xc0);
  mem.write32(0x1000, 0x80001100);
  mem.write32(0x1100, 0x4000);
  mem.write32(0x11c0, 0x1000);
  ctx.r[SP] = 0x801f0000;
  ctx.r31 = 0x80010010;
  KernelThreads threads(ctx);
  threadProbe = &threads;
  threadProbePhase = 0;
  struct Reset { ~Reset() { g_testRecompDispatchHook = nullptr; threadProbe = nullptr; } } reset;
  g_testRecompDispatchHook = runPortableThreadProbe;
  const auto handle = threads.open(0x80012340, 0x801e0000, 0x800abc00);
  threads.change(handle);
  threads.captureCurrent(0x80010010);
  ASSERT_TRUE(threads.canSnapshot());
  const auto snapshot = threads.snapshot();
  EXPECT_EQ(snapshot.currentTcb, 0x80001100u);
  EXPECT_EQ(snapshot.resumePcs[1], 0x80012348u);
  threads.restart(snapshot);
  // Save again before the worker runs: its reconstructed host fiber does
  // not exist yet, but the guest continuation must survive another restore.
  const auto dormantSnapshot = threads.snapshot();
  EXPECT_EQ(dormantSnapshot.resumePcs[1], 0x80012348u);
  threads.restart(dormantSnapshot);
  threads.change(handle);
  EXPECT_EQ(threadProbePhase, 2);
}

TEST_F(BiosTableBTest, DormantThreadRecoversLegacyOmittedContinuationFromTcb) {
  mem.write32(0x108, 0x80001000);
  mem.write32(0x110, 0x80001100);
  mem.write32(0x114, 2 * 0xc0);
  mem.write32(0x1000, 0x80001100);
  mem.write32(0x1100, 0x4000);
  mem.write32(0x11c0, 0x4000);
  mem.write32(0x11c0 + 0x88, 0x80012348);
  mem.write32(0x11c0 + 8 + S0 * 4, 0x1234abcd);
  mem.write32(0x11c0 + 8 + SP * 4, 0x801e0000);
  ctx.r[SP] = 0x801f0000;
  ctx.r31 = 0x80010010;
  KernelThreads threads(ctx);
  threadProbe = &threads;
  threadProbePhase = 0;
  struct Reset { ~Reset() { g_testRecompDispatchHook = nullptr; threadProbe = nullptr; } } reset;
  g_testRecompDispatchHook = runPortableThreadProbe;
  KernelThreads::Snapshot snapshot;
  snapshot.currentTcb = 0x80001100;
  snapshot.resumePcs[0] = 0x80010010;
  threads.restart(snapshot);
  EXPECT_EQ(threads.snapshot().resumePcs[1], 0x80012348u);
  threads.change(0xff000001);
  EXPECT_EQ(threadProbePhase, 2);
}
#endif
