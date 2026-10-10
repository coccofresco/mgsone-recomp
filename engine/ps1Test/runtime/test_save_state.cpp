#include "runtime/bios/bios.h"
#include "runtime/cdrom/virtual_fs.h"
#include "runtime/gte.h"
#include "runtime/psyq/psyq_pad.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/save_state.h"
#include "runtime/save_files.h"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

using namespace ps1;

class SaveStateTest : public ::testing::Test {
protected:
  std::string tmpPath;

  void SetUp() override {
    // Use a unique path per test instance to avoid parallel-test races.
    auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string name = std::string(info->test_case_name()) + "_" + info->name();
    tmpPath =
        (std::filesystem::temp_directory_path() / ("ps1_test_" + name + ".sav"))
            .string();
  }
  void TearDown() override { std::remove(tmpPath.c_str()); std::filesystem::remove_all(tmpPath + ".backups"); }
};

TEST_F(SaveStateTest, RecoveryUsesLatestReadableBackupAndKeepsDamagedPrimary) {
  Memory mem; gpu::GPU gpu; spu::SPU spu; DMA dma; cdrom::CdromController cd;
  Timers timers; InterruptController irq; input::InputController input; recomp_context ctx{};
  for (unsigned version = 1; version <= 5; ++version) {
    mem.write32(0x1000, version);
    ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  }
  const auto info = SaveState::inspect(tmpPath);
  EXPECT_EQ(info.version, 12u); EXPECT_TRUE(info.supported); EXPECT_FALSE(info.savedAt.empty());
  auto history = storage::backups(tmpPath); ASSERT_EQ(history.size(), 3u);
  std::ofstream(tmpPath, std::ios::trunc) << "broken";
  std::ofstream(history[0], std::ios::trunc) << "also broken";
  mem.write32(0x1000, 999);
  EXPECT_FALSE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  EXPECT_EQ(mem.read32(0x1000), 999u); // strict diagnostic path never falls back
  std::string recovered;
  ASSERT_TRUE(SaveState::loadRecovering(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input, nullptr, nullptr, recovered));
  EXPECT_EQ(recovered, history[1].string()); EXPECT_EQ(mem.read32(0x1000), 3u);
  EXPECT_EQ(std::filesystem::file_size(tmpPath), 6u);
  EXPECT_FALSE(SaveState::isValid(tmpPath));
}

TEST_F(SaveStateTest, FutureVersionIsNeitherOverwrittenNorSilentlyRolledBack) {
  Memory mem; gpu::GPU gpu; spu::SPU spu; DMA dma; cdrom::CdromController cd;
  Timers timers; InterruptController irq; input::InputController input; recomp_context ctx{};
  for (unsigned value : {1, 2}) {
    mem.write32(0x1000, value);
    ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  }
  { std::fstream file(tmpPath, std::ios::in | std::ios::out | std::ios::binary);
    uint32_t future = 13; file.seekp(4); file.write(reinterpret_cast<char *>(&future), sizeof(future)); }
  const auto history = storage::backups(tmpPath);
  std::string recovered; mem.write32(0x1000, 777);
  EXPECT_FALSE(SaveState::loadRecovering(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input, nullptr, nullptr, recovered));
  EXPECT_TRUE(recovered.empty()); EXPECT_EQ(mem.read32(0x1000), 777u);
  EXPECT_FALSE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  EXPECT_EQ(SaveState::inspect(tmpPath).version, 13u); EXPECT_EQ(storage::backups(tmpPath), history);
}

TEST_F(SaveStateTest, LoadingOldTimelinePreservesNewerDiskCardOnItsNextWrite) {
  Memory mem; gpu::GPU gpu; spu::SPU spu; DMA dma; cdrom::CdromController cd;
  Timers timers; InterruptController irq; input::InputController input; recomp_context ctx{};
  const auto cardPath = tmpPath + ".mcd";
  struct Cleanup { std::string path; ~Cleanup() { std::filesystem::remove(path); std::filesystem::remove_all(path+".backups"); } } cleanup{cardPath};
  auto &card = input.getMemoryCard(0); ASSERT_TRUE(card.attachFile(cardPath));
  ASSERT_EQ(card.createFile("progress", 1), 1);
  uint8_t value = 10; ASSERT_EQ(card.writeFile(1, 0, &value, 1), 1);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  value = 20; ASSERT_EQ(card.writeFile(1, 0, &value, 1), 1);
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  input::MemoryCard disk; ASSERT_TRUE(disk.loadFromFile(cardPath));
  ASSERT_EQ(disk.readFile(1, 0, &value, 1), 1); EXPECT_EQ(value, 20); // load alone never rewrites card
  value = 30; ASSERT_EQ(card.writeFile(1, 0, &value, 1), 1);
  const auto previous = storage::backups(cardPath); ASSERT_FALSE(previous.empty());
  ASSERT_TRUE(disk.loadFromFile(previous.front().string()));
  ASSERT_EQ(disk.readFile(1, 0, &value, 1), 1); EXPECT_EQ(value, 20); // pre-load disk progress is recoverable
}

