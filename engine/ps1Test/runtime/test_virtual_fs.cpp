#include "runtime/cdrom/virtual_fs.h"
#include "runtime/save_state.h"
#include "runtime/games/mgs_disc.h"
#include "runtime/games/mgs_profile.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

class VirtualFsTest : public ::testing::Test {
protected:
  void SetUp() override {
    testBinPath = fs::temp_directory_path() / "test_vfs.bin";
    writeImage(testBinPath, "SLUS_123.45");
  }

  static void writeImage(const fs::path &path, const std::string &boot) {
    std::ofstream off(path, std::ios::binary);

    std::vector<uint8_t> dummyData(ps1::cdrom::SECTOR_SIZE_RAW * 20, 0);

    // Standard ISO9660 PVD
    size_t pvdOffset = 16 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    dummyData[pvdOffset + 0] = 1;
    std::memcpy(&dummyData[pvdOffset + 1], "CD001", 5);

    uint32_t rootLba = 17;
    uint32_t rootLen = 2048;
    std::memcpy(&dummyData[pvdOffset + 156 + 2], &rootLba, 4);
    std::memcpy(&dummyData[pvdOffset + 156 + 10], &rootLen, 4);

    // Setup Root Directory at sector 17
    size_t rootOffset = 17 * ps1::cdrom::SECTOR_SIZE_RAW + 24;

    // SYSTEM.CNF record
    std::string sysCnfBody = "BOOT = cdrom:\\" + boot + ";1\r\nTCB = 4\r\n";
    uint8_t rec1_len = 34 + 10;
    dummyData[rootOffset + 0] = rec1_len;
    uint32_t sysCnfLba = 18;
    uint32_t sysCnfLen = sysCnfBody.length();
    std::memcpy(&dummyData[rootOffset + 2], &sysCnfLba, 4);
    std::memcpy(&dummyData[rootOffset + 10], &sysCnfLen, 4);
    dummyData[rootOffset + 25] = 0x00;
    dummyData[rootOffset + 32] = 10;
    std::memcpy(&dummyData[rootOffset + 33], "SYSTEM.CNF", 10);

    // SLUS_123.45 record
    size_t rootOffset2 = rootOffset + rec1_len;
    uint8_t rec2_len = 34 + 11;
    dummyData[rootOffset2 + 0] = rec2_len;
    uint32_t slusLba = 19;
    uint32_t slusLen = 8; // dummy exe length
    std::memcpy(&dummyData[rootOffset2 + 2], &slusLba, 4);
    std::memcpy(&dummyData[rootOffset2 + 10], &slusLen, 4);
    dummyData[rootOffset2 + 25] = 0x00;
    dummyData[rootOffset2 + 32] = 11;
    std::memcpy(&dummyData[rootOffset2 + 33], boot.c_str(), 11);

    // Write SYSTEM.CNF body at sector 18
    size_t sysCnfOffset = 18 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    std::memcpy(&dummyData[sysCnfOffset], sysCnfBody.data(),
                sysCnfBody.length());

    // Write SLUS_123.45 body at sector 19
    size_t slusOffset = 19 * ps1::cdrom::SECTOR_SIZE_RAW + 24;
    std::memcpy(&dummyData[slusOffset], "PS-X EXE", 8);

    off.write(reinterpret_cast<const char *>(dummyData.data()),
              dummyData.size());
  }

  void TearDown() override {
    ps1::games::mgs::configureDiscChange(nullptr, nullptr);
    ps1::games::mgs::configure(false, false);
    fs::remove(secondPath);
    fs::remove(statePath);
    if (fs::exists(testBinPath)) {
      fs::remove(testBinPath);
    }
  }

  fs::path testBinPath;
  fs::path secondPath = fs::temp_directory_path() / "test_vfs_second.bin";
  fs::path statePath = fs::temp_directory_path() / "test_vfs_media.ps1s";
};

