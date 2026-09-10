#include "bios_internal.h"
#include "runtime/bios/bios.h"
#include "runtime/cdrom/cdrom_controller.h"
#include "runtime/gpu/gpu.h"
#include <cctype>
#include <cstdlib>

namespace ps1::bios {

using detail::readString;

// PS1 BIOS Table A (0xA0) -- string/memory/heap helpers, printf, GPU stubs,
// CD-ROM low-level stubs. Dispatched from Bios::executeA0() in bios.cpp.

namespace {
uint32_t sRandSeed = 1;
}

void Bios::handleA0(uint32_t index) {
  switch (index) {
  // String functions
  case 0x0A: { // bzero
    uint32_t dst = ctx_.r[A0];
    uint32_t n = ctx_.r[A1];
    for (uint32_t i = 0; i < n; i++)
      ctx_.mem->write8(dst + i, 0);
    break;
  }
  case 0x0B: { // bcmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    int res = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (ctx_.mem->read8(s1 + i) != ctx_.mem->read8(s2 + i)) {
        res = 1;
        break;
      }
    }
    ctx_.r[V0] = res;
    break;
  }
  case 0x10: // abs
    ctx_.r[V0] =
        static_cast<uint32_t>(std::abs(static_cast<int32_t>(ctx_.r[A0])));
    break;
  case 0x11: // labs
    ctx_.r[V0] =
        static_cast<uint32_t>(std::abs(static_cast<int32_t>(ctx_.r[A0])));
    break;
  case 0x13: { // setjmp
    uint32_t buf = ctx_.r[A0];
    BIOS_LOG("[BIOS] setjmp(buf: 0x{:08X})\n", buf);
    // PsyQ jmp_buf is an array of 12 integers (48 bytes):
    // [0]=RA [1]=SP [2]=FP [3]=S0 [4]=S1 [5]=S2 [6]=S3 [7]=S4 [8]=S5 [9]=S6
    // [10]=S7 [11]=GP
    ctx_.mem->write32(buf + 0, ctx_.r[31]); // RA
    ctx_.mem->write32(buf + 4, ctx_.r[29]); // SP
    ctx_.mem->write32(buf + 8, ctx_.r[30]); // FP
    for (int i = 0; i < 8; i++) {
      ctx_.mem->write32(buf + 12 + (i * 4), ctx_.r[16 + i]); // S0-S7
    }
    ctx_.mem->write32(buf + 44, ctx_.r[28]); // GP
    ctx_.r[V0] = 0;
    break;
  }
  case 0x14: { // longjmp
    uint32_t buf = ctx_.r[A0];
    uint32_t val = ctx_.r[A1];
    BIOS_LOG("[BIOS] longjmp(buf: 0x{:08X}, val: {})\n", buf, val);

    ctx_.r[31] = ctx_.mem->read32(buf + 0); // RA
    ctx_.r[29] = ctx_.mem->read32(buf + 4); // SP
    ctx_.r[30] = ctx_.mem->read32(buf + 8); // FP
    for (int i = 0; i < 8; i++) {
      ctx_.r[16 + i] = ctx_.mem->read32(buf + 12 + (i * 4)); // S0-S7
    }
    ctx_.r[28] = ctx_.mem->read32(buf + 44); // GP

    ctx_.pc = ctx_.r[31]; // Jump to RA
    ctx_.r[V0] = val ? val : 1;
    break;
  }
  case 0x15: { // strcat(dst, src)
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    while (ctx_.mem->read8(dst) != '\0')
      dst++;
    char c;
    do {
      c = ctx_.mem->read8(src++);
      ctx_.mem->write8(dst++, c);
    } while (c != '\0');
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x16: { // strncat(dst, src, n)
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    while (ctx_.mem->read8(dst) != '\0')
      dst++;
    uint32_t i = 0;
    char c;
    while (i < n && (c = ctx_.mem->read8(src++)) != '\0') {
      ctx_.mem->write8(dst++, c);
      i++;
    }
    ctx_.mem->write8(dst, '\0');
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x17: { // strcmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    char c1, c2;
    do {
      c1 = ctx_.mem->read8(s1++);
      c2 = ctx_.mem->read8(s2++);
      if (c1 != c2)
        break;
    } while (c1 != '\0');
    ctx_.r[V0] = c1 - c2;
    break;
  }
  case 0x18: { // strncmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    if (n == 0) {
      ctx_.r[V0] = 0;
      break;
    }
    char c1, c2;
    do {
      c1 = ctx_.mem->read8(s1++);
      c2 = ctx_.mem->read8(s2++);
      if (c1 != c2 || --n == 0)
        break;
    } while (c1 != '\0');
    ctx_.r[V0] = c1 - c2;
    break;
  }
  case 0x19: { // strcpy
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    char c;
    do {
      c = ctx_.mem->read8(src++);
      ctx_.mem->write8(dst++, c);
    } while (c != '\0');
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x1A: { // strncpy
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    uint32_t i = 0;
    char c;
    while (i < n && (c = ctx_.mem->read8(src++)) != '\0') {
      ctx_.mem->write8(dst++, c);
      i++;
    }
    while (i < n) {
      ctx_.mem->write8(dst++, '\0');
      i++;
    }
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x1B: { // strlen
    uint32_t s = ctx_.r[A0];
    uint32_t len = 0;
    while (ctx_.mem->read8(s++) != '\0')
      len++;
    ctx_.r[V0] = len;
    break;
  }
  case 0x1C:   // index (BSD alias for strchr)
  case 0x1E: { // strchr
    uint32_t s = ctx_.r[A0];
    char c = static_cast<char>(ctx_.r[A1]);
    char curr;
    uint32_t res = 0;
    while ((curr = ctx_.mem->read8(s)) != '\0') {
      if (curr == c) {
        res = s;
        break;
      }
      s++;
    }
    if (c == '\0' && curr == '\0')
      res = s;
    ctx_.r[V0] = res;
    break;
  }
  case 0x1D:   // rindex (BSD alias for strrchr)
  case 0x1F: { // strrchr
    uint32_t s = ctx_.r[A0];
    char c = static_cast<char>(ctx_.r[A1]);
    char curr;
    uint32_t res = 0;
    while ((curr = ctx_.mem->read8(s)) != '\0') {
      if (curr == c)
        res = s;
      s++;
    }
    if (c == '\0')
      res = s;
    ctx_.r[V0] = res;
    break;
  }
  case 0x2E: { // memchr
    uint32_t s = ctx_.r[A0];
    uint8_t c = ctx_.r[A1] & 0xFF;
    uint32_t n = ctx_.r[A2];
    uint32_t res = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (ctx_.mem->read8(s + i) == c) {
        res = s + i;
        break;
      }
    }
    ctx_.r[V0] = res;
    break;
  }
  case 0x2F: // rand
    sRandSeed = sRandSeed * 1103515245 + 12345;
    ctx_.r[V0] = (sRandSeed >> 16) & 0x7FFF;
    break;
  case 0x30: // srand
    sRandSeed = ctx_.r[A0];
    break;

  // Character functions
  case 0x25: // toupper
    ctx_.r[V0] = std::toupper(static_cast<unsigned char>(ctx_.r[A0]));
    break;
  case 0x26: // tolower
    ctx_.r[V0] = std::tolower(static_cast<unsigned char>(ctx_.r[A0]));
    break;

  // Memory functions
  case 0x27: { // bcopy (src, dst, len) -- note: args reversed from memcpy
    uint32_t src = ctx_.r[A0];
    uint32_t dst = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    ctx_.mem->copyBytes(dst, src, n);
    break;
  }
  case 0x28: { // bzero (alias)
    uint32_t dst = ctx_.r[A0];
    uint32_t n = ctx_.r[A1];
    for (uint32_t i = 0; i < n; i++)
      ctx_.mem->write8(dst + i, 0);
    break;
  }
  case 0x29: { // bcmp (alias)
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    int res = 0;
    for (uint32_t i = 0; i < n; i++) {
      if (ctx_.mem->read8(s1 + i) != ctx_.mem->read8(s2 + i)) {
        res = 1;
        break;
      }
    }
    ctx_.r[V0] = res;
    break;
  }
  case 0x2A: // memcpy
    ctx_.mem->copyBytes(ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    ctx_.r[V0] = ctx_.r[A0];
    break;
  case 0x2B: // memset
    for (uint32_t i = 0; i < ctx_.r[A2]; i++) {
      ctx_.mem->write8(ctx_.r[A0] + i, ctx_.r[A1] & 0xFF);
    }
    ctx_.r[V0] = ctx_.r[A0];
    break;
  case 0x2C: { // memmove -- handles overlapping regions
    uint32_t dst = ctx_.r[A0];
    uint32_t src = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    ctx_.mem->copyBytes(dst, src, n);
    ctx_.r[V0] = ctx_.r[A0];
    break;
  }
  case 0x2D: { // memcmp
    uint32_t s1 = ctx_.r[A0];
    uint32_t s2 = ctx_.r[A1];
    uint32_t n = ctx_.r[A2];
    int32_t res = 0;
    for (uint32_t i = 0; i < n; i++) {
      int d = ctx_.mem->read8(s1 + i) - ctx_.mem->read8(s2 + i);
      if (d != 0) {
        res = d;
        break;
      }
    }
    ctx_.r[V0] = static_cast<uint32_t>(res);
    break;
  }

  // Heap
  case 0x33: // malloc
    ctx_.r[V0] = heap_.malloc(ctx_.r[A0]);
    break;
  case 0x34: // free
    heap_.free(ctx_.r[A0]);
    break;
  case 0x35: { // calloc
    uint32_t nmemb = ctx_.r[A0];
    uint32_t size = ctx_.r[A1];
    uint32_t total = nmemb * size;
    uint32_t ptr = heap_.malloc(total);
    if (ptr != 0) {
      for (uint32_t i = 0; i < total; i++)
        ctx_.mem->write8(ptr + i, 0);
    }
    ctx_.r[V0] = ptr;
    break;
  }
  case 0x36: { // realloc -- simplified: alloc new + copy + free old
    uint32_t oldPtr = ctx_.r[A0];
    uint32_t newSize = ctx_.r[A1];
    if (newSize == 0) {
      heap_.free(oldPtr);
      ctx_.r[V0] = 0;
      break;
    }
    uint32_t newPtr = heap_.malloc(newSize);
    if (oldPtr != 0 && newPtr != 0) {
      const uint32_t oldSize = heap_.allocationSize(oldPtr);
      ctx_.mem->copyBytes(newPtr, oldPtr, std::min(oldSize, newSize));
      heap_.free(oldPtr);
    }
    ctx_.r[V0] = newPtr;
    break;
  }
  case 0x39: // InitHeap
    heap_.initHeap(ctx_.r[A0], ctx_.r[A1]);
    break;
  case 0x3B: // _exit
    BIOS_LOG("[BIOS] _exit({})\n", static_cast<int32_t>(ctx_.r[A0]));
    break;

  // Printf
  case 0x3F:
    stub_printf();
    break;

  // GPU functions
  case 0x46: { // GPU_dw -- copy data block to GP0 (used by PutDispEnv init)
    // A0=src_addr, A1=count (words). Sends each word to GP0.
    uint32_t addr = ctx_.r[A0];
    uint32_t count = ctx_.r[A1];
    BIOS_LOG("[BIOS] GPU_dw(addr: 0x{:08X}, count: {})\n", addr, count);
    if (gpu_) {
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t source = addr + i * 4;
        const uint32_t word = ctx_.mem->read32(source);
        gpu_->writeGP0(word, source, nullptr, PGXPSubmissionPath::Bios);
      }
    }
    break;
  }
  case 0x47: { // gpu_send_dma -- wait for DMA, then trigger GPU DMA
    BIOS_LOG("[BIOS] gpu_send_dma() [STUB]\n");
    ctx_.r[V0] = 0;
    break;
  }
  case 0x48: { // SendGP1Command -- write single word to GP1 (0x1F801814)
    // PutDispEnv calls this to set display area (GP1 0x05), ranges (0x06/0x07),
    // and display mode (0x08). Critical for double-buffer display swap.
    uint32_t cmd = ctx_.r[A0];
    BIOS_LOG("[BIOS] SendGP1Command(0x{:08X})\n", cmd);
    if (gpu_) {
      gpu_->writeGP1(cmd);
    }
    ctx_.r[V0] = cmd;
    break;
  }
  case 0x4B: { // send_gpu_linked_list -- send GP0 OT via DMA-like walk
    // A0 = pointer to linked list tail. Walk the list sending GP0 commands.
    // Each entry: [23:0]=next_ptr, [31:24]=word_count, followed by count words.
    uint32_t ptr = ctx_.r[A0];
    BIOS_LOG("[BIOS] send_gpu_linked_list(0x{:08X})\n", ptr);
    if (gpu_) {
      int safety = 0;
      while ((ptr & 0xFFFFFF) != 0xFFFFFF && safety++ < 100000) {
        uint32_t hdr = ctx_.mem->read32(ptr);
        uint32_t wordCount = (hdr >> 24) & 0xFF;
        for (uint32_t i = 0; i < wordCount; i++) {
          const uint32_t source = ptr + 4 + i * 4;
          const uint32_t word = ctx_.mem->read32(source);
          gpu_->writeGP0(word, source, nullptr, PGXPSubmissionPath::Bios);
        }
        ptr = hdr & 0xFFFFFF;
        if (ptr != 0xFFFFFF)
          ptr |= 0x80000000u; // restore KSEG0 bit
      }
    }
    break;
  }
  case 0x4D: { // GetGPUStatus -- read GPUSTAT register
    ctx_.r[V0] = gpu_ ? gpu_->readGPUSTAT() : 0x14802000;
    break;
  }
  case 0x4E: { // gpu_sync -- wait for GPU to be idle
    // Our GPU is always synchronous, so this is a NOP.
    ctx_.r[V0] = 0;
    break;
  }
  case 0x49: { // GPU_cw -- send single GP0 command word
    uint32_t cmd = ctx_.r[A0];
    BIOS_LOG("[BIOS] GPU_cw(0x{:08X})\n", cmd);
    if (gpu_) {
      gpu_->writeGP0(cmd, UINT32_MAX, nullptr, PGXPSubmissionPath::Bios);
    }
    break;
  }
  case 0x4A: { // GPU_cwp -- send multiple GP0 command words from RAM
    uint32_t addr = ctx_.r[A0];
    uint32_t count = ctx_.r[A1];
    BIOS_LOG("[BIOS] GPU_cwp(addr: 0x{:08X}, count: {})\n", addr, count);
    if (gpu_) {
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t source = addr + i * 4;
        const uint32_t word = ctx_.mem->read32(source);
        gpu_->writeGP0(word, source, nullptr, PGXPSubmissionPath::Bios);
      }
    }
    break;
  }

  // Cache
  case 0x40: // FlushCache -- NOP in recompiler
  case 0x44: // FlushCache (alternate)
    break;

  // PsyQ libapi: directory iteration (PS1 BIOS A0:42 / A0:43)
  // Real BIOS returns a pointer to the DIRENTRY out-buffer (a1) when a
  // matching file is found, NULL (0) on no-match.  We return 0 to signal
  // "no entry" cleanly -- game's firstfile2 callers we've seen take the
  // not-found branch (which skips file-iteration init paths) instead of
  // reading garbage from a half-populated DIRENTRY.  A proper
  // implementation would walk the ISO9660 directory via VirtualFs;
  // upgrade if a future game depends on enumerating disc contents.
  case 0x42: // firstfile2(name, *dir_entry)
    BIOS_LOG("[BIOS] A0:42 firstfile2 [STUB returning 0 = no entry]\n");
    ctx_.r[V0] = 0;
    break;
  case 0x43: // nextfile(*dir_entry)
    BIOS_LOG("[BIOS] A0:43 nextfile [STUB returning 0 = end of dir]\n");
    ctx_.r[V0] = 0;
    break;

  // CD-ROM stubs
  case 0x70: // _bu_init
    BIOS_LOG("[BIOS] _bu_init() [STUB]\n");
    break;
  case 0x72: // _96_CdStop
    BIOS_LOG("[BIOS] _96_CdStop() [STUB]\n");
    break;
  case 0x78: // _96_CdAutoPause
    BIOS_LOG("[BIOS] _96_CdAutoPause({}) [STUB]\n", ctx_.r[A0]);
    break;
  case 0x7C: // _96_CdReadSector
    BIOS_LOG("[BIOS] _96_CdReadSector() [STUB]\n");
    ctx_.r[V0] = 0;
    break;

  case 0xAB: // _96_CdInitSubFunc -- PsyQ CD initialization sub-function
    BIOS_LOG("[BIOS] _96_CdInitSubFunc({}) -- firing INT3+INT2\n", ctx_.r[A0]);
    // On real hardware _96_CdInitSubFunc runs as the INT2 completion handler
    // for the CdlInit command.  Before returning it sends a GetStat (0x01)
    // which immediately generates INT3 (Ack).  The CdInit polling loop then
    // checks testEvent(0) [spec=0x0004 = INT3] to verify the drive responded.
    // We fire INT3 first so that testEvent(0) succeeds, then INT2 for event 1.
    triggerCdromEvent(3); // INT3 Ack  -> spec 0x0004 -> event 0 (GetStat Ack)
    triggerCdromEvent(2); // INT2 Complete -> spec 0x8000 -> event 1
    ctx_.r[V0] = 0;       // success
    break;

  case 0xAC: { // _96_CdGetStatus -- send GetStat to query disc/drive state
    // PsyQ CdInit calls this after _96_CdInitSubFunc to probe disc presence.
    // On real HW: sends GetStat(0x01), which fires INT3(Ack) with status byte.
    // We deliver INT3 synchronously and cancel the controller's pending
    // response to prevent double-interrupt for games using
    // SetCustomExitFromException.
    BIOS_LOG("[BIOS] _96_CdGetStatus() -- HLE GetStat (synchronous)\n");
    if (cdrom_) {
      cdrom_->writeRegister(0x1F801800, 0);    // index = 0
      cdrom_->writeRegister(0x1F801801, 0x01); // GetStat
      cdrom_->cancelPendingInterrupt();
    }
    triggerCdromEvent(3); // INT3 Ack -> disc-present status
    ctx_.r[V0] = 0;
    break;
  }

  case 0xAD: { // _96_CdReset -- send CdlInit (hardware reset)
    BIOS_LOG("[BIOS] _96_CdReset() -- HLE CdlInit (synchronous)\n");
    // Initialize the CDROM controller state without queuing an async response.
    // We deliver INT3+INT2 synchronously below, so we DON'T want the controller
    // to fire its own deferred INT3+INT2 later (that would cause double-events,
    // disrupting games that use SetCustomExitFromException for CD interrupts).
    if (cdrom_) {
      cdrom_->writeRegister(0x1F801800, 0); // index = 0
      cdrom_->writeRegister(0x1F801801,
                            0x0A); // CdlInit -> sets controller state
      // Discard the queued response so tick() doesn't fire a duplicate event.
      cdrom_->cancelPendingInterrupt();
    }
    // Fire INT3+INT2 immediately on the game thread so CdSync/testEvent/
    // SetCustomExitFromException handlers see the response right away.
    triggerCdromEvent(3); // INT3 Ack      -> cdSyncByte = 2
    triggerCdromEvent(2); // INT2 Complete -> cdSyncByte = 2
    ctx_.r[V0] = 0;       // success
    break;
  }

  default:
    BIOS_LOG("[BIOS] Unimplemented A0 call: 0x{:02X} (A0: {:08X}, A1: "
             "{:08X}, A2: {:08X})\n",
             index, ctx_.r[A0], ctx_.r[A1], ctx_.r[A2]);
    break;
  }
}

} // namespace ps1::bios