TEST_F(SaveStateTest, SaveAndLoadPreservesMemory) {
  // Set up initial state
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  // Write test pattern to RAM
  mem.write32(0x00000000, 0xDEADBEEF);
  mem.write32(0x00000004, 0xCAFEBABE);
  mem.write8(0x00001000, 0x42);

  // Set CPU registers
  ctx.r[1] = 100;
  ctx.r[31] = 0x80010000;
  ctx.pc = 0x80020000;
  input.setPadType(0, input::PadType::Analog);
  input.press(input::BTN_CROSS | input::BTN_START, 0);
  psyq::psyq_pad_restore({0x80120000u, 0x80120100u, true});

  // Save
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  // Verify file exists
  EXPECT_TRUE(std::filesystem::exists(tmpPath));
  EXPECT_TRUE(SaveState::isValid(tmpPath));

  // Reset everything
  mem.reset();
  ctx.reset();
  ctx.nativeResumePc = 0x8000DEADu;
  ctx.nativeEntryPc = 0x8000BEEFu;
  input.reset();
  psyq::psyq_pad_reset_for_tests();

  // Verify reset worked
  EXPECT_EQ(mem.read32(0x00000000), 0u);
  EXPECT_EQ(ctx.r[1], 0u);

  // Load
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  // Verify restored state
  EXPECT_EQ(mem.read32(0x00000000), 0xDEADBEEFu);
  EXPECT_EQ(mem.read32(0x00000004), 0xCAFEBABEu);
  EXPECT_EQ(mem.read8(0x00001000), 0x42u);
  EXPECT_EQ(ctx.r[1], 100u);
  EXPECT_EQ(ctx.r[31], 0x80010000u);
  EXPECT_EQ(ctx.pc, 0x80020000u);
  EXPECT_EQ(ctx.nativeResumePc, 0u);
  EXPECT_EQ(ctx.nativeEntryPc, 0u);
  EXPECT_EQ(input.getPadType(0), input::PadType::Analog);
  EXPECT_EQ(input.buttonState(0) & (input::BTN_CROSS | input::BTN_START), 0u);
  const auto padState = psyq::psyq_pad_snapshot();
  EXPECT_EQ(padState.buf1Addr, 0x80120000u);
  EXPECT_EQ(padState.buf2Addr, 0x80120100u);
  EXPECT_TRUE(padState.active);
  psyq::psyq_pad_reset_for_tests();
}

namespace {
class ContinuationFs : public cdrom::VirtualFs {
public:
  unsigned reads = 0;
  std::optional<std::vector<uint8_t>> readFile(const std::string &) override {
    ++reads;
    return std::vector<uint8_t>{11, 22, 33, 44, 55};
  }
};
}

class BiosContinuationTest : public SaveStateTest {
protected:
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cd;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx{};
  ContinuationFs fs;
  std::unique_ptr<bios::Bios> biosHle;
  void freshBios() {
    ctx.mem = &mem;
    biosHle = std::make_unique<bios::Bios>(ctx, fs, mem);
    biosHle->setInputController(&input);
  }
  uint32_t call(char table, uint32_t index, uint32_t a0 = 0,
                uint32_t a1 = 0, uint32_t a2 = 0) {
    ctx.r[T1] = index; ctx.r[A0] = a0; ctx.r[A1] = a1; ctx.r[A2] = a2;
    if (table == 'A') biosHle->executeA0();
    if (table == 'B') biosHle->executeB0();
    if (table == 'C') biosHle->executeC0();
    return ctx.r[V0];
  }
  uint32_t str(const std::string &s) {
    for (size_t i = 0; i <= s.size(); ++i) mem.write8(0x4000 + i, s.c_str()[i]);
    return 0x4000;
  }
  bool save() { return SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cd,
                                      timers, irq, input, nullptr, biosHle.get()); }
  bool load() { return SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cd,
                                      timers, irq, input, nullptr, biosHle.get()); }
};

TEST_F(BiosContinuationTest, FreshBiosResumesBothAllocatorsWithoutReinitializingRam) {
  freshBios();
  call('A', 0x39, 0x80100000, 0x10000);
  const auto first = call('A', 0x33, 100);
  ASSERT_NE(first, 0u);
  mem.write32(first, 0x12345678);
  call('C', 0x08, 0x80008000, 0x1000);
  ASSERT_EQ(call('B', 0x00, 17), 0x80008000u);
  ASSERT_TRUE(save());
  const auto expectedUser = call('A', 0x33, 64);
  const auto expectedKernel = call('B', 0x00, 9);
  ASSERT_NE(expectedUser, 0u);
  ASSERT_NE(expectedKernel, 0u);
  for (bool fresh : {false, true}) {
    SCOPED_TRACE(fresh);
    if (fresh) freshBios();
    ASSERT_TRUE(load());
    EXPECT_EQ(call('A', 0x33, 64), expectedUser);
    EXPECT_EQ(call('B', 0x00, 9), expectedKernel);
    EXPECT_EQ(mem.read32(first), 0x12345678u);
  }
}

TEST_F(BiosContinuationTest, BiosRandomSequenceRewindsWithTheTimeline) {
  freshBios();
  call('A', 0x30, 0x12345678);
  call('A', 0x2f);
  ASSERT_TRUE(save());
  std::array<uint32_t, 32> expected;
  for (auto &value : expected) value = call('A', 0x2f);
  for (bool fresh : {false, true}) {
    SCOPED_TRACE(fresh);
    if (fresh) freshBios();
    call('A', 0x30, 0xabcdef);
    ASSERT_TRUE(load());
    for (const auto value : expected) EXPECT_EQ(call('A', 0x2f), value);
  }
}