TEST_F(VirtualFsTest, LoadAndReadInfo) {
  ps1::cdrom::VirtualFs vfs;
  EXPECT_TRUE(vfs.loadDisc(testBinPath));

  auto bootPath = vfs.getBootPath();
  ASSERT_TRUE(bootPath.has_value());
  EXPECT_EQ(bootPath.value(), "SLUS_123.45");

  auto exeData = vfs.readFile(bootPath.value());
  ASSERT_TRUE(exeData.has_value());
  EXPECT_EQ(exeData->size(), 8);
  EXPECT_EQ(std::string(reinterpret_cast<const char *>(exeData->data()), 8),
            "PS-X EXE");
}

TEST_F(VirtualFsTest, UnpacedDataUsesExactMountedFileExtentsAndExplicitPolicy) {
  ps1::cdrom::VirtualFs vfs;
  vfs.setUnpacedDataFiles({"/SYSTEM.CNF", "/MISSING.DAT"});
  EXPECT_FALSE(vfs.allowsUnpacedData(18));
  ASSERT_TRUE(vfs.loadDisc(testBinPath));
  EXPECT_FALSE(vfs.allowsUnpacedData(17));
  EXPECT_TRUE(vfs.allowsUnpacedData(18)); // Last partial sector is included.
  EXPECT_FALSE(vfs.allowsUnpacedData(19));
  bool consuming = false;
  vfs.setUnpacedDataGuard([&](std::string_view path) {
    return path != "/SYSTEM.CNF" || consuming;
  });
  EXPECT_FALSE(vfs.allowsUnpacedData(18));
  consuming = true;
  EXPECT_TRUE(vfs.allowsUnpacedData(18));
  vfs.setUnpacedDataGuard({});
  vfs.setUnpacedDataFiles({});
  EXPECT_FALSE(vfs.allowsUnpacedData(18));
  vfs.setUnpacedDataFiles({"/SLUS_123.45"});
  EXPECT_TRUE(vfs.allowsUnpacedData(19));
  const auto first = vfs.mediaIdentity();
  writeImage(secondPath, "SLUS_007.76");
  ASSERT_TRUE(vfs.registerDisc(secondPath));
  EXPECT_TRUE(vfs.allowsUnpacedData(19)); // Registration is not a mount.
  EXPECT_FALSE(vfs.swapDisc(secondPath.string() + ".missing"));
  EXPECT_TRUE(vfs.allowsUnpacedData(19));
  ASSERT_TRUE(vfs.swapDisc(secondPath));
  EXPECT_FALSE(vfs.allowsUnpacedData(19)); // Same LBA, different file.
  ASSERT_TRUE(vfs.mountIdentity(first));
  EXPECT_TRUE(vfs.allowsUnpacedData(19));
  ASSERT_TRUE(vfs.mountIdentity(""));
  EXPECT_FALSE(vfs.allowsUnpacedData(19));
  ASSERT_TRUE(vfs.mountIdentity(first));
  EXPECT_TRUE(vfs.allowsUnpacedData(19));
}

TEST_F(VirtualFsTest, RegistrationAndFailedSwapPreserveMountedMedium) {
  ps1::cdrom::VirtualFs fs;
  ASSERT_TRUE(fs.loadDisc(testBinPath));
  const auto first = fs.mediaIdentity();
  EXPECT_EQ(fs.getDiscCount(), 1u);
  writeImage(secondPath, "SLUS_007.76");
  ASSERT_TRUE(fs.registerDisc(secondPath, "SLUS_007.76"));
  EXPECT_EQ(fs.mediaIdentity(), first);
  EXPECT_EQ(fs.getDiscCount(), 2u);
  EXPECT_FALSE(fs.registerDisc(secondPath, "SLUS_005.94"));
  EXPECT_FALSE(fs.swapDisc(secondPath.string() + ".missing"));
  EXPECT_EQ(fs.mediaIdentity(), first);
  ASSERT_TRUE(fs.swapDisc(secondPath));
  EXPECT_EQ(fs.getCurrentDisc(), 1u);
  EXPECT_EQ(fs.getBootPath(), "SLUS_007.76");
  ASSERT_TRUE(fs.mountIdentity(first));
  EXPECT_EQ(fs.getCurrentDisc(), 0u);
  EXPECT_EQ(fs.getDiscCount(), 2u);
  const auto second = fs.identityForBoot("SLUS_007.76");
  writeImage(secondPath, "SLUS_005.94"); // replaced after registration
  EXPECT_FALSE(fs.mountIdentity(second));
  EXPECT_EQ(fs.mediaIdentity(), first);
  EXPECT_EQ(fs.getBootPath(), "SLUS_123.45");
}

