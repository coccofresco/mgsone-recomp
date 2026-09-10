#include "runtime/psyq/psyq_libapi.h"
#include "runtime/bios/bios.h"
#include "runtime/psyq/psyq_registry.h"

#include <cstdint>

namespace ps1::psyq {

namespace {

// All libapi BIOS-wrapper HLEs share this shape: load $t1 with the syscall
// index, then dispatch through the matching A/B/C table.  Bios::execute*
// reads $a0..$a3 and writes $v0 -- exactly what the original PsyQ wrapper does.
inline void dispatchA(recomp_context *ctx, uint32_t index) {
  ctx->r[T1] = index;
  ctx->bios->executeA0();
}
inline void dispatchB(recomp_context *ctx, uint32_t index) {
  ctx->r[T1] = index;
  ctx->bios->executeB0();
}
inline void dispatchC(recomp_context *ctx, uint32_t index) {
  ctx->r[T1] = index;
  ctx->bios->executeC0();
}

} // namespace

// Event manager
void hle_libapi_OpenEvent(recomp_context *ctx)    { dispatchB(ctx, 0x08); }
void hle_libapi_CloseEvent(recomp_context *ctx)   { dispatchB(ctx, 0x09); }
void hle_libapi_DeliverEvent(recomp_context *ctx) { dispatchB(ctx, 0x07); }
void hle_libapi_EnableEvent(recomp_context *ctx)  { dispatchB(ctx, 0x0C); }
void hle_libapi_DisableEvent(recomp_context *ctx) { dispatchB(ctx, 0x0D); }
void hle_libapi_TestEvent(recomp_context *ctx)    { dispatchB(ctx, 0x0B); }

// Critical section (kernel SYSCALL on real HW -- stubbed here)
//
// EnterCriticalSection: SYS(01h).  Disables interrupts; returns 1 if they had
// been enabled.  Our recompiler does not model COP0 interrupt enable bits, so
// drainPendingCallbacks always runs at deterministic yield points regardless.
// Returning 1 matches what real PsyQ code expects.
void hle_libapi_EnterCriticalSection(recomp_context *ctx) {
  ctx->r[V0] = 1;
}
void hle_libapi_ExitCriticalSection(recomp_context *ctx) {
  ctx->r[V0] = 0;
}

// Exception / IRQ chain
void hle_libapi_ReturnFromException(recomp_context *ctx) {
  dispatchB(ctx, 0x17);
}
// HookEntryInt(*entry) -> C(0x01) SysEnqIntRP -- register a handler in the
// priority chain.  PsyQ's HookEntryInt computes priority/handler from the
// `entry` struct and forwards to SysEnqIntRP; the C0 stub already accepts
// (priority, handler) but the recompiled binary supplies them via $a0/$a1
// after the libapi wrapper unpacks the struct.  In practice, HookEntryInt
// for our purposes can be a NOP: nothing in the recompiled environment runs
// the IRQ chain.  We still call SysEnqIntRP so the BIOS_LOG line fires.
void hle_libapi_HookEntryInt(recomp_context *ctx) {
  dispatchC(ctx, 0x01);
  ctx->r[V0] = 0;
}
void hle_libapi_ChangeClearRCnt(recomp_context *ctx) {
  dispatchC(ctx, 0x0A);
}

// File / Memory Card I/O
void hle_libapi_open(recomp_context *ctx)   { dispatchB(ctx, 0x32); }
void hle_libapi_close(recomp_context *ctx)  { dispatchB(ctx, 0x36); }
void hle_libapi_read(recomp_context *ctx)   { dispatchB(ctx, 0x34); }
void hle_libapi_write(recomp_context *ctx)  { dispatchB(ctx, 0x35); }
void hle_libapi_erase(recomp_context *ctx)  { dispatchB(ctx, 0x45); } // delete(filename)
void hle_libapi_format(recomp_context *ctx) { dispatchB(ctx, 0x41); }
void hle_libapi__bu_init(recomp_context *ctx)   { dispatchA(ctx, 0x70); }
// PS1 BIOS table A: A0:0x42 = firstfile2, A0:0x43 = nextfile.
// Previously dispatched to B0:0x42 (SetConf -- stub returns 1 = "ok") and
// B0:0x43 (HookEntryInt) which silently corrupted the firstfile2 caller's
// out-pointer with wrong values.  Game subsequently tried to read a file
// at a garbage LBA and the CD wait loop hung pre-render.
void hle_libapi_firstfile2(recomp_context *ctx) { dispatchA(ctx, 0x42); }
void hle_libapi_nextfile(recomp_context *ctx)   { dispatchA(ctx, 0x43); }
void hle_libapi__96_remove(recomp_context *ctx) { dispatchA(ctx, 0x71); }

// Pad
void hle_libapi_InitPAD2(recomp_context *ctx)       { dispatchB(ctx, 0x12); }
void hle_libapi_StartPAD2(recomp_context *ctx)      { dispatchB(ctx, 0x13); }
void hle_libapi_StopPAD2(recomp_context *ctx)       { dispatchB(ctx, 0x14); }
void hle_libapi_ChangeClearPAD(recomp_context *ctx) { dispatchB(ctx, 0x5B); }

// Heap
void hle_libapi_InitHeap(recomp_context *ctx) { dispatchA(ctx, 0x39); }

// GPU command word passthrough
void hle_libapi_GPU_cw(recomp_context *ctx) { dispatchA(ctx, 0x49); }

void psyq_register_libapi() {
  psyq_register("libapi_OpenEvent",            &hle_libapi_OpenEvent);
  psyq_register("libapi_CloseEvent",           &hle_libapi_CloseEvent);
  psyq_register("libapi_DeliverEvent",         &hle_libapi_DeliverEvent);
  psyq_register("libapi_EnableEvent",          &hle_libapi_EnableEvent);
  psyq_register("libapi_DisableEvent",         &hle_libapi_DisableEvent);
  psyq_register("libapi_TestEvent",            &hle_libapi_TestEvent);
  psyq_register("libapi_EnterCriticalSection", &hle_libapi_EnterCriticalSection);
  psyq_register("libapi_ExitCriticalSection",  &hle_libapi_ExitCriticalSection);
  psyq_register("libapi_ReturnFromException",  &hle_libapi_ReturnFromException);
  psyq_register("libapi_HookEntryInt",         &hle_libapi_HookEntryInt);
  psyq_register("libapi_ChangeClearRCnt",      &hle_libapi_ChangeClearRCnt);
  psyq_register("libapi_open",                 &hle_libapi_open);
  psyq_register("libapi_close",                &hle_libapi_close);
  psyq_register("libapi_read",                 &hle_libapi_read);
  psyq_register("libapi_write",                &hle_libapi_write);
  psyq_register("libapi_erase",                &hle_libapi_erase);
  psyq_register("libapi_format",               &hle_libapi_format);
  psyq_register("libapi__bu_init",             &hle_libapi__bu_init);
  psyq_register("libapi_firstfile2",           &hle_libapi_firstfile2);
  psyq_register("libapi_nextfile",             &hle_libapi_nextfile);
  psyq_register("libapi__96_remove",           &hle_libapi__96_remove);
  psyq_register("libapi_InitPAD2",             &hle_libapi_InitPAD2);
  psyq_register("libapi_StartPAD2",            &hle_libapi_StartPAD2);
  psyq_register("libapi_StopPAD2",             &hle_libapi_StopPAD2);
  psyq_register("libapi_ChangeClearPAD",       &hle_libapi_ChangeClearPAD);
  psyq_register("libapi_InitHeap",             &hle_libapi_InitHeap);
  psyq_register("libapi_GPU_cw",               &hle_libapi_GPU_cw);
}

} // namespace ps1::psyq