TEST_F(BiosContinuationTest, FreshBiosResumesOpenCdAndBothCardsAndDirectoryCursor) {
  freshBios();
  const auto cdFd = call('B', 0x32, str("cdrom:\\SYSTEM.CNF;1"));
  ASSERT_EQ(cdFd, 3u);
  ASSERT_EQ(call('B', 0x34, cdFd, 0x5000, 2), 2u);
  uint32_t cardFd[2];
  for (unsigned slot = 0; slot < 2; ++slot) {
    auto &card = input.getMemoryCard(slot);
    ASSERT_EQ(card.createFile("FIRST", 1), 1);
    ASSERT_EQ(card.createFile("SECOND", 1), 2);
    const uint8_t bytes[] = {uint8_t(70 + slot), uint8_t(80 + slot), uint8_t(90 + slot)};
    ASSERT_EQ(card.writeFile(1, 0, bytes, sizeof(bytes)), 3);
    cardFd[slot] = call('B', 0x32, str(slot ? "bu10:FIRST" : "bu00:FIRST"));
    ASSERT_EQ(call('B', 0x34, cardFd[slot], 0x5000, 1), 1u);
  }
  ASSERT_EQ(call('B', 0x42, str("bu00:*"), 0x6000), 0x6000u);
  ASSERT_EQ(mem.read8(0x6000), 'F');
  ASSERT_TRUE(save());
  for (bool fresh : {false, true}) {
    SCOPED_TRACE(fresh);
    if (fresh) freshBios();
    ASSERT_TRUE(load());
    EXPECT_EQ(call('B', 0x34, cdFd, 0x5000, 2), 2u);
    EXPECT_EQ(mem.read8(0x5000), 33);
    EXPECT_EQ(mem.read8(0x5001), 44);
    for (unsigned slot = 0; slot < 2; ++slot) {
      EXPECT_EQ(call('B', 0x34, cardFd[slot], 0x5000, 1), 1u);
      EXPECT_EQ(mem.read8(0x5000), 80 + slot);
    }
    EXPECT_EQ(call('B', 0x43, 0x6000), 0x6000u);
    EXPECT_EQ(mem.read8(0x6000), 'S');
    EXPECT_EQ(call('B', 0x43, 0x6000), 0u);
  }
  EXPECT_EQ(fs.reads, 1u); // restoring must not reopen media or host files
}

TEST_F(BiosContinuationTest, InvalidResourcePayloadNeverReplacesTheLiveTimeline) {
  freshBios();
  call('A', 0x39, 0x80100000, 0x10000);
  ASSERT_TRUE(save());
  std::ifstream source(tmpPath, std::ios::binary);
  const std::vector<uint8_t> original((std::istreambuf_iterator<char>(source)), {});
  source.close();
  // Empty NSV12 resources: 20 allocator bytes, 16 descriptors of 21 bytes,
  // directory count/cursor and RNG seed. The final 64-byte trailer is frozen.
  const size_t resource = original.size() - 64 - 368;
  for (unsigned mutation = 0; mutation < 14; ++mutation) {
    SCOPED_TRACE(mutation);
    auto bytes = original;
    const auto u32 = [&](size_t offset, uint32_t v) {
      std::memcpy(bytes.data() + resource + offset, &v, 4);
    };
    switch (mutation) {
    case 0: u32(4, UINT32_MAX); break; // overflowing heap size
    case 1: u32(8, 0x80100004); break; // inconsistent block-list root
    case 2: u32(12, 0x80010000); u32(16, 0x80008000); break; // reversed arena
    case 3: u32(16, 0x1f800000); break; // non-RAM arena
    case 4: bytes[resource + 20] = 1; break; // reserved descriptor
    case 5: bytes[resource + 83] = 3; break; // invalid device kind
    case 6: u32(84, UINT32_MAX); break; // excessive path length
    case 7: u32(88, UINT32_MAX); break; // excessive data length
    case 8: u32(88, 64 * 1024 * 1024); break; // bounded length, absent bytes
    case 9: u32(92, 1); break; // cursor on a closed descriptor
    case 10: u32(356, 16); break; // impossible directory size
    case 11: u32(360, 1); break; // directory cursor past end
    case 12: bytes.erase(bytes.end() - 65); break; // truncated resources
    case 13: bytes.insert(bytes.end() - 64, 0); break; // unexplained tail
    }
    uint32_t checksum = 2166136261u;
    for (size_t i = 16; i < bytes.size(); ++i) checksum = (checksum ^ bytes[i]) * 16777619u;
    std::memcpy(bytes.data() + 12, &checksum, 4);
    {
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    mem.write32(0x5000, 0xcafebabe);
    ctx.r[1] = 12345;
    input.press(input::BTN_CROSS);
    const auto before = biosHle->snapshot();
    EXPECT_FALSE(load());
    EXPECT_EQ(mem.read32(0x5000), 0xcafebabeu);
    EXPECT_EQ(ctx.r[1], 12345u);
    EXPECT_EQ(input.buttonState() & input::BTN_CROSS, 0);
    EXPECT_EQ(biosHle->snapshot().heap.baseAddress, before.heap.baseAddress);
    EXPECT_EQ(biosHle->snapshot().kernelHeapNext, before.kernelHeapNext);
  }
}

TEST_F(BiosContinuationTest, MissingCardAttachmentRejectsBeforeRamCommit) {
  freshBios();
  ASSERT_EQ(input.getMemoryCard(1).createFile("FIRST", 1), 1);
  ASSERT_EQ(call('B', 0x32, str("bu10:FIRST")), 3u);
  ASSERT_TRUE(save());
  freshBios();
  biosHle->setInputController(nullptr);
  mem.write32(0x5000, 0xcafebabe);
  EXPECT_FALSE(load());
  EXPECT_EQ(mem.read32(0x5000), 0xcafebabeu);
  biosHle->setInputController(&input);
  EXPECT_TRUE(load());
  EXPECT_EQ(call('B', 0x34, 3, 0x5000, 1), 1u);
}

TEST_F(BiosContinuationTest, LegacyStateClearsResourcesFromAbandonedTimeline) {
  freshBios();
  ASSERT_TRUE(save());
  std::ifstream source(tmpPath, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(source)), {});
  source.close();
  bytes.erase(bytes.end() - 64 - 368, bytes.end() - 64);
  SaveStateHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  header.version = 11;
  header.flags = 0x4256534e;
  header.checksum = 2166136261u;
  for (size_t i = sizeof(header); i < bytes.size(); ++i)
    header.checksum = (header.checksum ^ uint8_t(bytes[i])) * 16777619u;
  std::memcpy(bytes.data(), &header, sizeof(header));
  {
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
  }
  call('A', 0x39, 0x80100000, 0x10000);
  call('C', 0x08, 0x80008000, 0x1000);
  ASSERT_EQ(call('B', 0x32, str("SYSTEM.CNF")), 3u);
  ASSERT_TRUE(load());
  EXPECT_EQ(call('A', 0x33, 16), 0u);
  EXPECT_EQ(call('B', 0x00, 16), 0u);
  EXPECT_EQ(call('B', 0x34, 3, 0x5000, 1), UINT32_MAX);
}

