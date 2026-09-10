/**
 * @file gool_interp.cpp
 * @brief HLE interpreter for the GOOL bytecode VM (Crash Bandicoot).
 *
 * Crash Bandicoot drives all game objects through GOOL, an in-game
 * bytecode VM whose native interpreter lives at 0x800201DC in the
 * SCUS-94900 binary. This file re-implements that VM in C++ on top of
 * the EMU_* shim (`runtime/gool/emu_compat.h`), so the interpreter can
 * be observed, traced, and extended while everything it calls out to
 * (NS paging, object lifecycle, GTE math) stays native recompiled code
 * reached through `EMU_Invoke`.
 *
 * Semantics were derived from the game binary's disassembly with the
 * ManDude `c1c` decompilation as a cross-reference for opcode encoding
 * and struct layout (see `gool_types.h`). Register-style sections (e.g.
 * `opPaging`) intentionally mirror the original MIPS register flow.
 *
 * Interception: the recompiler's `[hle_overrides]` stanza replaces the
 * body of `func_800201DC` with a call to `ps1::psyq::hle_gool_InterpretObject`,
 * so both direct JAL callers and indirect dispatch reach this VM.
 *
 * Opcodes not yet ported log once and abort the current interpretation
 * with the game's own INVALID error code; the log line names the opcode
 * so coverage can be extended exactly where the boot path needs it.
 */

#include "runtime/gool/emu_compat.h"
#include "runtime/gool/gool_types.h"

