#include "runtime/memory.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/dma/dma.h"
#include "runtime/input/input.h"
#include "runtime/mdec/mdec.h"
#include "runtime/timers/timers.h"
#include <gtest/gtest.h>

namespace {
using namespace ps1;
constexpr uint32_t mirrors[] = {0, 0x80000000, 0xa0000000};
uint32_t read(Memory &m, uint32_t a, unsigned bytes) {
  return bytes == 1 ? m.read8(a) : bytes == 2 ? m.read16(a) : m.read32(a);
}
void write(Memory &m, uint32_t a, uint32_t v, unsigned bytes) {
  if (bytes == 1) m.write8(a, v);
  else if (bytes == 2) m.write16(a, v);
  else m.write32(a, v);
}

TEST(MemoryMMIO, TimerCounterAndTargetReadsAcrossWidthsLanesAndMirrors) {
  Memory m; Timers timers; m.setTimers(&timers);
  for (auto mirror : mirrors) for (unsigned timer = 0; timer < 3; ++timer)
    for (unsigned reg : {0u, 8u}) {
      uint32_t a = 0x1f801100 + timer * 16 + reg;
      timers.writeRegister(a, 0xb39d);
      for (unsigned width : {1u, 2u, 4u})
        for (unsigned lane = 0; lane < 4; lane += width) {
          SCOPED_TRACE(::testing::Message() << std::hex << (a | mirror)
                       << " width=" << width << " lane=" << lane);
          uint32_t mask = width == 4 ? ~0u : (1u << (width * 8)) - 1;
          EXPECT_EQ(read(m, (a | mirror) + lane, width),
                    (0xb39du >> (lane * 8)) & mask);
        }
    }
}

TEST(MemoryMMIO, TimerWordWritesReachAllThreeCountersWithoutTouchingNeighbours) {
  Memory m; Timers timers; m.setTimers(&timers);
  for (auto mirror : mirrors) for (unsigned timer = 0; timer < 3; ++timer)
    for (unsigned width : {2u, 4u}) {
      timers.reset();
      const uint32_t a = (0x1f801100 + timer * 16) | mirror;
      write(m, a + 8, 0x7654abcd, width);
      write(m, a, 0x12349753, width);
      EXPECT_EQ(timers.readRegister(a & 0x1fffffff), 0x9753);
      EXPECT_EQ(timers.readRegister((a + 8) & 0x1fffffff), 0xabcd);
      write(m, a + 4, 0xffff0058, width); // repeated target IRQ; reset counter
      EXPECT_EQ(timers.readRegister(a & 0x1fffffff), 0);
      EXPECT_EQ(timers.readRegister((a + 4) & 0x1fffffff), 0x458);
      EXPECT_EQ(timers.readRegister((a + 8) & 0x1fffffff), 0xabcd);
      EXPECT_EQ(timers.readRegister(0x1f801100 + ((timer + 1) % 3) * 16), 0);
    }
}

TEST(MemoryMMIO, TimerModeReadConsumesFlagsOnceRegardlessOfWidthOrLane) {
  Memory m; Timers timers; m.setTimers(&timers);
  for (auto mirror : mirrors) for (unsigned timer = 0; timer < 3; ++timer)
    for (unsigned width : {1u, 2u, 4u})
      for (unsigned lane = 0; lane < 4; lane += width) {
        timers.reset();
        const uint32_t a = 0x1f801100 + timer * 16;
        timers.writeRegister(a + 8, 1);
        timers.tick(65536);
        ASSERT_EQ(timers.snapshot().timers[timer].mode & 0x1800, 0x1800);
        const uint32_t mask = width == 4 ? ~0u : (1u << (width * 8)) - 1;
        EXPECT_EQ(read(m, (a | mirror) + 4 + lane, width),
                  (0x1c00u >> (lane * 8)) & mask);
        EXPECT_EQ(timers.snapshot().timers[timer].mode & 0x1800, 0);
      }
}

TEST(MemoryMMIO, DmaReadsAcrossWidthsLanesChannelsAndMirrors) {
  Memory m; DMA dma; m.setDMA(&dma);
  for (auto mirror : mirrors) for (unsigned channel = 0; channel < 7; ++channel) {
    const uint32_t a = 0x1f801084 + channel * 16;
    dma.writeRegister(a, 0x7654abcd);
    for (unsigned width : {1u, 2u, 4u})
      for (unsigned lane = 0; lane < 4; lane += width) {
        const uint32_t mask = width == 4 ? ~0u : (1u << (width * 8)) - 1;
        EXPECT_EQ(read(m, (a | mirror) + lane, width),
                  (0x7654abcdu >> (lane * 8)) & mask);
      }
  }
}

TEST(MemoryMMIO, DmaHalfwordCancelCannotRunIntermediateCommand) {
  Memory m; DMA dma; mdec::MDEC decoder;
  m.setDMA(&dma); dma.setMemory(&m); dma.setMDEC(&decoder);
  dma.writeRegister(0x1f8010f0, 0x88);
  dma.writeRegister(0x1f801090, 0x2000);
  dma.writeRegister(0x1f801094, 0x00010080);
  dma.writeRegister(0x1f801098, 0x01000200); // wait for MDEC output
  decoder.writeCommand(0x38000006);
  for (unsigned i = 0; i < 6; ++i) decoder.writeCommand(0xfe000400);
  ASSERT_FALSE(decoder.dataOutEmpty());
  m.write32(0x2000, 0xdeadbeef);
  m.write16(0xbf80109a, 0); // cancel busy in ONE transaction
  EXPECT_EQ(dma.completionGeneration(DMA::MDEC_OUT), 0);
  EXPECT_EQ(m.read32(0x2000), 0xdeadbeef);
  EXPECT_FALSE(decoder.dataOutEmpty());
  EXPECT_EQ(dma.readRegister(0x1f801098), 0x200);
}

TEST(MemoryMMIO, CdReadsIssueOneOrderedByteTransactionPerLane) {
  Memory m; cdrom::CdromController cd; m.setCDROM(&cd);
  for (auto mirror : mirrors) for (unsigned index = 0; index < 4; ++index)
    for (unsigned width : {1u, 2u, 4u})
      for (unsigned lane = 0; lane < 4; lane += width) {
        cd.reset(); auto state = cd.snapshot();
        state.indexReg = index; state.responseFifo = {0x5a, 0x93};
        state.interruptEnable = 0x13; state.interruptFlag = 3;
        state.sectorSize = 2048; state.sectorReady = true;
        state.sectorBuffer[24] = 0xab; state.sectorBuffer[25] = 0xcd;
        cd.restore(state);
        // Independent device path: same ordered bus cycles as SwanStation.
        cdrom::CdromController reference; reference.restore(state);
        uint32_t expected = 0;
        for (unsigned i = 0; i < width; ++i)
          expected |= uint32_t(reference.readRegister(0x1f801800 + lane + i)) << (8 * i);
        EXPECT_EQ(read(m, (0x1f801800 | mirror) + lane, width), expected);
        EXPECT_EQ(cd.snapshot().responseFifo, reference.snapshot().responseFifo);
        EXPECT_EQ(cd.snapshot().dataReadOffset, reference.snapshot().dataReadOffset);
      }
}

TEST(MemoryMMIO, CdWideWritesSelectIndexBeforeApplyingAudioOrInterruptRegisters) {
  Memory m; cdrom::CdromController cd; m.setCDROM(&cd);
  for (auto mirror : mirrors) {
    cd.reset();
    m.write32(0x1f801800 | mirror, 0x22110002); // index 2; stage LL/LR
    EXPECT_EQ(cd.snapshot().nextAudioVolume[0], 0x11);
    EXPECT_EQ(cd.snapshot().nextAudioVolume[1], 0x22);
    m.write32(0x1f801800 | mirror, 0x20443303); // index 3; RR/RL; apply
    const std::array<uint8_t,4> expected{0x11, 0x22, 0x44, 0x33};
    EXPECT_EQ(cd.snapshot().audioVolume, expected);
    m.write8(0x1f801800 | mirror, 1);
    m.write16(0x1f801802 | mirror, 0x0015);
    EXPECT_EQ(cd.snapshot().interruptEnable, 0x15);
  }
}

TEST(MemoryMMIO, PadRegistersAccessibleAtAlignedWidthsAndByteReadLanes) {
  Memory m; input::InputController input; m.setInput(&input);
  for (auto mirror : mirrors) for (unsigned width : {2u, 4u})
    for (unsigned reg : {8u, 10u, 14u}) {
      if (width == 4 && reg % 4 != 0) continue; // MIPS SW/LW require alignment
      input.reset();
      const uint32_t a = 0x1f801040 + reg;
      const uint16_t value = reg == 10 ? 0x2003 : 0x1234;
      write(m, a | mirror, value, width);
      if (reg % 4 == 0) EXPECT_EQ(m.read32(a | mirror), value);
      EXPECT_EQ(m.read16(a | mirror), value);
      EXPECT_EQ(m.read8(a | mirror), uint8_t(value));
      EXPECT_EQ(m.read8((a | mirror) + 1), uint8_t(value >> 8));
    }
}

TEST(MemoryMMIO, DigitalAndAnalogPadPacketsAcrossAccessWidthsPortsAndMirrors) {
  Memory m; input::InputController input; m.setInput(&input);
  for (auto mirror : mirrors) for (unsigned width : {1u, 2u, 4u})
    for (unsigned port = 0; port < 2; ++port)
      for (auto type : {input::PadType::Digital, input::PadType::Analog}) {
        input.reset(); input.setPadType(port, type);
        input.press(input::BTN_CROSS | input::BTN_START, port);
        input.setAnalog(port, 0x12, 0x34, 0x56, 0x78);
        m.write16(0x1f80104a | mirror, (port << 13) | 3);
        const uint8_t tx[] = {1, 0x42, 0, 0, 0, 0, 0, 0, 0};
        const uint8_t rx[] = {0xff, uint8_t(type), 0x5a, 0xf7, 0xbf,
                              0x56, 0x78, 0x12, 0x34};
        const unsigned count = type == input::PadType::Digital ? 5 : 9;
        for (unsigned i = 0; i < count; ++i) {
          write(m, 0x1f801040 | mirror, tx[i], width);
          EXPECT_EQ(read(m, 0x1f801040 | mirror, width) & 0xff, rx[i]);
        }
      }
}

TEST(MemoryMMIO, SerialPadSnapshotResumesAtEveryDigitalAndAnalogPacketBoundary) {
  Memory m;
  for (unsigned port = 0; port < 2; ++port)
    for (auto type : {input::PadType::Digital, input::PadType::Analog}) {
      const uint8_t tx[] = {1, 0x42, 0, 0, 0, 0, 0, 0, 0};
      const uint8_t rx[] = {0xff, uint8_t(type), 0x5a, 0xf7, 0xbf,
                            0x56, 0x78, 0x12, 0x34};
      const unsigned count = type == input::PadType::Digital ? 5 : 9;
      for (unsigned cut = 0; cut <= count; ++cut) {
        input::InputController original;
        original.setPadType(port, type);
        original.press(input::BTN_CROSS | input::BTN_START, port);
        original.setAnalog(port, 0x12, 0x34, 0x56, 0x78);
        original.setFrameSampling(true);
        m.setInput(&original);
        m.write16(0xbf80104a, (port << 13) | 3);
        for (unsigned i = 0; i < cut; ++i) m.write8(0xbf801040, tx[i]);
        input::InputController resumed;
        resumed.setFrameSampling(true);
        ASSERT_TRUE(resumed.restore(original.snapshot()));
        m.setInput(&resumed);
        for (unsigned i = cut; i < count; ++i) {
          m.write8(0xbf801040, tx[i]);
          EXPECT_EQ(m.read8(0xbf801040), rx[i]);
        }
        // A new packet must start cleanly, including restore after its last byte.
        m.write8(0xbf801040, 1);
        EXPECT_EQ(m.read8(0xbf801040), 0xff);
        m.write8(0xbf801040, 0x42);
        EXPECT_EQ(m.read8(0xbf801040), uint8_t(type));
      }
    }
}
} // namespace