TEST_F(SaveStateTest, LoadInvalidatesPgxpStateFromReplacedTimeline) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  constexpr uint32_t address = 0x1000;
  constexpr uint32_t value = 0x0014000A;
  mem.write32(address, value);
  ctx.r[1] = value;
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  PGXPValue precision;
  precision.x = 10.25f;
  precision.y = 20.5f;
  precision.z = 100.0f;
  precision.value = value;
  precision.flags = PGXPValue::VALID_XY | PGXPValue::VALID_Z;
  mem.storePrecision(address, value, precision);
  ctx.pgxpGpr[1] = precision;
  ctx.pgxpGteData[GTE_SXY2] = precision;
  PGXPValue found;
  ASSERT_TRUE(mem.lookupPrecision(address, value, found));

  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(mem.read32(address), value);
  EXPECT_EQ(ctx.r[1], value);
  EXPECT_FALSE(mem.lookupPrecision(address, value, found));
  EXPECT_EQ(ctx.pgxpGpr[1].flags, 0u);
  EXPECT_EQ(ctx.pgxpGteData[GTE_SXY2].flags, 0u);
}

TEST_F(SaveStateTest, InvalidFileRejected) {
  // Try loading a non-existent file
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  EXPECT_FALSE(SaveState::load("/nonexistent/path.sav", ctx, mem, gpu, spu, dma,
                               cdrom, timers, irq, input));
}

TEST_F(SaveStateTest, TimerContinuationPreservesOneShotFlagsAndFractionalClock) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx{};
  timers.writeRegister(0x1f801108, 4);
  timers.writeRegister(0x1f801104, 0x18); // one-shot target/reset
  timers.writeRegister(0x1f801118, 4);
  timers.writeRegister(0x1f801114, 0xd8); // repeated toggle
  timers.writeRegister(0x1f801124, 0x200); // /8, pending fractional credit
  EXPECT_EQ(timers.tick(7), IRQ_TMR0 | IRQ_TMR1);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  // Saving is observational; a later guest MODE read must still see target.
  EXPECT_NE(timers.readRegister(0x1f801104) & 0x800, 0);
  const auto expectedIrqs = timers.tick(1);
  EXPECT_EQ(expectedIrqs, 0u); // one-shot spent, toggle returns inactive
  EXPECT_EQ(timers.readRegister(0x1f801120), 1);
  timers.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  EXPECT_NE(timers.readRegister(0x1f801104) & 0x800, 0);
  EXPECT_EQ(timers.tick(1), expectedIrqs);
  EXPECT_EQ(timers.readRegister(0x1f801120), 1);
  EXPECT_EQ(timers.tick(4), IRQ_TMR1);
}

TEST_F(SaveStateTest, IsValidChecks) {
  EXPECT_FALSE(SaveState::isValid("/nonexistent.sav"));

  // Create a file with wrong magic
  {
    std::ofstream f(tmpPath, std::ios::binary);
    f << "NOT_A_SAVE_STATE";
  }
  EXPECT_FALSE(SaveState::isValid(tmpPath));
}

TEST_F(SaveStateTest, SavePreservesCOP0) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  ctx.cop0[12] = 0x10000000; // SR
  ctx.cop0[13] = 0x00000400; // Cause

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));
  ctx.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(ctx.cop0[12], 0x10000000u);
  EXPECT_EQ(ctx.cop0[13], 0x00000400u);
}

TEST_F(SaveStateTest, SavePreservesIRQState) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();

  irq.writeIMask(IRQ_VBLANK | IRQ_CDROM);
  irq.raiseInterrupt(IRQ_VBLANK);

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  irq.reset();
  EXPECT_FALSE(irq.hasPendingInterrupt());

  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input));

  EXPECT_EQ(irq.readIMask(), (IRQ_VBLANK | IRQ_CDROM));
  EXPECT_TRUE(irq.readIStat() & IRQ_VBLANK);
}