#include "runtime/cpu_context.h"
#include "runtime/emu_globals.h"
#include "runtime/emuptr.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_hle.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace ps1::gool {
namespace {

// MIPS register aliases over the bound recomp_context (see emu_compat.h).
#define GREG(n) (::ps1::gool::emu_reg(n))
#define AT GREG(1)
#define V0 GREG(2)
#define V1 GREG(3)
#define A0 GREG(4)
#define A1 GREG(5)
#define A2 GREG(6)
#define S0 GREG(16)
#define S1 GREG(17)
#define S2 GREG(18)
#define S3 GREG(19)
#define S4 GREG(20)
#define S5 GREG(21)
#define S6 GREG(22)
#define S7 GREG(23)
#define SP GREG(29)
#define FP GREG(30)
#define RA GREG(31)
constexpr uint32_t R0 = 0;

// Game error-code convention (values >= ALLOCFAILED are errors).
constexpr uint32_t ERR_SUCCESS = 0xFFFFFF01;
constexpr uint32_t ERR_ALLOCFAILED = 0xFFFFFF02;
constexpr uint32_t ERR_INVALIDSTATERETURN = 0xFFFFFFE6;
constexpr uint32_t ERR_INVALIDRETURN = 0xFFFFFFE7;
constexpr uint32_t ERR_INVALID = 0xFFFFFFF2;
inline bool isError(uint32_t x) { return x >= ERR_ALLOCFAILED; }

// Native callouts (Crash SCUS-94900 addresses); all are recompiled
// functions reached through recomp_dispatch via EMU_Invoke.
constexpr uint32_t FN_RAND = 0x8002F6B0;
constexpr uint32_t FN_TEST_CONTROLS = 0x8001FDC4;
constexpr uint32_t FN_SIN = 0x8003905C;
constexpr uint32_t FN_ANG_DIST = 0x800245F0;
constexpr uint32_t FN_CHANGE_OBJECT_STATE = 0x8001D698;
constexpr uint32_t FN_CREATE_OBJECT = 0x8001C6C8;
constexpr uint32_t FN_SEND_EVENT_BROADCAST = 0x80025134;
constexpr uint32_t FN_SEND_EVENT_DIRECT = 0x80024040;
constexpr uint32_t FN_SEND_EVENT_CASCADE = 0x800251B8;
constexpr uint32_t FN_NS_LOOKUP = 0x8001FC4C;
constexpr uint32_t FN_NS_PAGE_A = 0x80015118;
constexpr uint32_t FN_NS_PAGE_B = 0x80015458;
constexpr uint32_t FN_NS_PAGE_C = 0x800156D4;
constexpr uint32_t FN_NS_PAGE_D = 0x8001579C;
constexpr uint32_t FN_SOLID_RANGE_TEST = 0x80029C90;
constexpr uint32_t FN_OBJECT_FLIP = 0x8001D33C;

// GOOL globals in game BSS.
EMUGLOBALVAR(::ps1::emuptr<uint32_t>, constbuf, 0x80056480)
EMUGLOBALVAR(uint32_t, activebufindex, 0x80056484)
EMUGLOBALVAR(::ps1::emuptr<goolobj>, crash_obj, 0x800566B4)
EMUGLOBALVAR(uint32_t, rendercount, 0x80057960)
EMUGLOBALVAR(::ps1::emuptr<uint8_t>, dispenv, 0x80058404)
EMUGLOBALVAR(uint32_t, tickselapsed, 0x80060E04)
EMUGLOBALARR(uint32_t, gool_globals, 0x8006188C)

// Instruction field extraction.
inline uint32_t opcodeOf(uint32_t ins) { return ins >> 24; }
inline uint32_t gopA(uint32_t ins) { return (ins >> 12) & 0xFFF; }
inline uint32_t gopB(uint32_t ins) { return ins & 0xFFF; }

constexpr uint32_t GOP_STACKTOP = 0xE1F;

// Object stack / register-file helpers. The GOOL "register file" is the
// run of uint32 slots starting at the object's `self` field.
inline uint32_t selfBase(emuptr<goolobj> obj) {
  return obj.addr() + offsetof(goolobj, self);
}
inline emuptr<uint32_t> objReg(emuptr<goolobj> obj, uint32_t idx) {
  return emuptr<uint32_t>(selfBase(obj) + idx * 4);
}
inline void push(emuptr<goolobj> obj, uint32_t v) {
  EMU_Write32((obj->sp++).addr(), v);
}
inline uint32_t pop(emuptr<goolobj> obj) {
  return EMU_ReadU32((--obj->sp).addr());
}
inline uint32_t peek(emuptr<goolobj> obj) {
  return EMU_ReadU32(obj->sp.addr());
}
// Conditional operand: register index, or stack pop when 0x1F.
inline uint32_t regRef(emuptr<goolobj> obj, uint32_t idx) {
  return (idx == 0x1F) ? pop(obj) : EMU_ReadU32(objReg(obj, idx).addr());
}
inline uint32_t fpRel(emuptr<goolobj> obj) {
  return obj->fp.addr() ? obj->fp.addr() - selfBase(obj) : 0;
}
inline uint32_t spRel(emuptr<goolobj> obj) {
  return obj->sp.addr() ? obj->sp.addr() - selfBase(obj) : 0;
}
inline uint32_t frameRange(uint32_t fprel, uint32_t sprel) {
  return (fprel << 16) | (sprel & 0xFFFF);
}

// Operand load/store go through Memory so a null/garbage translated
// pointer behaves like the console (a plain RAM access) instead of
// tripping host-side asserts.
inline uint32_t ld(emuptr<uint32_t> p) { return EMU_ReadU32(p.addr()); }
inline void st(emuptr<uint32_t> p, uint32_t v) { EMU_Write32(p.addr(), v); }

// --- Operand translation (gop -> memory ref) ---------------------------

emuptr<uint32_t> translateGop(emuptr<goolobj> obj, uint32_t gop) {
  if (!(gop & 0x800)) { // ireg (local) or pool (external) ref
    uint32_t index = gop & 0x3FF;
    uint32_t execdata = (!(gop & 0x400)) ? obj->local->items[2].addr()
                                         : obj->external->items[2].addr();
    return emuptr<uint32_t>(execdata + index * 4);
  }
  if (!(gop & 0x400)) {
    // The const-buffer pointer (0x80056480) targets SCRATCHPAD (0x1F800040
    // in Crash). emuptr raw derefs only know main RAM, so the stores must
    // go through Memory (EMU_Write32); the returned ref is later read via
    // ld() which is Memory-routed too.
    if (!(gop & 0x200)) { // sign-extended int constant
      int32_t intval = static_cast<int32_t>((gop & 0x1FF) << 23) >> 15;
      activebufindex() = !activebufindex();
      emuptr<uint32_t> slot = constbuf() + activebufindex();
      EMU_Write32(slot.addr(), static_cast<uint32_t>(intval));
      return slot;
    }
    if (!(gop & 0x100)) { // sign-extended fractional constant
      int32_t fracval = static_cast<int32_t>((gop & 0xFF) << 24) >> 20;
      activebufindex() = !activebufindex();
      emuptr<uint32_t> slot = constbuf() + activebufindex();
      EMU_Write32(slot.addr(), static_cast<uint32_t>(fracval));
      return slot;
    }
    if (!(gop & 0x80)) { // frame-pointer-relative var
      int32_t index = static_cast<int32_t>((gop & 0x3F) << 26) >> 26;
      return emuptr<uint32_t>(obj->fp.addr() +
                              static_cast<uint32_t>(index * 4));
    }
    if (gop == 0xBE0) // null ref
      return emuptr<uint32_t>(0);
    if (gop == 0xBF0) // sp-double sentinel
      return emuptr<uint32_t>(0xBF0);
    return emuptr<uint32_t>(1); // invalid ref
  }
  // link-object register ref
  uint32_t linkindex = (gop >> 6) & 0x7;
  uint32_t linkreg = gop & 0x3F;
  emuptr<goolobj> link(EMU_ReadU32(objReg(obj, linkindex).addr()));
  if (link)
    return emuptr<uint32_t>(selfBase(link) + linkreg * 4);
  return emuptr<uint32_t>(0);
}

emuptr<uint32_t> inGop(emuptr<goolobj> obj, uint32_t gop) {
  if ((gop & 0xFFF) == GOP_STACKTOP) // stack pop
    return --obj->sp;
  if ((gop & 0xE00) == 0xE00) // stack/register-file ref
    return objReg(obj, gop & 0x1FF);
  return translateGop(obj, gop);
}

emuptr<uint32_t> outGop(emuptr<goolobj> obj, uint32_t gop) {
  if ((gop & 0xFFF) == GOP_STACKTOP) // stack push
    return obj->sp++;
  if ((gop & 0xE00) == 0xE00)
    return objReg(obj, gop & 0x1FF);
  return translateGop(obj, gop);
}

inline uint32_t ldA(emuptr<goolobj> obj, uint32_t ins) {
  return ld(inGop(obj, gopA(ins)));
}
inline uint32_t ldB(emuptr<goolobj> obj, uint32_t ins) {
  return ld(inGop(obj, gopB(ins)));
}

// --- Interpreter frame (mirrors the native prologue/epilogue) -----------

thread_local uint32_t g_retflag = 0;
thread_local uint32_t g_retcode = 0;

void nativeEnter() {
  SP -= 320;
  EMU_Write32(SP + 316, RA);
  EMU_Write32(SP + 312, FP);
  EMU_Write32(SP + 308, S7);
  EMU_Write32(SP + 304, S6);
  EMU_Write32(SP + 300, S5);
  EMU_Write32(SP + 296, S4);
  EMU_Write32(SP + 292, S3);
  EMU_Write32(SP + 288, S2);
  EMU_Write32(SP + 284, S1);
  EMU_Write32(SP + 280, S0);
}

void nativeReturn(uint32_t result) {
  RA = EMU_ReadU32(SP + 316);
  FP = EMU_ReadU32(SP + 312);
  S7 = EMU_ReadU32(SP + 308);
  S6 = EMU_ReadU32(SP + 304);
  S5 = EMU_ReadU32(SP + 300);
  S4 = EMU_ReadU32(SP + 296);
  S3 = EMU_ReadU32(SP + 292);
  S2 = EMU_ReadU32(SP + 288);
  S1 = EMU_ReadU32(SP + 284);
  S0 = EMU_ReadU32(SP + 280);
  SP += 320;
  g_retflag = 1;
  g_retcode = result;
}

#define greturn(x)                                                             \
  do {                                                                         \
    nativeReturn(x);                                                           \
    return;                                                                    \
  } while (0)

int traceLevel() {
  static int level = [] {
    const char *e = std::getenv("PS1_GOOL_TRACE");
    return e ? std::atoi(e) : 0;
  }();
  return level;
}

// --- Named operations (boot-path set) -----------------------------------

void opControlFlow(emuptr<goolobj> obj, uint32_t ins, uint32_t &flags,
                   emuptr<goolstateref> transition) {
  // `cond` persists across invocations: condtype 3 reuses the last result.
  thread_local uint32_t cond = 0;
  thread_local uint32_t flagspair = 0;

  uint32_t condtype = (ins >> 20) & 3;
  if (condtype == 0)
    cond = 1;
  else if (condtype == 1)
    cond = regRef(obj, (ins >> 14) & 0x3F);
  else if (condtype == 2)
    cond = !regRef(obj, (ins >> 14) & 0x3F);

  uint32_t optype = (ins >> 22) & 3;
  if (!cond || optype == 3)
    return;

  if (optype == 0) { // branch
    int32_t offset = static_cast<int32_t>((ins & 0x3FF) << 22) >> 22;
    int32_t varskip = (ins >> 10) & 0xF;
    obj->pc += offset;
    obj->sp -= varskip;
  } else if (optype == 1) { // state change
    uint32_t state = ins & 0x3FFF;
    emuptr<goolstateinfo> execstates(obj->local->items[4].addr());
    emuptr<goolstateinfo> stateinfo = execstates + state;

    uint32_t stateflagstest;
    if (obj->displaymode >= 2 && obj->displaymode <= 4)
      stateflagstest = (obj->statusc | 0x1002) & stateinfo->flags;
    else
      stateflagstest = obj->statusc & stateinfo->flags;

    if (!stateflagstest) {
      uint32_t result =
          EMU_Invoke(FN_CHANGE_OBJECT_STATE, 4, obj.addr(), state, 0, 0);
      if (isError(result) || !(flags & 2))
        pop(obj);
      else
        greturn(result);
    }
  } else { // optype == 2: return from the current GOOL frame
    flagspair = flags;

    obj->sp = obj->fp + 2;
    uint32_t callerframerange = peek(obj);
    uint32_t callerframefp = callerframerange >> 16;
    uint32_t callerframesp = callerframerange & 0xFFFF;

    uint32_t result;
    if (callerframefp) { // not the initial frame
      result = ERR_SUCCESS;

      obj->sp = obj->fp + 1;
      obj->pc = emuptr<uint32_t>(peek(obj));

      pop(obj);
      uint32_t prevflags = peek(obj);
      flagspair = (flags & 0xFFFF0000u) | (prevflags & 0xFFFF);

      obj->sp = emuptr<uint32_t>(selfBase(obj) + callerframesp);
      obj->fp = emuptr<uint32_t>(selfBase(obj) + callerframefp);
    } else {
      result = ERR_INVALIDRETURN;
    }

    if (flags & 8) {
      if (flags & 0x20) {
        transition->state = 0xFF;
        greturn(ERR_SUCCESS);
      }
      greturn(ERR_INVALIDSTATERETURN);
    }

    if ((flags & 1) || isError(result)) {
      if ((flags & 0x10) && !isError(result))
        push(obj, 0); // initial value for wait
      greturn(result);
    }
    if ((flagspair & 0xFFFF) != 0xFFFF)
      flags = flagspair;
  }
}

void opJumpAndLink(emuptr<goolobj> obj, uint32_t ins, uint32_t &flags) {
  uint32_t address = ins & 0x3FFF;
  uint32_t argc = (ins >> 20) & 0xF;

  uint32_t fprel = fpRel(obj);
  uint32_t sprel = spRel(obj) - argc * 4;

  obj->fp = obj->sp;
  push(obj, flags);
  push(obj, obj->pc.addr());
  push(obj, frameRange(fprel, sprel));

  if (address != 0x3FFF) {
    uint32_t execcode = obj->local->items[1].addr();
    obj->pc = emuptr<uint32_t>(execcode + address * 4);
  } else {
    obj->pc = emuptr<uint32_t>(0);
  }
  flags &= 0xFFFFFFF6u;
}

void opReturnStateTransition(emuptr<goolobj> obj, uint32_t ins,
                             uint32_t &flags,
                             emuptr<goolstateref> transition,
                             uint32_t opcode) {
  if (!(flags & 8))
    greturn(ERR_INVALID);

  thread_local uint32_t cond = 0;

  uint32_t condtype = (ins >> 20) & 3;
  if (condtype == 0)
    cond = 1;
  else if (condtype == 1)
    cond = regRef(obj, (ins >> 14) & 0x3F);
  else if (condtype == 2)
    cond = !regRef(obj, (ins >> 14) & 0x3F);

  uint32_t rettype = (ins >> 22) & 3;
  if (cond) {
    flags |= 0x20; // RETURNEVENT
    transition->guard = (opcode == 0x89);

    if (rettype == 1 || rettype == 2) {
      obj->sp = obj->fp + 2;
      uint32_t callerframerange = peek(obj);
      uint32_t callerframefp = callerframerange >> 16;
      uint32_t callerframesp = callerframerange & 0xFFFF;

      uint32_t result;
      if (callerframefp) {
        obj->sp = obj->fp + 1;
        obj->pc = emuptr<uint32_t>(peek(obj));
        pop(obj);

        obj->sp = emuptr<uint32_t>(selfBase(obj) + callerframesp);
        obj->fp = emuptr<uint32_t>(selfBase(obj) + callerframefp);
        result = ERR_SUCCESS;
      } else {
        result = ERR_INVALIDRETURN;
      }

      if (!isError(result))
        transition->state = (rettype == 1) ? (ins & 0x3FFF) : 0xFF;
      greturn(result);
    }
  } else if (rettype == 0) {
    int32_t offset = static_cast<int32_t>((ins & 0x3FF) << 22) >> 22;
    int32_t varskip = (ins >> 10) & 0xF;
    obj->pc += offset;
    obj->sp -= varskip;
  }
}

void opSendEvent(emuptr<goolobj> obj, uint32_t ins, uint32_t &flags,
                 emuptr<goolobj> recipient, uint32_t opcode) {
  SP -= 64;
  emuptr<uint32_t> args(SP); // scratch buffer in the interpreter frame

  emuptr<uint32_t> eventptr = inGop(obj, gopB(ins));

  obj->statusa &= 0xFFFDFFFFu; // clear bit 17 (state-changed marker)

  uint32_t cond = regRef(obj, (ins >> 12) & 0x3F);
  uint32_t argc = (ins >> 18) & 0x7;
  uint32_t mode = (ins >> 21) & 0x7;

  if (eventptr && cond && (recipient || opcode == 0x8F)) {
    for (uint32_t i = 0; i < argc; i++)
      args[i] = ld(obj->sp + (static_cast<int32_t>(i) -
                              static_cast<int32_t>(argc)));

    uint32_t event = ld(eventptr);
    if (opcode == 0x8F)
      EMU_Invoke(FN_SEND_EVENT_BROADCAST, 5, obj.addr(), event, mode, argc,
                 args.addr());
    else if (opcode == 0x87)
      EMU_Invoke(FN_SEND_EVENT_DIRECT, 5, obj.addr(), recipient.addr(), event,
                 argc, args.addr());
    else if (opcode == 0x90)
      EMU_Invoke(FN_SEND_EVENT_CASCADE, 6, obj.addr(), recipient.addr(), event,
                 mode, argc, args.addr());
  } else {
    obj->misc_flag = 0;
  }
  SP += 64;

  if (obj->statusa & 0x20000) { // state changed as a result of the event
    if (flags & 2)
      greturn(ERR_SUCCESS);
    pop(obj);
  } else {
    obj->sp -= argc;
  }
}

void opSpawnChildren(emuptr<goolobj> obj, uint32_t ins, uint32_t opcode) {
  uint32_t spawncount = ins & 0x3F;
  uint32_t argc = (ins >> 20) & 0xF;

  if (!spawncount) {
    argc -= 1;
    spawncount = ld(obj->sp + static_cast<int32_t>(argc));
  }

  if (spawncount > 0) {
    uint32_t flag = (opcode == 0x91);
    for (uint32_t i = 0; i < spawncount; i++) {
      uint32_t type = (ins >> 12) & 0xFF;
      uint32_t subtype = (ins >> 6) & 0x3F;

      uint32_t argsAddr = obj->sp.addr() - argc * 4;
      uint32_t child = EMU_Invoke(FN_CREATE_OBJECT, 6, obj.addr(), type,
                                  subtype, argc, argsAddr, flag);

      obj->misc_child =
          isError(child) ? emuptr<goolobj>(0) : emuptr<goolobj>(child);
      // The game writes `creator` without checking the error path; done
      // through Memory so an error-code "address" degrades like on console.
      EMU_Write32(child + offsetof(goolobj, creator), obj.addr());
    }
  }
  obj->sp -= argc;
}

// Register-style port: the NS paging op drives the native page loaders
// (0x80015118 / 0x80015458 / 0x800156D4 / 0x8001579C) exactly as the
// original code does, args pre-set in A0..A2.
void opPaging(emuptr<goolobj> obj, uint32_t ins) {
  S1 = ins;
  S2 = obj.addr();

  A0 = S2;
  A1 = (S1 >> 12) & 0xFFF;
  A2 = R0;
  V0 = EMU_Invoke(FN_NS_LOOKUP, 0);
  A0 = S2;
  A1 = S1 & 0xFFF;
  S0 = EMU_ReadU32(V0);
  A2 = R0;
  V0 = EMU_Invoke(FN_NS_LOOKUP, 0);
  A0 = V0;
  S0 -= 1;
  if (S0 >= 6) {
    V0 = S0 << 2;
    return;
  }
  V0 = S0 << 2;
  AT = 0x80010000 + V0;
  V0 = EMU_ReadU32(AT + 2040); // original jumptable fetch (kept for fidelity)

  switch (S0) {
  case 0:
  case 5:
    A1 = (S0 == 5) ? 1 : R0;
    A2 = 1;
    V0 = EMU_Invoke(FN_NS_PAGE_A, 0);
    EMU_Write32(S2 + 244, V0); // obj->misc (+0xF4)
    return;
  case 1:
    A1 = 1;
    V0 = EMU_Invoke(FN_NS_PAGE_B, 0);
    EMU_Write32(S2 + 244, V0);
    return;
  case 2:
    A1 = R0;
    V0 = EMU_Invoke(FN_NS_PAGE_B, 0);
    A0 = EMU_ReadU32(S2 + 220); // obj->sp (+0xDC)
    V1 = A0 + 4;
    break;
  case 3:
    V0 = EMU_Invoke(FN_NS_PAGE_C, 0);
    A0 = EMU_ReadU32(S2 + 220);
    V1 = A0 + 4;
    break;
  case 4:
    S0 = EMU_ReadU32(A0);
    V0 = EMU_ReadU32(S2 + 220);
    A0 = S0 << 2;
    A0 = V0 - A0;
    A1 = S0;
    V0 = EMU_Invoke(FN_NS_PAGE_D, 0);
    A0 = EMU_ReadU32(S2 + 220);
    S0 <<= 2;
    A0 = A0 - S0;
    V1 = A0 + 4;
    break;
  }
  EMU_Write32(S2 + 220, V1);
  EMU_Write32(A0, V0);
}

void opChangeAnim(emuptr<goolobj> obj, uint32_t ins, uint32_t &flags) {
  uint32_t animframe = ins & 0x7F;
  uint32_t anim = (ins >> 7) & 0x1FF;
  uint32_t wait = (ins >> 16) & 0x3F;
  uint32_t flip = (ins >> 22) & 0x3;

  uint32_t execanims = obj->local->items[5].addr();
  obj->animframe = animframe << 8;
  obj->animseq = emuptr<uint32_t>(execanims + anim * 4);

  push(obj, (wait << 24) | tickselapsed());

  int32_t sx = obj->scale.x;
  if (flip == 0)
    obj->scale.x = sx < 0 ? sx : -sx;
  else if (flip == 1)
    obj->scale.x = sx < 0 ? -sx : sx;
  else if (flip == 2)
    obj->scale.x = -sx;

  if (obj->statusb & 0x18) { // solid object stopped by solid surfaces
    uint32_t outofrange = EMU_Invoke(
        FN_SOLID_RANGE_TEST, 6, obj.addr(),
        obj.addr() + offsetof(goolobj, trans),
        crash_obj().addr() + offsetof(goolobj, trans), 0x7D000, 0xAF000,
        0x7D000);
    if (!outofrange || (obj->statusb & 0x80000000u))
      EMU_Invoke(FN_OBJECT_FLIP, 2, obj.addr(),
                 obj.addr() + offsetof(goolobj, scale));
  }

  if (flags & 4)
    greturn(ERR_SUCCESS);
}

void opChangeAnimFrame(emuptr<goolobj> obj, uint32_t ins, uint32_t &flags) {
  uint32_t animframe = ldB(obj, ins);
  uint32_t wait = (ins >> 16) & 0x3F;
  uint32_t flip = (ins >> 22) & 0x3;

  obj->animframe = animframe;
  push(obj, (wait << 24) | tickselapsed());

  int32_t sx = obj->scale.x;
  if (flip == 0)
    obj->scale.x = sx < 0 ? sx : -sx;
  else if (flip == 1)
    obj->scale.x = sx < 0 ? -sx : sx;
  else if (flip == 2)
    obj->scale.x = -sx;

  EMU_Invoke(FN_OBJECT_FLIP, 2, obj.addr(),
             obj.addr() + offsetof(goolobj, scale));

  if (flags & 4)
    greturn(ERR_SUCCESS);
}

void opNotPorted(emuptr<goolobj> obj, uint32_t ins) {
  static bool warned[256] = {};
  uint32_t opcode = opcodeOf(ins);
  char mname[32];
  std::snprintf(mname, sizeof(mname), "gool.opcode_missing.%02X", opcode);
  ps1::metrics::count(mname);
  if (!warned[opcode]) {
    warned[opcode] = true;
    std::fprintf(stderr,
                 "[GOOL] opcode 0x%02X not ported (ins=0x%08X pc=0x%08X "
                 "obj=0x%08X) -- aborting this interpretation\n",
                 opcode, ins, (obj->pc - 1).addr(), obj.addr());
  }
  greturn(ERR_INVALID);
}

// --- Core interpretation loop -------------------------------------------

uint32_t interpretObject(emuptr<goolobj> obj, uint32_t flags,
                         emuptr<goolstateref> transition) {
  nativeEnter();
  ps1::metrics::count("gool.interpret");

  emuptr<goolobj> recipient{};
  uint32_t argbuf = 0;
  const bool traceOps = traceLevel() >= 2;

  do {
    FP = GOP_STACKTOP;
    uint32_t ins = EMU_ReadU32(obj->pc.addr());
    obj->pc += 1;
    uint32_t opcode = opcodeOf(ins);

    if (traceOps)
      std::fprintf(stderr, "[GOOL]   pc=0x%08X ins=0x%08X op=0x%02X\n",
                   (obj->pc - 1).addr(), ins, opcode);

    switch (opcode) {
    case 0x00: {
      uint32_t r = ldA(obj, ins), l = ldB(obj, ins);
      push(obj, l + r);
      break;
    }
    case 0x01: {
      uint32_t r = ldA(obj, ins), l = ldB(obj, ins);
      push(obj, l - r);
      break;
    }
    case 0x02: {
      uint32_t r = ldA(obj, ins), l = ldB(obj, ins);
      push(obj, l * r);
      break;
    }
    case 0x03: {
      // Unsigned divide; MIPS DIVU by zero yields all-ones, no trap.
      uint32_t r = ldA(obj, ins), l = ldB(obj, ins);
      push(obj, r ? l / r : 0xFFFFFFFFu);
      break;
    }
    case 0x04: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      push(obj, !(a ^ b));
      break;
    }
    case 0x05: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      push(obj, b ? (a != 0) : 0);
      break;
    }
    case 0x06: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      push(obj, a || b);
      break;
    }
    case 0x07: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      push(obj, a & b);
      break;
    }
    case 0x08: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      push(obj, a | b);
      break;
    }
    case 0x09: {
      int32_t r = static_cast<int32_t>(ldA(obj, ins));
      int32_t l = static_cast<int32_t>(ldB(obj, ins));
      push(obj, l > r);
      break;
    }
    case 0x0A: {
      int32_t r = static_cast<int32_t>(ldA(obj, ins));
      int32_t l = static_cast<int32_t>(ldB(obj, ins));
      push(obj, l >= r);
      break;
    }
    case 0x0B: {
      int32_t r = static_cast<int32_t>(ldA(obj, ins));
      int32_t l = static_cast<int32_t>(ldB(obj, ins));
      push(obj, l < r);
      break;
    }
    case 0x0C: {
      int32_t r = static_cast<int32_t>(ldA(obj, ins));
      int32_t l = static_cast<int32_t>(ldB(obj, ins));
      push(obj, l <= r);
      break;
    }
    case 0x0D: {
      // Unsigned modulo; DIVU-by-zero remainder is the numerator.
      uint32_t r = ldA(obj, ins), l = ldB(obj, ins);
      push(obj, r ? l % r : l);
      break;
    }
    case 0x0E: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      push(obj, a ^ b);
      break;
    }
    case 0x0F: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      push(obj, !((a & b) ^ a));
      break;
    }
    case 0x10: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      if (a == b) {
        push(obj, b);
      } else {
        uint32_t rnd = EMU_Invoke(FN_RAND, 1, a - b);
        push(obj, b + rnd);
      }
      break;
    }
    case 0x11: {
      uint32_t a = ldA(obj, ins);
      emuptr<uint32_t> dst = outGop(obj, gopB(ins));
      if (dst)
        st(dst, a);
      break;
    }
    case 0x12: {
      uint32_t a = ldA(obj, ins);
      emuptr<uint32_t> dst = outGop(obj, gopB(ins));
      st(dst, !a);
      break;
    }
    case 0x14: {
      emuptr<uint32_t> src = inGop(obj, gopA(ins));
      emuptr<uint32_t> dst = outGop(obj, gopB(ins));
      st(dst, src.addr());
      break;
    }
    case 0x15: {
      // Variable shift; count masks to 5 bits like MIPS SLLV/SRAV.
      int32_t sr = static_cast<int32_t>(ldA(obj, ins));
      int32_t sl = static_cast<int32_t>(ldB(obj, ins));
      if (sr < 0)
        push(obj, static_cast<uint32_t>(sl >> ((-sr) & 31)));
      else
        push(obj, static_cast<uint32_t>(sl) << (sr & 31));
      break;
    }
    case 0x16:
    case 0x26: {
      emuptr<uint32_t> src = inGop(obj, gopA(ins));
      emuptr<uint32_t> dst = inGop(obj, gopB(ins));
      if (opcode == 0x26) {
        if (!dst)
          break;
        push(obj, dst.addr());
        if (src)
          push(obj, src.addr());
      } else {
        if (!dst)
          break;
        if (src)
          argbuf = ld(src);
        push(obj, ld(dst));
        if (src)
          push(obj, argbuf);
      }
      break;
    }
    case 0x17: {
      emuptr<uint32_t> src = inGop(obj, gopA(ins));
      emuptr<uint32_t> dst = outGop(obj, gopB(ins));
      if (dst)
        st(dst, ~ld(src));
      break;
    }
    case 0x18: {
      uint32_t execcode = obj->local->items[1].addr();
      uint32_t offset = ins & 0x3FFF;
      uint32_t mem = (ins >> 14) & 0x3F;
      uint32_t target = execcode + offset * 4;
      if (mem == 0x1F)
        push(obj, target);
      else
        EMU_Write32(objReg(obj, mem).addr(), target);
      break;
    }
    case 0x19: {
      int32_t sa = static_cast<int32_t>(ldA(obj, ins));
      emuptr<uint32_t> dst = outGop(obj, gopB(ins));
      st(dst, static_cast<uint32_t>(sa < 0 ? -sa : sa));
      break;
    }
    case 0x1A: {
      uint32_t result = EMU_Invoke(FN_TEST_CONTROLS, 2, ins & 0x1FFFFF, 0);
      push(obj, result);
      break;
    }
    case 0x1B: {
      int32_t sa = static_cast<int32_t>(ldA(obj, ins));
      int32_t sb = static_cast<int32_t>(ldB(obj, ins));
      uint32_t gamespeed = EMU_ReadU32(dispenv().addr() + 0x84);
      int32_t speed = (gamespeed >= 0x67) ? sa * 0x66
                                          : sa * static_cast<int32_t>(gamespeed);
      if (speed < 0)
        speed += 0x3FF;
      push(obj, static_cast<uint32_t>(sb + (speed >> 10)));
      break;
    }
    case 0x1D: {
      int32_t sa = static_cast<int32_t>(ldA(obj, ins));
      int32_t sb = static_cast<int32_t>(ldB(obj, ins));
      uint32_t angle =
          sa ? static_cast<uint32_t>((sb << 11) / sa) - 0x400 : 0;
      uint32_t sin = EMU_Invoke(FN_SIN, 1, angle);
      uint32_t sinadjusted = sin + 0x1000;
      uint32_t sinscaled =
          (sinadjusted * static_cast<uint32_t>(sa)) >> 13;
      push(obj, sinscaled);
      break;
    }
    case 0x1E: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      uint32_t n = a + rendercount();
      push(obj, b ? n % b : n);
      break;
    }
    case 0x1F: {
      uint32_t b = ldB(obj, ins);
      push(obj, gool_globals()[b >> 8]);
      break;
    }
    case 0x20: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      gool_globals()[b >> 8] = a;
      break;
    }
    case 0x21: {
      uint32_t a = ldA(obj, ins), b = ldB(obj, ins);
      uint32_t angdist = EMU_Invoke(FN_ANG_DIST, 2, b, a);
      push(obj, angdist);
      break;
    }
    case 0x23: {
      uint32_t linkindex = (ins >> 12) & 7;
      uint32_t colorindex = (ins >> 15) & 0x3F;
      uint32_t link = EMU_ReadU32(objReg(obj, linkindex).addr());
      uint32_t color =
          EMU_ReadU16(link + offsetof(goolobj, colors) + colorindex * 2);
      push(obj, color);
      break;
    }
    case 0x24: {
      uint32_t b = ldB(obj, ins);
      uint32_t linkindex = (ins >> 12) & 7;
      uint32_t colorindex = (ins >> 15) & 0x3F;
      uint32_t link = EMU_ReadU32(objReg(obj, linkindex).addr());
      EMU_Write16(link + offsetof(goolobj, colors) + colorindex * 2,
                  static_cast<uint16_t>(b));
      break;
    }
    case 0x27: {
      uint32_t a = ldA(obj, ins);
      emuptr<uint32_t> dst = outGop(obj, gopB(ins));
      uint32_t execanims = obj->local->items[5].addr();
      st(dst, execanims + a / 64);
      break;
    }
    case 0x82:
      opControlFlow(obj, ins, flags, transition);
      break;
    case 0x83:
      opChangeAnim(obj, ins, flags);
      break;
    case 0x84:
      opChangeAnimFrame(obj, ins, flags);
      break;
    case 0x86:
      opJumpAndLink(obj, ins, flags);
      break;
    case 0x87:
    case 0x90:
      recipient =
          emuptr<goolobj>(EMU_ReadU32(objReg(obj, (ins >> 21) & 0x7).addr()));
      [[fallthrough]];
    case 0x8F:
      opSendEvent(obj, ins, flags, recipient, opcode);
      break;
    case 0x88:
    case 0x89:
      opReturnStateTransition(obj, ins, flags, transition, opcode);
      break;
    case 0x8A:
    case 0x91:
      opSpawnChildren(obj, ins, opcode);
      break;
    case 0x8B:
      opPaging(obj, ins);
      break;
    default:
      // 0x13, 0x1C (Misc), 0x22/0x25, 0x85, 0x8C/0x8D (audio), 0x8E and
      // anything unknown: not in the boot-path slice yet.
      opNotPorted(obj, ins);
      break;
    }
  } while (!g_retflag);

  g_retflag = 0;
  return g_retcode;
}

