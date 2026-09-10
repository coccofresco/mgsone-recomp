#include "runtime/cdrom/cdrom_controller.h"
#include <gtest/gtest.h>

using namespace ps1::cdrom;

class CdromControllerTest : public ::testing::Test {
protected:
  CdromController cdrom;
  void SetUp() override { cdrom.reset(); }
};

TEST_F(CdromControllerTest, InitialState) {
  EXPECT_EQ(cdrom.getState(), CdromState::Idle);
  EXPECT_FALSE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, GetStatCommand) {
  // Write index 0 to select command registers
  cdrom.writeRegister(0x1F801800, 0x00);
  // Send GetStat (0x01)
  cdrom.writeRegister(0x1F801801, 0x01);
  // Tick to process command
  cdrom.tick(100000);
  // Should have interrupt response
  EXPECT_TRUE(cdrom.hasInterrupt());
  // Read response - should be status byte
  uint8_t stat = cdrom.readRegister(0x1F801801);
  EXPECT_NE(stat, 0); // Motor should be on after GetStat
}

TEST_F(CdromControllerTest, SetLocCommand) {
  cdrom.writeRegister(0x1F801800, 0x00);
  // Set parameters: minute=0, second=2, sector=0 (LBA 0)
  cdrom.writeRegister(0x1F801802, 0x00); // minute BCD
  cdrom.writeRegister(0x1F801802, 0x02); // second BCD
  cdrom.writeRegister(0x1F801802, 0x00); // sector BCD
  // Send SetLoc (0x02)
  cdrom.writeRegister(0x1F801801, 0x02);
  cdrom.tick(100000);
  EXPECT_TRUE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, InitCommand) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801801, 0x0A); // Init
  cdrom.tick(100000);
  EXPECT_EQ(cdrom.getState(), CdromState::Idle);
  EXPECT_TRUE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, StatusRegisterReflectsState) {
  // Read status register (port 0)
  uint8_t status = cdrom.readRegister(0x1F801800);
  // Index bits should be 0
  EXPECT_EQ(status & 3, 0);
}

TEST_F(CdromControllerTest, InterruptAcknowledge) {
  cdrom.writeRegister(0x1F801800, 0x00);
  cdrom.writeRegister(0x1F801801, 0x01); // GetStat
  cdrom.tick(100000);
  EXPECT_TRUE(cdrom.hasInterrupt());
  // Acknowledge interrupt
  cdrom.ackInterrupt(0x1F);
  EXPECT_FALSE(cdrom.hasInterrupt());
}

TEST_F(CdromControllerTest, MsfLbaConversion) {
  // MSF 00:02:00 = LBA 150 (pregap) -> 0 data
  EXPECT_EQ(CdromController::msfToLba(0, 2, 0), 150u);
  EXPECT_EQ(CdromController::msfToLba(0, 0, 0), 0u);
  EXPECT_EQ(CdromController::msfToLba(1, 0, 0), 4500u); // 60*75

  uint8_t m, s, f;
  CdromController::lbaToMsf(150, m, s, f);
  EXPECT_EQ(m, 0);
  EXPECT_EQ(s, 2);
  EXPECT_EQ(f, 0);
}

TEST_F(CdromControllerTest, BcdConversion) {
  EXPECT_EQ(CdromController::toBcd(0), 0x00);
  EXPECT_EQ(CdromController::toBcd(10), 0x10);
  EXPECT_EQ(CdromController::toBcd(59), 0x59);
  EXPECT_EQ(CdromController::fromBcd(0x59), 59);
  EXPECT_EQ(CdromController::fromBcd(0x10), 10);
}