TEST_F(SaveStateTest, ExtendedStateRestoresStreamAndTimingState) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  mdec::MDEC mdec;
  recomp_context ctx;
  ctx.reset();

  gpu.writeGP1(0x05000000u | 320u | (64u << 10u));
  gpu.writeGP1(0x06000000u | 600u | (3000u << 12u));
  timers.writeRegister(0x1F801104u, 0x0058u);
  timers.writeRegister(0x1F801108u, 1234u);
  timers.writeRegister(0x1F801100u, 777u);

  auto cdState = cdrom.snapshot();
  cdState.state = cdrom::CdromState::ReadingData;
  cdState.currentLba = 4242;
  cdState.readCycleCounter = 98765;
  cdState.waitingForAck = true;
  cdState.sectorReady = true;
  cdState.sectorSize = 2048;
  cdState.sectorBuffer[17] = 0xA5;
  cdState.responseFifo = {0x12, 0x34};
  cdrom.restore(cdState);

  auto spuState = spu.snapshot();
  spuState.voices[3].currentAddr = 0x4560;
  spuState.voices[3].adsrVolume = 0x2345;
  spuState.xaBuffer = {1, -2, 3, -4};
  spuState.xaReadPos = 2;
  spu.restore(spuState);

  mdec.writeCommand(3u << 29);
  for (unsigned i = 0; i < 32; ++i) mdec.writeCommand((i * 17) | ((65535-i*19) << 16));
  const auto expectedScale = mdec.snapshot().scaleTable;
  mdec.writeCommand((1u << 29u) | 4u);
  mdec.writeCommand(0xFE000001u);

  auto cardState = input.getMemoryCard(0).snapshot();
  cardState.data[0x1234] = 0x6D;
  input.getMemoryCard(0).restore(cardState);

  auto &psyqState = psyq::psyq_state();
  psyqState.reset();
  psyqState.vsyncCounter.store(321, std::memory_order_release);
  psyqState.lastVSyncCounter = 320;
  psyqState.cdRemaining = 19;
  psyqState.cdDestPtr = 0x80123400;
  psyqState.gpuSwapCb = 0x80022200;
  psyqState.intrCallback[9] = 0x80085150;
  spu.accrueSamples(735);

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec));

  gpu.reset();
  timers.reset();
  cdrom.reset();
  spu.reset();
  mdec.reset();
  input.getMemoryCard(0).reset();
  psyqState.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec));

  EXPECT_EQ(gpu.guestState().displayVRAMXStart, 320u);
  EXPECT_EQ(timers.readRegister(0x1F801100u), 777u);
  EXPECT_EQ(timers.readRegister(0x1F801108u), 1234u);
  const auto restoredCd = cdrom.snapshot();
  EXPECT_EQ(restoredCd.state, cdrom::CdromState::ReadingData);
  EXPECT_EQ(restoredCd.currentLba, 4242u);
  EXPECT_EQ(restoredCd.sectorBuffer[17], 0xA5u);
  EXPECT_EQ(restoredCd.responseFifo, (std::vector<uint8_t>{0x12, 0x34}));
  const auto restoredSpu = spu.snapshot();
  EXPECT_EQ(restoredSpu.voices[3].currentAddr, 0x4560u);
  EXPECT_EQ(restoredSpu.voices[3].adsrVolume, 0x2345);
  EXPECT_EQ(restoredSpu.xaBuffer, (std::vector<int16_t>{1, -2, 3, -4}));
  EXPECT_EQ(restoredSpu.xaReadPos, 2u);
  EXPECT_TRUE(mdec.isBusy());
  EXPECT_EQ(mdec.snapshot().remainingWords, 3u);
  EXPECT_EQ(mdec.snapshot().scaleTable, expectedScale);
  EXPECT_EQ(input.getMemoryCard(0).snapshot().data[0x1234], 0x6Du);
  EXPECT_EQ(psyqState.vsyncCounter.load(std::memory_order_acquire), 321u);
  EXPECT_TRUE(
      psyqState.blockingVSyncPacing.load(std::memory_order_acquire));
  EXPECT_EQ(psyqState.blockingVSyncTarget.load(std::memory_order_acquire), 0u);
  EXPECT_EQ(psyqState.autonomousClockWaiters.load(std::memory_order_acquire),
            0u);
  EXPECT_EQ(psyqState.cdRemaining, 19u);
  EXPECT_TRUE(psyqState.cdClockDemand.load(std::memory_order_acquire));
  EXPECT_EQ(psyqState.cdDestPtr, 0x80123400u);
  EXPECT_EQ(psyqState.gpuSwapCb, 0x80022200u);
  EXPECT_EQ(psyqState.intrCallback[9], 0x80085150u);
  EXPECT_EQ(spu.snapshot().clockSamplesPending, 735u);
  psyqState.reset();
}

TEST_F(SaveStateTest, VersionFourRestoresBiosCallbackTimeline) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  cdrom::VirtualFs fs;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  mdec::MDEC mdec;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  bios::Bios biosHle(ctx, fs, mem);
  biosHle.setDma(&dma);

  auto dmaState = dma.snapshot();
  dmaState.completionGeneration[DMA::SPU_CH] = 17;
  dma.restore(dmaState);

  auto biosState = biosHle.snapshot();
  biosState.eventSystem.events.push_back(
      {0xF2000002u, 0x0002u, 0x1000u, 0x80012340u, true, false, false});
  biosState.eventSystem.triggeredBits = 1;
  biosState.eventSystem.pendingCallbacks.push_back(
      {0x80012340u, 4, 0, true, false});
  biosState.cdIntPending = 1;
  biosState.cdEventQueue = {2, 3};
  biosState.cdExceptionPending = 4;
  biosState.lastIntrTickFrame = 300;
  biosState.lastDmaCompletion[DMA::SPU_CH] = 17;
  biosState.customExceptionBuffer = 0x80010000u;
  ASSERT_TRUE(biosHle.restore(biosState));

  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec, &biosHle));

  biosHle.rebaseAfterTimelineLoad();
  auto cleared = biosHle.snapshot();
  ASSERT_TRUE(cleared.eventSystem.pendingCallbacks.empty());
  ASSERT_TRUE(cleared.cdEventQueue.empty());

  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                              irq, input, &mdec, &biosHle));
  const auto restored = biosHle.snapshot();
  ASSERT_EQ(restored.eventSystem.events.size(), 1u);
  ASSERT_EQ(restored.eventSystem.pendingCallbacks.size(), 1u);
  EXPECT_EQ(restored.eventSystem.pendingCallbacks[0].handlerPc, 0x80012340u);
  EXPECT_EQ(restored.eventSystem.triggeredBits, 1u);
  EXPECT_EQ(restored.cdIntPending, 1u);
  EXPECT_EQ(restored.cdEventQueue, (std::vector<uint8_t>{2, 3}));
  EXPECT_EQ(restored.cdExceptionPending, 4u);
  EXPECT_EQ(restored.lastIntrTickFrame, 300u);
  EXPECT_EQ(restored.lastDmaCompletion[DMA::SPU_CH], 17u);
  EXPECT_EQ(restored.customExceptionBuffer, 0x80010000u);
}