#undef greturn

} // namespace
} // namespace ps1::gool

namespace ps1::psyq {

void hle_gool_InterpretObject(recomp_context *ctx) {
  using namespace ps1::gool;
  using ps1::emuptr;

  // Re-entrant: native ChangeObjectState calls the interpreter recursively.
  static thread_local int depth = 0;
  static thread_local uint32_t calls = 0;

  auto *rdram = static_cast<uint8_t *>(ps1::emuptr_translate(0x80000000u));
  if (depth++ == 0)
    emu_bind(rdram, ctx);
  ++calls;

  // Entry sampler: object state at the first calls and every power of two.
  // Gated behind PS1_GOOL_TRACE like every other diagnostic in this file --
  // it used to print unconditionally because it was the go/no-go instrument
  // for the "does interpreting GOOL populate the NS hash table" question,
  // which closed on 2026-07-09 (hypothesis A confirmed). The hash-table words
  // it sampled for that question went with it; `gool.interpret` in the metric
  // report is the uncapped call count now.
  if (traceLevel() >= 1 && (calls <= 4 || (calls & (calls - 1)) == 0)) {
    uint32_t objAddr = ctx->r[4];
    std::fprintf(stderr,
                 "[GOOL] interpret #%u obj=0x%08X flags=0x%X pc=0x%08X "
                 "sp=%08X fp=%08X local=%08X\n",
                 calls, objAddr, ctx->r[5],
                 objAddr ? EMU_ReadU32(objAddr + offsetof(goolobj, pc)) : 0,
                 objAddr ? EMU_ReadU32(objAddr + offsetof(goolobj, sp)) : 0,
                 objAddr ? EMU_ReadU32(objAddr + offsetof(goolobj, fp)) : 0,
                 objAddr ? EMU_ReadU32(objAddr + offsetof(goolobj, local)) : 0);
  }

  uint32_t result = interpretObject(emuptr<goolobj>(ctx->r[4]), ctx->r[5],
                                    emuptr<goolstateref>(ctx->r[6]));
  ctx->r[2] = result; // V0

  if (--depth == 0)
    emu_unbind();
}

} // namespace ps1::psyq