TEST_F(VirtualFsTest, LowercaseBootAndRelocatedIdenticalImageAreSupported) {
  // Retail SYSTEM.CNF uses lowercase; ISO directory names are uppercase.
  writeImage(testBinPath, "SLUS_005.94");
  std::fstream file(testBinPath, std::ios::binary | std::ios::in | std::ios::out);
  file.seekp(18 * ps1::cdrom::SECTOR_SIZE_RAW + 24 + 14);
  file.write("slus_005.94", 10);
  file.close();
  ps1::cdrom::VirtualFs fs;
  ASSERT_TRUE(fs.loadDisc(testBinPath));
  const auto id = fs.mediaIdentity();
  EXPECT_EQ(fs.getBootPath(), "SLUS_005.94");
  fs::copy_file(testBinPath, secondPath, fs::copy_options::overwrite_existing);
  ASSERT_TRUE(fs.registerDisc(secondPath));
  fs::remove(testBinPath);
  ASSERT_TRUE(fs.mountIdentity(""));
  ASSERT_TRUE(fs.mountIdentity(id));
  EXPECT_EQ(fs.getDiscCount(), 1u);
  EXPECT_EQ(fs.getBootPath(), "SLUS_005.94");
}

TEST_F(VirtualFsTest, LegacyStatesUseFilesystemDiscNumberBeforeLinkedDiscRefresh) {
  using namespace ps1;
  writeImage(testBinPath, "SLUS_005.94");
  writeImage(secondPath, "SLUS_007.76");
  cdrom::VirtualFs fs;
  cdrom::CdromController drive;
  drive.attachVirtualFs(&fs);
  ASSERT_TRUE(fs.loadDisc(testBinPath));
  ASSERT_TRUE(fs.registerDisc(secondPath));
  games::mgs::configure(true, false);
  games::mgs::configureDiscChange(&fs, &drive);
  std::string bytes(476 + Memory::RAM_SIZE, '\0');
  bytes[476 + 0xB7518] = 1; // GM_Disk has not refreshed yet
  bytes[476 + (games::mgs::UsaFsDiskNum & 0x1fffffff)] = 1; // FS uses CD 2
  std::istringstream second(bytes);
  ASSERT_TRUE(games::mgs::restoreLegacyDisc(second, 7, drive));
  EXPECT_EQ(fs.getBootPath(), "SLUS_007.76");
  bytes[476 + (games::mgs::UsaFsDiskNum & 0x1fffffff)] = 0;
  std::istringstream first(bytes);
  ASSERT_TRUE(games::mgs::restoreLegacyDisc(first, 6, drive));
  EXPECT_EQ(fs.getBootPath(), "SLUS_005.94");
  bytes[476 + (games::mgs::UsaFsDiskNum & 0x1fffffff)] = 3;
  std::istringstream invalid(bytes);
  EXPECT_FALSE(games::mgs::restoreLegacyDisc(invalid, 7, drive));
  EXPECT_EQ(fs.getBootPath(), "SLUS_005.94");
}