TEST(SaveStateBiosCompatibilityTest, LegacyTimelineRebaseCannotWrapDmaEpoch) {
  Memory mem;
  cdrom::VirtualFs fs;
  DMA dma;
  recomp_context ctx;
  ctx.reset();
  ctx.mem = &mem;
  bios::Bios biosHle(ctx, fs, mem);
  biosHle.setDma(&dma);

  auto dmaState = dma.snapshot();
  dmaState.completionGeneration[DMA::SPU_CH] = 16;
  dma.restore(dmaState);
  auto stale = biosHle.snapshot();
  stale.lastIntrTickFrame = 4000;
  stale.lastDmaCompletion[DMA::SPU_CH] = 17;
  stale.eventSystem.pendingCallbacks.push_back(
      {0x80012340u, 0, 0, false, false});
  stale.cdEventQueue = {1};
  ASSERT_TRUE(biosHle.restore(stale));

  psyq::psyq_state().vsyncCounter.store(3000, std::memory_order_release);
  biosHle.rebaseAfterTimelineLoad();
  const auto rebased = biosHle.snapshot();
  EXPECT_EQ(rebased.lastIntrTickFrame, 3000u);
  EXPECT_EQ(rebased.lastDmaCompletion[DMA::SPU_CH], 16u);
  EXPECT_TRUE(rebased.eventSystem.pendingCallbacks.empty());
  EXPECT_TRUE(rebased.cdEventQueue.empty());
  psyq::psyq_state().reset();
}

TEST(SaveStateCoordinatorTest, ParksAtResumePcAndContinuesAfterSave) {
  SaveStateCoordinator coordinator;
  recomp_context ctx;
  ctx.reset();
  ASSERT_TRUE(
      coordinator.request(SaveStateCoordinator::Action::Save, "slot.ps1s"));
  std::atomic<bool> returned{false};
  std::thread guest([&] {
    coordinator.checkpoint(ctx, 0x80012348u);
    returned.store(true, std::memory_order_release);
  });
  while (!coordinator.service([](const auto &request) {
    return request.action == SaveStateCoordinator::Action::Save &&
           request.path == "slot.ps1s";
  }))
    std::this_thread::yield();
  guest.join();
  EXPECT_TRUE(returned.load(std::memory_order_acquire));
  EXPECT_EQ(ctx.pc, 0x80012348u);
  EXPECT_FALSE(coordinator.pending());
}

TEST(SaveStateCoordinatorTest, SuccessfulLoadUnwindsOldTimeline) {
  SaveStateCoordinator coordinator;
  recomp_context ctx;
  ctx.reset();
  ASSERT_TRUE(
      coordinator.request(SaveStateCoordinator::Action::Load, "slot.ps1s"));
  std::atomic<bool> reloaded{false};
  std::thread guest([&] {
    try {
      coordinator.checkpoint(ctx, 0x80045678u);
    } catch (const SaveStateTimelineReload &) {
      reloaded.store(true, std::memory_order_release);
    }
  });
  while (!coordinator.service([](const auto &) { return true; }))
    std::this_thread::yield();
  guest.join();
  EXPECT_TRUE(reloaded.load(std::memory_order_acquire));
  EXPECT_EQ(ctx.pc, 0x80045678u);
}

