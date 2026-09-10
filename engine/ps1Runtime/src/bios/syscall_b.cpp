#include "bios_internal.h"
#include "runtime/bios/bios.h"
#include "runtime/input/input.h"
#include <fmt/format.h>

namespace ps1::bios {

using detail::readString;

// PS1 BIOS Table B (0xB0) -- event system, pad init, file I/O, memory card,
// table accessors, exception bookkeeping. Dispatched from Bios::executeB0()
// in bios.cpp.

void Bios::handleB0(uint32_t index) {
  switch (index) {
  case 0x00: { // alloc_kernel_memory, aligned guest kernel arena
    const uint32_t size = (ctx_.r[A0] + 7u) & ~7u;
    if (size < ctx_.r[A0] || kernelHeapNext_ > kernelHeapEnd_ ||
        size > kernelHeapEnd_ - kernelHeapNext_) ctx_.r[V0] = 0;
    else {ctx_.r[V0]=kernelHeapNext_;kernelHeapNext_+=size;}
    break;
  }
  case 0x0E:
    ctx_.r[V0]=kernelThreads_.open(ctx_.r[A0],ctx_.r[A1],ctx_.r[A2]);
    break;
  case 0x0F:
    ctx_.r[V0]=kernelThreads_.close(ctx_.r[A0]);break;
  case 0x10:
    kernelThreads_.change(ctx_.r[A0]);break;
  case 0x07: // DeliverEvent
    eventSystem_.triggerEvent(ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x08: // openEvent
    ctx_.r[V0] =
        eventSystem_.openEvent(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2], ctx_.r[A3]);
    break;
  case 0x09: // closeEvent
    ctx_.r[V0] = eventSystem_.closeEvent(ctx_.r[A0]);
    break;
  case 0x0A: // waitEvent
    // Drain pending callbacks (game-thread yield point)
    drainPendingCallbacks();
    ctx_.r[V0] = eventSystem_.waitEvent(ctx_.r[A0]);
    break;
  case 0x0B: { // testEvent
    // Drain pending callbacks (game-thread yield point)
    drainPendingCallbacks();
    static int testEventCallCount = 0;
    ctx_.r[V0] = eventSystem_.testEvent(ctx_.r[A0]);
    testEventCallCount++;
    if (testEventCallCount <= 50 || ctx_.r[V0] != 0) {
      BIOS_LOG("[BIOS] B0:0B testEvent({}) -> {} (call #{})\n", ctx_.r[A0],
                 ctx_.r[V0], testEventCallCount);
    }
    break;
  }
  case 0x0C: // enableEvent
    ctx_.r[V0] = eventSystem_.enableEvent(ctx_.r[A0]);
    break;
  case 0x0D: // disableEvent
    ctx_.r[V0] = eventSystem_.disableEvent(ctx_.r[A0]);
    break;
  case 0x12: { // InitPAD -- store pad buffer addresses for VBlank polling
    padBuf1Addr_ = ctx_.r[A0];
    padBuf1Size_ = ctx_.r[A1];
    padBuf2Addr_ = ctx_.r[A2];
    padBuf2Size_ = ctx_.r[A3];
    padActive_ = false; // Needs StartPAD to activate
    BIOS_LOG("[BIOS] InitPAD(buf1: 0x{:08X}, sz1: {}, buf2: 0x{:08X}, sz2: "
               "{})\n",
               padBuf1Addr_, padBuf1Size_, padBuf2Addr_, padBuf2Size_);

    // Pre-fill pad buffers immediately so the game doesn't read uninitialized
    // zeros (which active-low encoding interprets as "all buttons pressed").
    // Write: status=OK(0x00), type=Digital(0x41), no-buttons(0xFF,0xFF).
    auto initBuf = [&](uint32_t addr, uint32_t sz) {
      if (addr == 0 || sz < 4) return;
      ctx_.mem->write8(addr + 0, 0x00); // status OK
      ctx_.mem->write8(addr + 1, 0x41); // Digital pad
      ctx_.mem->write8(addr + 2, 0xFF); // no buttons pressed (active-low)
      ctx_.mem->write8(addr + 3, 0xFF); // no buttons pressed
    };
    initBuf(padBuf1Addr_, padBuf1Size_);
    initBuf(padBuf2Addr_, padBuf2Size_);
    break;
  }
  case 0x13: // StartPAD -- begin controller polling during VBlank
    padActive_ = true;
    BIOS_LOG("[BIOS] StartPAD() -- polling active\n");
    break;
  case 0x14: // StopPAD -- stop controller polling
    padActive_ = false;
    BIOS_LOG("[BIOS] StopPAD()\n");
    break;
  case 0x17: { // ReturnFromException
    // Restore PC from EPC (COP0 reg 14) and shift Status Register
    ctx_.pc = ctx_.cop0[COP0_EPC];
    // Shift SR exception stack: bits 5:2 -> bits 3:0
    uint32_t sr = ctx_.cop0[COP0_SR];
    ctx_.cop0[COP0_SR] = (sr & ~0xF) | ((sr >> 2) & 0xF);
    break;
  }
  case 0x18: // SetDefaultExitFromException
    BIOS_LOG("[BIOS] SetDefaultExitFromException() [STUB]\n");
    break;
  case 0x19: { // SetCustomExitFromException
    uint32_t buf = ctx_.r[A0];
    BIOS_LOG("[BIOS] SetCustomExitFromException(handler: 0x{:08X})\n", buf);
    if (buf == 0) {
      customExceptionBuffer_ = 0;
      customExceptionCallback_ = nullptr;
      customExceptionRegistered_.store(false, std::memory_order_release);
    } else {
      customExceptionBuffer_ = buf;
      customExceptionCallback_ = [this, buf]() {
        hle_longjmp_emulator(ctx_, *ctx_.mem, buf);
      };
      customExceptionRegistered_.store(true, std::memory_order_release);
    }
    break;
  }
  case 0x20: // UnDeliverEvent
    BIOS_LOG("[BIOS] UnDeliverEvent(class: 0x{:X}, spec: 0x{:X}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x32: // open
  {
    std::string path = readString(*ctx_.mem, ctx_.r[A0]);
    ctx_.r[V0] = fileIo_.open(path.c_str(), ctx_.r[A1]);
  } break;
  case 0x33: // lseek
    ctx_.r[V0] = fileIo_.lseek(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x34: // read
    ctx_.r[V0] = fileIo_.read(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  case 0x35: // write -- stub (stdout)
    BIOS_LOG("[BIOS] write(fd: {}, src: 0x{:08X}, len: {}) [STUB]\n",
               ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    if (ctx_.r[A0] == 1) { // stdout
      for (uint32_t i = 0; i < ctx_.r[A2]; i++) {
        char c = ctx_.mem->read8(ctx_.r[A1] + i);
        fmt::print("{}", c);
      }
    }
    ctx_.r[V0] = ctx_.r[A2];
    break;
  case 0x36: // close
    ctx_.r[V0] = fileIo_.close(ctx_.r[A0]);
    break;
  case 0x3D: // std_out_putchar
    fmt::print("{}", static_cast<char>(ctx_.r[A0]));
    break;
  case 0x3E: // std_out_puts
  {
    std::string s = readString(*ctx_.mem, ctx_.r[A0]);
    fmt::print("{}\n", s);
  } break;
  case 0x3F: // printf
    stub_printf();
    break;
  case 0x47: // AddDevice
    BIOS_LOG("[BIOS] AddDevice(device_info: 0x{:08X}) [STUB]\n", ctx_.r[A0]);
    ctx_.r[V0] = 1; // Success
    break;
  case 0x48: // RemoveDevice
    BIOS_LOG("[BIOS] RemoveDevice(device_info: 0x{:08X}) [STUB]\n",
               ctx_.r[A0]);
    ctx_.r[V0] = 1;
    break;
  case 0x4A: // InitCARD
    BIOS_LOG("[BIOS] InitCARD(pad_enable: {})\n", ctx_.r[A0]);
    break;
  case 0x4B: // StartCARD
    BIOS_LOG("[BIOS] StartCARD()\n");
    break;
  case 0x4C: // StopCARD
    BIOS_LOG("[BIOS] StopCARD()\n");
    break;
  case 0x50: // _new_card: acknowledge insertion for the next transfer
    ctx_.r[V0] = 1;
    break;
  case 0x4E: // _card_write
  case 0x4F: { // _card_read
    const uint32_t slot = ctx_.r[A0] >> 4;
    const uint32_t sector = ctx_.r[A1];
    if (slot >= 2 || sector >= input::MemoryCard::CARD_SIZE / input::MemoryCard::SECTOR_SIZE) {
      ctx_.r[V0] = 0;
      break;
    }
    ctx_.r[V0] = 1;
    if (!input_ || !input_->getMemoryCard(slot).isPresent()) {
      eventSystem_.triggerEvent(0xF0000011u, 0x0100u);
      break;
    }
    auto& card = input_->getMemoryCard(slot);
    auto state = card.snapshot();
    for (uint32_t i = 0; i < input::MemoryCard::SECTOR_SIZE; ++i) {
      const uint32_t offset = sector * input::MemoryCard::SECTOR_SIZE + i;
      if (index == 0x4E) state.data[offset] = ctx_.mem->read8(ctx_.r[A2] + i);
      else ctx_.mem->write8(ctx_.r[A2] + i, state.data[offset]);
    }
    if (index == 0x4E) card.restore(state);
    // Event callbacks run later at a cooperative boundary, after submission.
    eventSystem_.triggerEvent(0xF0000011u, 0x0004u);
    break;
  }
  case 0x56: // GetC0Table -- return pointer to C0 jump table
    BIOS_LOG("[BIOS] GetC0Table() -> 0x{:08X}\n", 0x801FFF80u);
    ctx_.r[V0] = 0x801FFF80u;
    break;
  case 0x57: // GetB0Table -- return pointer to B0 jump table
    BIOS_LOG("[BIOS] GetB0Table() -> 0x{:08X}\n", 0x801FFE00u);
    ctx_.r[V0] = 0x801FFE00u;
    break;
  case 0x42: // B0:42 -- internal PsyQ CdInit verification (SetConf/cdioctl-like)
    // Called by CdInit after 2 rounds of _96_CdInitSubFunc+testEvent+GetStat.
    // Return value: 1 = success (CdInit succeeds), 0 = failure (enters 5-retry
    // hardware loop then prints "CdInit: Init failed").
    // Returning 1 here skips the retry loop and lets CdInit succeed.
    BIOS_LOG("[BIOS] B0:42 [STUB] (a0=0x{:08X}, a1=0x{:08X}, a2={})\n",
               ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    ctx_.r[V0] = 1; // success -- skip retry loop
    break;
  case 0x5B: // ChangeClearPad
    BIOS_LOG("[BIOS] ChangeClearPad({}) [STUB]\n", ctx_.r[A0]);
    break;
  default:
    BIOS_LOG("[BIOS] Unimplemented B0 call: 0x{:02X} (A0: {:08X}, A1: "
               "{:08X}, A2: {:08X})\n",
               index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

} // namespace ps1::bios