TEST_F(VirtualFsTest, AutomaticChangeRetainsOriginalFilesystemValidation) {
  using namespace ps1;
  cdrom::VirtualFs fs;
  cdrom::CdromController drive;
  drive.attachVirtualFs(&fs);
  ASSERT_TRUE(fs.loadDisc(testBinPath));
  writeImage(secondPath, "SLUS_007.76");
  ASSERT_TRUE(fs.registerDisc(secondPath));
  games::mgs::configure(true, false);
  games::mgs::configureDiscChange(&fs, &drive);
  Memory ram;
  recomp_context ctx{};
  ctx.mem = &ram;
  ctx.r4 = 0x80110000;
  ram.write32(ctx.r4 + 8, 0x800C699C);
  ram.write32(ctx.r4 + 0x6b0, 2);
  ram.write32(games::mgs::UsaFsDiskNum, 0);
  ram.write16(0x800B7518, 1);
  EXPECT_FALSE(mgsTryDiscChange(&ctx, 0x800C63A0));
  EXPECT_EQ(ram.read32(ctx.r4 + 0x6b0), 5u);
  EXPECT_EQ(ram.read32(games::mgs::UsaFsDiskNum), 0u);
  EXPECT_EQ(ram.read16(0x800B7518), 1u);
  EXPECT_EQ(fs.getBootPath(), "SLUS_007.76");
  EXPECT_TRUE(drive.snapshot().motorOn);
  EXPECT_FALSE(drive.hasSectorReady());
  // A restored interior continuation must execute its original body.
  ctx.nativeResumePc = 0x800C68A0;
  EXPECT_FALSE(mgsTryDiscChange(&ctx, 0x800C76A0));
  ctx.nativeResumePc = 0;
  EXPECT_TRUE(mgsTryDiscChange(&ctx, 0x800C76A0));
  EXPECT_EQ(ctx.r2, 0u);
  ram.write32(ctx.r4 + 0x6ac, 1);
  // Legacy saves can contain a running logo. Removing the fade must not leave
  // its overlay callback alive while the original filesystem replaces code.
  const uint32_t logo = 0x80120000, prev = 0x80130000, next = 0x80130100;
  ram.write32(ctx.r4 + 0x69c, logo);
  ram.write32(logo, prev);
  ram.write32(logo + 4, next);
  ram.write32(prev + 4, logo);
  ram.write32(next, logo);
  ram.write32(logo + 8, 0x800C7404);
  ram.write32(logo + 12, 0x800C7598);
  ram.write32(logo + 16, 0x8001591c);
  EXPECT_FALSE(mgsTryDiscChange(&ctx, 0x800C699C));
  EXPECT_EQ(ram.read32(ctx.r4 + 0x6a4), 71u);
  EXPECT_EQ(ram.read32(ctx.r4 + 0x69c), 0u);
  EXPECT_EQ(ram.read32(logo + 8), 0x80015350u);
  EXPECT_EQ(ram.read32(logo + 12), 0x800C7598u); // native Die/free retained
  EXPECT_EQ(ram.read32(logo + 16), 0x8001591cu);
  EXPECT_EQ(ram.read32(prev + 4), logo); // unlink is deferred to the scheduler
}

TEST_F(VirtualFsTest, UnavailableDiscDoesNotConsumePendingDataOrChangeActor) {
  using namespace ps1;
  cdrom::VirtualFs fs;
  cdrom::CdromController drive;
  drive.attachVirtualFs(&fs);
  ASSERT_TRUE(fs.loadDisc(testBinPath));
  const auto first = fs.mediaIdentity();
  auto state = drive.snapshot();
  state.sectorReady = true;
  state.sectorBuffer[17] = 91;
  state.currentLba = 123;
  drive.restore(state);
  EXPECT_FALSE(drive.changeMedia("missing"));
  EXPECT_EQ(drive.snapshot().sectorBuffer[17], 91);
  EXPECT_EQ(drive.snapshot().currentLba, 123u);
  EXPECT_TRUE(drive.hasSectorReady());
  games::mgs::configure(true, false);
  games::mgs::configureDiscChange(&fs, &drive);
  Memory ram;
  recomp_context ctx{};
  ctx.mem = &ram;
  ctx.r4 = 0x80110000;
  ram.write32(ctx.r4 + 8, 0x800C699C);
  ram.write32(ctx.r4 + 0x6b0, 2);
  EXPECT_THROW(mgsTryDiscChange(&ctx, 0x800C63A0), std::runtime_error);
  EXPECT_EQ(ram.read32(ctx.r4 + 0x6b0), 2u);
  EXPECT_EQ(fs.mediaIdentity(), first);
}