TEST(SaveStateCoordinatorTest, HostPauseCanLoadThenSaveWithoutAdvancingGuest) {
  SaveStateCoordinator coordinator;
  recomp_context ctx; ctx.reset();
  std::atomic<bool> returned{false}, reloaded{false};
  coordinator.setHostPaused(true);
  std::thread guest([&] {
    try { coordinator.checkpoint(ctx, 0x80012348u); }
    catch (const SaveStateTimelineReload &) { reloaded = true; }
    returned = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!coordinator.hostPaused() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  EXPECT_TRUE(coordinator.hostPaused());
  EXPECT_FALSE(returned);
  EXPECT_TRUE(coordinator.request(SaveStateCoordinator::Action::Load, "load.ps1s"));
  EXPECT_TRUE(coordinator.service([&](const auto &) { ctx.pc = 0x80098764; return true; }));
  EXPECT_TRUE(coordinator.hostPaused());
  EXPECT_FALSE(coordinator.pending());
  EXPECT_FALSE(returned);
  EXPECT_TRUE(coordinator.request(SaveStateCoordinator::Action::Save, "save.ps1s"));
  EXPECT_TRUE(coordinator.service([&](const auto &) { EXPECT_EQ(ctx.pc, 0x80098764u); return true; }));
  EXPECT_FALSE(returned);
  coordinator.setHostPaused(false);
  guest.join();
  EXPECT_TRUE(reloaded); // save must not erase the preceding load's unwind
}

TEST(SaveStateCoordinatorTest, CancelReleasesHostPauseWithoutTimelineReload) {
  SaveStateCoordinator coordinator;
  recomp_context ctx; ctx.reset();
  coordinator.setHostPaused(true);
  std::thread guest([&] { EXPECT_NO_THROW(coordinator.checkpoint(ctx, 0x80010000)); });
  coordinator.cancel();
  guest.join();
  EXPECT_FALSE(coordinator.hostPaused());
}

TEST_F(SaveStateTest, CorruptPayloadIsRejectedBeforeChangingLiveState) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx;
  ctx.reset();
  ctx.r[1] = 12;
  mem.write32(0x1000, 34);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  { // A damaged final byte used to go unnoticed by the RAM-only checksum.
    std::fstream file(tmpPath, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(-1, std::ios::end);
    char byte; file.get(byte); byte ^= 1;
    file.seekp(-1, std::ios::end); file.put(byte);
  }
  ctx.r[1] = 56;
  mem.write32(0x1000, 78);
  EXPECT_FALSE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  EXPECT_EQ(ctx.r[1], 56u);
  EXPECT_EQ(mem.read32(0x1000), 78u);
  std::filesystem::resize_file(tmpPath, 100);
  EXPECT_FALSE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  EXPECT_EQ(ctx.r[1], 56u);
  EXPECT_EQ(mem.read32(0x1000), 78u);
}

TEST_F(SaveStateTest, StructurallyInvalidChecksummedPayloadDoesNotReplaceLiveTimeline) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx{};
  ctx.r[1] = 12;
  mem.write32(0x1000, 34);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  std::ifstream source(tmpPath, std::ios::binary);
  const std::vector<uint8_t> valid((std::istreambuf_iterator<char>(source)), {});
  source.close();
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    SCOPED_TRACE(mutation);
    auto bytes = valid;
    const auto xa = bytes.size() - 64 - 4 - 163; // NSV10 decoder/mixer extension
    if (mutation == 0) bytes[xa + 144] = 32; // ring index outside array
    if (mutation == 1) bytes[xa + 145] = 0;  // impossible interpolation phase
    if (mutation == 2) bytes[xa + 145] = 7;
    if (mutation == 3) bytes.erase(bytes.end() - 65); // missing payload byte
    if (mutation == 4) bytes.insert(bytes.end() - 64, 0); // unaccounted payload
    if (mutation == 6) bytes[bytes.size() - 65] = 8; // invalid timer divider
    if (mutation == 7) bytes[bytes.size() - 68] = 2; // invalid one-shot latch
    if (mutation == 5) bytes.resize(16 + 460 + 100); // truncated fixed prefix
    uint32_t checksum = 2166136261u;
    for (size_t i = 16; i < bytes.size(); ++i) checksum = (checksum ^ bytes[i]) * 16777619u;
    std::memcpy(bytes.data() + 12, &checksum, sizeof(checksum));
    {
      std::ofstream file(tmpPath, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    }
    ctx.r[1] = 56;
    ctx.nativeResumePc = 0x80012340;
    mem.write32(0x1000, 78);
    irq.writeIMask(0x155);
    input.press(input::BTN_CROSS);
    EXPECT_FALSE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
    EXPECT_EQ(ctx.r[1], 56u);
    EXPECT_EQ(ctx.nativeResumePc, 0x80012340u);
    EXPECT_EQ(mem.read32(0x1000), 78u);
    EXPECT_EQ(irq.readIMask(), 0x155u);
    EXPECT_EQ(input.buttonState() & input::BTN_CROSS, 0);
  }
}

TEST_F(SaveStateTest, VersionSixMgsSpuSlotMigratesWithoutChangingDiskPrefix) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx{};
  auto &state = psyq::psyq_state();
  state.reset();
  state.intrCallback[0] = 0x80085150; // old runtime misregistration
  state.gpuSwapCb = 0x8008BEC8; // separately registered scheduler survives
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                             irq, input));
  // v7 appends only slots 8-10 and sample credit to the frozen v6 layout.
  std::ifstream source(tmpPath, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(source)), {});
  source.close();
  ASSERT_GT(bytes.size(), sizeof(SaveStateHeader) + 20u);
  bytes.resize(bytes.size() - 20 - 128 - 163 - 4 - 64); // v9 MDEC table and v8 media trailer
  SaveStateHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  header.version = 6;
  header.flags = 0x3656534e;
  header.checksum = 2166136261u;
  for (size_t i = sizeof(header); i < bytes.size(); ++i)
    header.checksum = (header.checksum ^ static_cast<uint8_t>(bytes[i])) * 16777619u;
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::ofstream legacy(tmpPath, std::ios::binary | std::ios::trunc);
  legacy.write(bytes.data(), bytes.size());
  legacy.close();
  ASSERT_TRUE(SaveState::isValid(tmpPath));
  spu.enableGuestClock();
  state.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                             irq, input));
  EXPECT_EQ(state.intrCallback[0], 0u);
  EXPECT_EQ(state.intrCallback[9], 0x80085150u);
  EXPECT_EQ(state.gpuSwapCb, 0x8008BEC8u);
  state.reset();
}

TEST_F(SaveStateTest, VersionTenWithoutTimerExtensionStillLoads) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx{};
  timers.writeRegister(0x1f801108, 4);
  timers.writeRegister(0x1f801104, 0x18);
  timers.writeRegister(0x1f801124, 0x200);
  timers.tick(7);
  mem.write32(0x1000, 0x12345678);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  std::ifstream source(tmpPath, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(source)), {});
  source.close();
  bytes.erase(bytes.end() - 68, bytes.end() - 64);
  SaveStateHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  header.version = 10;
  header.flags = 0x4156534e;
  header.checksum = 2166136261u;
  for (size_t i = sizeof(header); i < bytes.size(); ++i)
    header.checksum = (header.checksum ^ uint8_t(bytes[i])) * 16777619u;
  std::memcpy(bytes.data(), &header, sizeof(header));
  {
    std::ofstream legacy(tmpPath, std::ios::binary | std::ios::trunc);
    legacy.write(bytes.data(), bytes.size());
  }
  ASSERT_TRUE(SaveState::isValid(tmpPath));
  timers.tick(1); // abandoned host phase must not leak into a legacy load
  mem.write32(0x1000, 0);
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers, irq, input));
  EXPECT_EQ(mem.read32(0x1000), 0x12345678u);
  EXPECT_EQ(timers.snapshot().sysclkDiv8Carry, 0);
  EXPECT_EQ(timers.snapshot().timers[0].irqFired, 0); // absent in old format
  EXPECT_EQ(timers.readRegister(0x1f801100), 3);
}

