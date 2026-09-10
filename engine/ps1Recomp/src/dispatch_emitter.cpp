#include "ps1recomp/dispatch_emitter.h"

namespace ps1recomp {

std::string emitDispatchBody() {
  return R"CPP(void recomp_dispatch(uint8_t* rdram, recomp_context* ctx, uint32_t addr) {
    // Lazy-init on first call
    if (!recomp_table_ready) recomp_init_dispatch_table();

    // Compatibility-interpreter exits are architectural tail transfers. Keep
    // resolving them in this dispatcher frame instead of recursively calling
    // recomp_dispatch once per private label. Ordinary JAL/JALR calls still
    // use nested dispatcher frames and therefore retain their return boundary.
    uint32_t deferredTailHops = 0;
    auto continueDeferredTail = [&]() {
        uint32_t target = 0;
        if (!ps1::mips::takeDeferredTailDispatch(target)) return false;
        if (++deferredTailHops > 1'000'000u) {
            fmt::print(stderr,
                       "[DISPATCH] FATAL: interpreter tail chain exceeded "
                       "one million transfers at 0x{:08X}\n", target);
            std::abort();
        }
        addr = target;
        return true;
    };

    for (;;) {

    // 1. NULL pointer guard
    if (addr == 0) [[unlikely]] {
        static bool nullDispatchWarned = false;
        if (!nullDispatchWarned) {
            nullDispatchWarned = true;
            fmt::print("[DISPATCH] null addr suppressed (startup transient, RA=0x{:08X})\n", ctx->r[31]);
        }
        return;
    }

    if (auto overlay = recomp_lookup_overlay(rdram, addr)) {
        overlay(rdram, ctx);
        if (continueDeferredTail()) continue;
        return;
    }

    // 2. Direct lookup in dispatch table
    recomp_func_t fn = recomp_lookup(addr);
    if (fn) {
        fn(rdram, ctx);
        if (continueDeferredTail()) continue;
        return;
    }

    // 3. Address normalization (KSEG mirrors) and retry
    uint32_t phys = addr & 0x1FFFFFFFu;
    uint32_t normalized = phys | 0x80000000u;
    if (normalized != addr) {
        fn = recomp_lookup(normalized);
        if (fn) {
            fn(rdram, ctx);
            if (continueDeferredTail()) continue;
            return;
        }
    }

    // 3b. Configured native entry aliases. Hand-written engines can jump to
    // an internal label with a non-ABI register convention. Re-enter the one
    // compiled owner at that exact label instead of duplicating its body or
    // guessing a return convention in the instruction fallback.
    fn = recomp_lookup_entry_alias(normalized);
    if (fn) {
        ctx->nativeEntryPc = normalized;
        fn(rdram, ctx);
        if (continueDeferredTail()) continue;
        return;
    }

    // Private labels inside a configured compatibility island are valid code
    // even when the analyzer did not promote them to standalone functions.
    if (recomp_try_static_interpreter(rdram, ctx, normalized)) {
        if (continueDeferredTail()) continue;
        return;
    }

    // 4. BIOS entry points (A0, B0, C0 -- any KSEG mirror)
    if (ctx->bios) {
        if (phys == 0xA0) { ps1::runtimeBiosExecuteA0(ctx->bios); return; }
        if (phys == 0xB0) { ps1::runtimeBiosExecuteB0(ctx->bios); return; }
        if (phys == 0xC0) { ps1::runtimeBiosExecuteC0(ctx->bios); return; }
    }

    // 5. BIOS table sentinel dispatch (B0:xx / C0:xx)
    if (ctx->bios) {
        if (phys >= 0xB000 && phys < 0xB100) {
            ctx->r[9] = phys & 0xFF; // set $t1 = function index
            ps1::runtimeBiosExecuteB0(ctx->bios);
            return;
        }
        if (phys >= 0xC000 && phys < 0xC100) {
            ctx->r[9] = phys & 0xFF;
            ps1::runtimeBiosExecuteC0(ctx->bios);
            return;
        }
        if (phys >= 0xA000 && phys < 0xA100) {
            ctx->r[9] = phys & 0xFF;
            ps1::runtimeBiosExecuteA0(ctx->bios);
            return;
        }
    }

    // 6. JR RA trampoline detection in RAM, restricted to empty trampolines.
    // A non-NOP delay slot is guest work and must fall through to the dynamic
    // R3000A interpreter.
    // Crash 3 uses `jr $ra; ori $s5, $zero, 0` to terminate streamed GOOL
    // programs; swallowing that delay slot leaves the VM program counter live.
    if (phys + 8u <= 0x200000u) { // Two words within 2MB main RAM
        uint32_t instr = (uint32_t)rdram[phys] | ((uint32_t)rdram[phys+1] << 8) |
                         ((uint32_t)rdram[phys+2] << 16) | ((uint32_t)rdram[phys+3] << 24);
        uint32_t delaySlot = (uint32_t)rdram[phys+4] | ((uint32_t)rdram[phys+5] << 8) |
                             ((uint32_t)rdram[phys+6] << 16) | ((uint32_t)rdram[phys+7] << 24);
        if (instr == 0x03E00008u && delaySlot == 0x00000000u) {
            return; // JR RA trampoline -- no-op return
        }
    }

    // 7. Heap-loaded native code (for example Crash 2/3 GOOL entries).
    // This is opt-in and range/shape checked by the runtime, so a genuinely
    // missing static function still reaches the fatal diagnostic below.
    if (phys < 0x200000u &&
        ps1::mips::interpretDynamicCall(rdram, ctx, addr)) {
        if (continueDeferredTail()) continue;
        return;
    }

    // 8. Unmapped target -- fatal unless PS1_DISPATCH_PERMISSIVE
    //
    // Anything reaching here is code the recompiler never emitted. Logging and
    // continuing turns the miss into a silent no-op: execution proceeds with
    // corrupted state and the symptom surfaces far from its cause. Aborting
    // makes the failure point at the cause, which is what makes the fix loop
    // converge -- run, read the address and caller, emit it, repeat.
    static const bool s_permissive = []() {
        const char* e = std::getenv("PS1_DISPATCH_PERMISSIVE");
        return e && e[0] != '\0' && std::strcmp(e, "0") != 0;
    }();
    if (!s_permissive) {
        fmt::print(stderr,
                   "[DISPATCH] FATAL: unmapped call to 0x{:08X} (RA=0x{:08X}, phys=0x{:08X})\n"
                   "           a0=0x{:08X} a1=0x{:08X} a3=0x{:08X} "
                   "s0=0x{:08X} s5=0x{:08X} s6=0x{:08X} "
                   "s7=0x{:08X} gp=0x{:08X} sp=0x{:08X} fp=0x{:08X}\n"
                   "           This address was never emitted by the recompiler.\n"
                   "           Set PS1_DISPATCH_PERMISSIVE=1 to log and continue instead.\n",
                   addr, ctx->r[31], phys, ctx->r[4], ctx->r[5], ctx->r[7],
                   ctx->r[16], ctx->r[21], ctx->r[22], ctx->r[23], ctx->r[28],
                   ctx->r[29], ctx->r[30]);
        const uint32_t spPhys = ctx->r[29] & 0x1FFFFFu;
        if (spPhys <= 0x1FFFE0u) {
            auto ram32 = [&](uint32_t off) {
                const uint8_t* p = rdram + spPhys + off;
                return static_cast<uint32_t>(p[0]) |
                       (static_cast<uint32_t>(p[1]) << 8) |
                       (static_cast<uint32_t>(p[2]) << 16) |
                       (static_cast<uint32_t>(p[3]) << 24);
            };
            fmt::print(stderr,
                       "           [sp+00..1C] {:08X} {:08X} {:08X} {:08X} "
                       "{:08X} {:08X} {:08X} {:08X}\n",
                       ram32(0), ram32(4), ram32(8), ram32(12), ram32(16),
                       ram32(20), ram32(24), ram32(28));
        }
        std::fflush(stderr);
        std::abort();
    }
    static std::unordered_map<uint32_t, uint32_t> s_unknownHits;
    auto& hitCount = s_unknownHits[addr];
    if (hitCount < 5) {
        fmt::print(stderr, "[DISPATCH] Unknown target: 0x{:08X} (RA=0x{:08X}, phys=0x{:08X})\n",
                   addr, ctx->r[31], phys);
    } else if (hitCount == 5) {
        fmt::print(stderr, "[DISPATCH] Unknown target: 0x{:08X} -- suppressing further logs\n", addr);
    }
    hitCount++;
    return;
    }
}
)CPP";
}

} // namespace ps1recomp