TEST_F(VirtualFsTest, SavestateRestoresDiscAndBufferedReadInBothDirections) {
  using namespace ps1;
  cdrom::VirtualFs fs;
  cdrom::CdromController drive;
  drive.attachVirtualFs(&fs);
  ASSERT_TRUE(fs.loadDisc(testBinPath));
  const auto first = fs.mediaIdentity();
  writeImage(secondPath, "SLUS_007.76");
  ASSERT_TRUE(fs.registerDisc(secondPath));
  const auto second = fs.identityForBoot("SLUS_007.76");
  Memory mem;
  gpu::GPU gpu;
  spu::SPU spu;
  DMA dma;
  Timers timers;
  InterruptController irq;
  input::InputController input;
  recomp_context ctx{};
  for (const auto &id : {first, second}) {
    ASSERT_TRUE(drive.changeMedia(id));
    auto buffered = drive.snapshot();
    buffered.sectorReady = true;
    buffered.currentLba = 1234;
    buffered.dataReadOffset = 48;
    buffered.sectorBuffer[24 + 48] = 0x5a;
    drive.restore(buffered);
    mem.write32(0x1000, 0xcafebabe);
    ASSERT_TRUE(SaveState::save(statePath, ctx, mem, gpu, spu, dma, drive, timers, irq, input));
    ASSERT_TRUE(SaveState::isValid(statePath));
    ASSERT_TRUE(drive.changeMedia(id == first ? second : first));
    mem.write32(0x1000, 0);
    ASSERT_TRUE(SaveState::load(statePath, ctx, mem, gpu, spu, dma, drive, timers, irq, input));
    EXPECT_EQ(fs.mediaIdentity(), id);
    EXPECT_EQ(mem.read32(0x1000), 0xcafebabeu);
    EXPECT_EQ(drive.snapshot().currentLba, 1234u);
    EXPECT_EQ(drive.readDataByte(), 0x5a);

    // A structurally invalid file can still have a correct payload checksum.
    // Reject it before switching to its recorded medium in either direction.
    std::ifstream saved(statePath, std::ios::binary);
    const std::vector<uint8_t> original((std::istreambuf_iterator<char>(saved)), {});
    saved.close();
    auto invalid = original;
    invalid[invalid.size() - 64 - 4 - 163 + 144] = 32; // invalid XA ring position
    uint32_t checksum = 2166136261u;
    for (size_t i = 16; i < invalid.size(); ++i) checksum = (checksum ^ invalid[i]) * 16777619u;
    std::memcpy(invalid.data() + 12, &checksum, sizeof(checksum));
    {
      std::ofstream file(statePath, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char *>(invalid.data()), invalid.size());
    }
    const auto running = id == first ? second : first;
    ASSERT_TRUE(drive.changeMedia(running));
    mem.write32(0x1000, 0x12345678);
    const auto liveDrive = drive.snapshot();
    EXPECT_FALSE(SaveState::load(statePath, ctx, mem, gpu, spu, dma, drive, timers, irq, input));
    EXPECT_EQ(fs.mediaIdentity(), running);
    EXPECT_EQ(mem.read32(0x1000), 0x12345678u);
    EXPECT_EQ(drive.snapshot().currentLba, liveDrive.currentLba);
    EXPECT_EQ(drive.snapshot().dataReadOffset, liveDrive.dataReadOffset);
    {
      std::ofstream file(statePath, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char *>(original.data()), original.size());
    }
  }
  // Saved disc 2 is no longer available. Rejection precedes any RAM/device mutation.
  ASSERT_TRUE(drive.changeMedia(first));
  fs::remove(secondPath);
  mem.write32(0x1000, 0x12345678);
  EXPECT_FALSE(SaveState::load(statePath, ctx, mem, gpu, spu, dma, drive, timers, irq, input));
  EXPECT_EQ(mem.read32(0x1000), 0x12345678u);
  EXPECT_EQ(fs.mediaIdentity(), first);
}