TEST_F(SaveStateTest, VersionEightMigratesOnlyUndeliveredDmaInterrupts) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cdrom;
  cdrom::VirtualFs fs;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  mdec::MDEC decoder;
  recomp_context ctx{};
  ctx.mem = &mem;
  bios::Bios biosHle(ctx, fs, mem);
  biosHle.setDma(&dma);
  auto &psyqState = psyq::psyq_state();
  psyqState.reset();
  psyqState.dmaCallback[1] = 0x800C75BC;
  psyqState.dmaCallback[3] = 0x800DB530;
  auto ds = dma.snapshot();
  ds.dicr = 0x7f000000; // legacy unconditionally latched all channels
  ds.completionGeneration[1] = 20;
  ds.completionGeneration[3] = 8;
  dma.restore(ds);
  auto bs = biosHle.snapshot();
  bs.lastDmaCompletion[1] = 20; // already delivered
  bs.lastDmaCompletion[3] = 7;  // one pending completion
  ASSERT_TRUE(biosHle.restore(bs));
  mem.write32(0x1000, 0x1234abcd);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                             irq, input, &decoder, &biosHle));
  std::ifstream source(tmpPath, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(source)), {});
  source.close();
  // Remove NSV9-11 hardware additions plus the empty 368-byte NSV12 HLE tail.
  bytes.erase(bytes.end()-359-368, bytes.end()-64);
  SaveStateHeader header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  header.version = 8;
  header.flags = 0x3856534e;
  header.checksum = 2166136261u;
  for (size_t i = sizeof(header); i < bytes.size(); ++i)
    header.checksum = (header.checksum ^ static_cast<uint8_t>(bytes[i])) * 16777619u;
  std::memcpy(bytes.data(), &header, sizeof(header));
  std::ofstream legacy(tmpPath, std::ios::binary | std::ios::trunc);
  legacy.write(bytes.data(), bytes.size());
  legacy.close();
  dma.reset();
  psyqState.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cdrom, timers,
                             irq, input, &decoder, &biosHle));
  const auto dicr = dma.readRegister(0x1F8010F4);
  EXPECT_EQ(dicr & 0x7f000000, 1u << 27);
  EXPECT_EQ(dicr & 0x00ff0000, (1u << 23) | (1u << 17) | (1u << 19));
  EXPECT_TRUE(dma.hasInterrupt());
  EXPECT_EQ(decoder.snapshot().scaleTable, mdec::MDEC().snapshot().scaleTable);
  EXPECT_EQ(mem.read32(0x1000), 0x1234abcdu);
  EXPECT_EQ(psyqState.dmaCallback[3], 0x800DB530u);
  psyqState.reset();
}

TEST_F(SaveStateTest, XaDecoderMixerAndQueuedAudioSurviveSaveLoad) {
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  cdrom::CdromController cd;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx{};
  auto savedCd = cd.snapshot();
  savedCd.xaDecoder.history = {300, -800, 2000, -4000};
  for (unsigned c = 0; c < 2; ++c)
    for (unsigned i = 0; i < 32; ++i) savedCd.xaDecoder.ring[c][i] = i * 200 - c * 600;
  savedCd.xaDecoder.position = 19;
  savedCd.xaDecoder.sixstep = 3;
  savedCd.xaCurrentFile = 7;
  savedCd.xaCurrentChannel = 4;
  savedCd.xaCurrentSet = true;
  savedCd.muted = true;
  savedCd.adpcmMuted = true;
  savedCd.audioVolume = {80, 5, 12, 97};
  savedCd.nextAudioVolume = {37, 19, 28, 76};
  cd.restore(savedCd);
  spu.writeRegister(0x1f801db0, 0x4000);
  spu.writeRegister(0x1f801db2, 0xc000);
  const int16_t audio[] = {1000, -1000, 2000, -2000};
  spu.pushXaSamples(audio, 4);
  ASSERT_TRUE(SaveState::save(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  ASSERT_TRUE(SaveState::isValid(tmpPath));
  cd.reset();
  spu.reset();
  ASSERT_TRUE(SaveState::load(tmpPath, ctx, mem, gpu, spu, dma, cd, timers, irq, input));
  const auto actual = cd.snapshot();
  EXPECT_EQ(actual.xaDecoder.history, savedCd.xaDecoder.history);
  EXPECT_EQ(actual.xaDecoder.ring, savedCd.xaDecoder.ring);
  EXPECT_EQ(actual.xaDecoder.position, 19);
  EXPECT_EQ(actual.xaDecoder.sixstep, 3);
  EXPECT_EQ(actual.xaCurrentFile, 7);
  EXPECT_EQ(actual.xaCurrentChannel, 4);
  EXPECT_TRUE(actual.xaCurrentSet);
  EXPECT_TRUE(actual.muted);
  EXPECT_TRUE(actual.adpcmMuted);
  EXPECT_EQ(actual.audioVolume, savedCd.audioVolume);
  EXPECT_EQ(actual.nextAudioVolume, savedCd.nextAudioVolume);
  EXPECT_EQ(spu.readRegister(0x1f801db0), 0x4000);
  EXPECT_EQ(spu.readRegister(0x1f801db2), 0xc000);
  EXPECT_EQ(spu.snapshot().xaBuffer, std::vector<int16_t>(audio, audio+4));
}
