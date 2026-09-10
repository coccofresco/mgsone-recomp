// Tests for ps1::psyq HLE implementations
// Covers: VSync, DrawSync, ResetGraph, ClearOTag, ClearOTagR, DrawOTag,
//         SetDefDispEnv, PutDispEnv, SetDefDrawEnv, PutDrawEnv

#include "runtime/cpu_context.h"
#include "runtime/emuptr.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include "runtime/psyq/psyq_hle.h"
#include "runtime/psyq/psyq_libgpu.h"
#include "runtime/psyq/psyq_state.h"
#include "runtime/timers/timers.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <vector>

using namespace ps1;
using namespace ps1::psyq;

// Fixture

class PsyqHleTest : public ::testing::Test {
  protected:
    Memory mem;
    recomp_context ctx;

    // GP0/GP1 capture
    std::vector<uint32_t> gp0Words;
    std::vector<uint32_t> gp0Sources;
    std::vector<PGXPValue> gp0Precisions;
    // list, node, traversal ordinal, last zero-word OT anchor, anchor exact.
    std::vector<std::array<uint32_t, 5>> gp0DmaProvenance;
    std::vector<uint32_t> gp1Words;

    // Each drainCallbacks call simulates one VBlank tick.
    int drainCalls = 0;

    void SetUp() override {
        mem.reset();
        ctx.reset();
        ctx.mem = &mem;
        emuptr_set_ram(mem.ramPtr());
        ctx.r[SP] = 0x801FF000; // stack pointer
        gp0Words.clear();
        gp0Sources.clear();
        gp0Precisions.clear();
        gp0DmaProvenance.clear();
        gp1Words.clear();
        drainCalls = 0;
        psyq_state().reset();

        HleConfig cfg;
        cfg.writeGP0 = [this](uint32_t w) { gp0Words.push_back(w); };
        cfg.writeGP0Tracked = [this](uint32_t w, uint32_t source,
                                     const PGXPValue *precision, uint32_t listStart,
                                     uint32_t nodeAddress,
                                     uint32_t traversalOrdinal,
                                     uint32_t orderingTableAnchorAddress,
                                     bool orderingTableAnchorExact) {
            gp0Words.push_back(w);
            gp0Sources.push_back(source);
            gp0Precisions.push_back(precision ? *precision : PGXPValue{});
            gp0DmaProvenance.push_back(
                {listStart, nodeAddress, traversalOrdinal,
                 orderingTableAnchorAddress,
                 orderingTableAnchorExact ? 1u : 0u});
        };
        cfg.writeGP1 = [this](uint32_t w) { gp1Words.push_back(w); };
        cfg.drainCallbacks = [this]() {
            ++drainCalls;
            psyq_state().vsyncCounter.fetch_add(1, std::memory_order_release);
        };
        configure(cfg);
    }

    void TearDown() override {
        psyq_state().reset();
        emuptr_set_ram(nullptr);
    }
};

// DrawSync

TEST_F(PsyqHleTest, DrawSyncMode0ReturnsZero) {
    ctx.r[A0] = 0; // mode 0 = wait for completion
    hle_DrawSync(&ctx);
    EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqHleTest, DrawSyncMode1ReturnsZero) {
    ctx.r[A0] = 1; // mode 1 = query remaining
    hle_DrawSync(&ctx);
    EXPECT_EQ(ctx.r[V0], 0u);
}

// ResetGraph

TEST_F(PsyqHleTest, ResetGraphIsNop) {
    ctx.r[A0] = 0;
    hle_ResetGraph(&ctx);
    // No crash, no GP0/GP1 commands emitted
    EXPECT_TRUE(gp0Words.empty());
    EXPECT_TRUE(gp1Words.empty());
}

// VSync

TEST_F(PsyqHleTest, VSyncWaitsOneFrame) {
    ctx.r[A0] = 0;
    hle_VSync(&ctx);
    EXPECT_GE(drainCalls, 1);
    EXPECT_GE(psyq_state().vsyncCounter.load(), 1u);
    EXPECT_EQ(ctx.r[V0], 263u);
    EXPECT_TRUE(psyq_state().blockingVSyncPacing.load());
    EXPECT_EQ(psyq_state().blockingVSyncTarget.load(), 0u);
}

TEST_F(PsyqHleTest, VSyncWaitsMultipleFrames) {
    ctx.r[A0] = 3;
    hle_VSync(&ctx);
    EXPECT_GE(psyq_state().vsyncCounter.load(), 3u);
    EXPECT_EQ(ctx.r[V0], 3u * 263u);
}

TEST_F(PsyqHleTest, VSyncZeroWaitsOneFrame) {
    ctx.r[A0] = 0; // n=0 means "wait for next VBlank"
    hle_VSync(&ctx);
    EXPECT_GE(psyq_state().vsyncCounter.load(), 1u);
    EXPECT_EQ(ctx.r[V0], 263u);
}

TEST_F(PsyqHleTest, VSyncZeroReportsElapsedEdgesPlusTheNewBlockingEdge) {
    psyq_state().lastVSyncCounter = 10;
    psyq_state().vsyncCounter.store(12, std::memory_order_release);

    ctx.r[A0] = 0;
    hle_VSync(&ctx);

    EXPECT_GE(drainCalls, 1);
    EXPECT_EQ(psyq_state().lastVSyncCounter, 13u);
    EXPECT_EQ(ctx.r[V0], 3u * 263u);
}

TEST_F(PsyqHleTest, VSyncNUsesThePreviousCompletionAsItsTarget) {
    psyq_state().lastVSyncCounter = 10;
    psyq_state().vsyncCounter.store(12, std::memory_order_release);

    ctx.r[A0] = 2;
    hle_VSync(&ctx);

    EXPECT_EQ(drainCalls, 0);
    EXPECT_EQ(psyq_state().lastVSyncCounter, 12u);
    EXPECT_EQ(ctx.r[V0], 2u * 263u);
}

TEST_F(PsyqHleTest, VSyncZeroWaitsForANewEdgeAndReportsFullElapsedInterval) {
    psyq_state().lastVSyncCounter = 10;
    psyq_state().vsyncCounter.store(11, std::memory_order_release);

    ctx.r[A0] = 0;
    hle_VSync(&ctx);

    // Mode 0 means the next vertical sync generated after this call. Frame 11
    // contributes to the elapsed return, but cannot satisfy the blocking
    // condition by itself; the callback advances the simulated clock to 12.
    EXPECT_GE(drainCalls, 1);
    EXPECT_EQ(psyq_state().vsyncCounter.load(), 12u);
    EXPECT_EQ(psyq_state().lastVSyncCounter, 12u);
    EXPECT_EQ(ctx.r[V0], 2u * 263u);
}

TEST_F(PsyqHleTest, VSyncNegativeReturnsAbsoluteCounterWithoutWaiting) {
    psyq_state().vsyncCounter.store(100, std::memory_order_release);
    ctx.r[A0] = static_cast<uint32_t>(-1);
    hle_VSync(&ctx);
    EXPECT_EQ(ctx.r[V0], 100u);
    EXPECT_EQ(drainCalls, 0);
    EXPECT_EQ(psyq_state().vsyncCounter.load(), 100u);
}

TEST_F(PsyqHleTest, VSyncModeOneQueriesElapsedTimeWithoutWaiting) {
    psyq_state().lastVSyncCounter = 7;
    psyq_state().vsyncCounter.store(10, std::memory_order_release);
    ctx.r[A0] = 1;
    hle_VSync(&ctx);
    EXPECT_EQ(ctx.r[V0], 3u * 263u);
    EXPECT_EQ(drainCalls, 0);
    EXPECT_EQ(psyq_state().lastVSyncCounter, 7u);
}

TEST_F(PsyqHleTest, RootCounterHleProgramsTheHardwareTimer) {
    Timers timers;
    InterruptController irq;
    mem.setTimers(&timers);
    mem.setInterruptController(&irq);

    ctx.r[A0] = 0xF2000002u; // RCntCNT2
    ctx.r[A1] = 4092;
    ctx.r[A2] = 0x1000; // RCntMdINTR + sysclock/8
    hle_SetRCnt(&ctx);

    EXPECT_EQ(ctx.r[V0], 1u);
    EXPECT_EQ(timers.readRegister(0x1F801128u), 4092u);
    EXPECT_EQ(timers.readRegister(0x1F801124u) & 0x07FFu, 0x0258u);

    ctx.r[A0] = 0xF2000002u;
    hle_StartRCnt(&ctx);
    EXPECT_EQ(irq.readIMask() & IRQ_TMR2, IRQ_TMR2);

    // One 50 Hz PAL interval supplies 33,868,800 / 50 / 8 = 84,672
    // timer ticks: twenty target IRQs plus a 2,832-tick remainder.
    EXPECT_NE(timers.tick(33868800u / 50u) & IRQ_TMR2, 0u);
    EXPECT_EQ(timers.lastIrqCount(2), 20u);
    EXPECT_EQ(timers.readRegister(0x1F801120u), 2832u);

    ctx.r[A0] = 0xF2000002u;
    hle_StopRCnt(&ctx);
    EXPECT_EQ(irq.readIMask() & IRQ_TMR2, 0u);
}

// Phase 3.2: the VBlank thread no longer calls Bios::triggerVBlankEvent()
// directly -- it only increments `psyq_state().pendingVblanks`. hle_VSync,
// running on the game thread, consumes that count at the end of its wait and
// invokes `deliverVBlankEvent` once for every edge.
//
// Two consecutive hle_VSync calls without an intervening VBlank thread
// tick must NOT both observe the same pending count -- the first call drains
// it and the second sees zero.
TEST_F(PsyqHleTest, VBlankPendingPreservesEveryEdge) {
    int vblankDeliveries = 0;
    HleConfig cfg = getConfig();
    cfg.deliverVBlankEvent = [&vblankDeliveries]() { ++vblankDeliveries; };
    configure(cfg);

    // Simulate two ticks accumulated while the render thread was stalled.
    psyq_state().vsyncCounter.store(0, std::memory_order_release);
    psyq_state().pendingVblanks.store(2, std::memory_order_release);

    // First VSync(-1) drains the pending asynchronous IRQ flag without
    // introducing a wait of its own.
    ctx.r[A0] = static_cast<uint32_t>(-1);
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 2);
    EXPECT_EQ(psyq_state().pendingVblanks.load(std::memory_order_acquire), 0u);

    // Second query -- no new VBlank thread tick happened, so the
    // flag must remain false and delivery count must NOT increment.
    // Only the deliverVBlankEvent path is exercised here.
    ctx.r[A0] = static_cast<uint32_t>(-1);
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 2)
        << "Same VBlank delivered twice across consecutive hle_VSync calls";
    EXPECT_EQ(psyq_state().pendingVblanks.load(std::memory_order_acquire), 0u);

    // Add one edge (simulates the next VBlank thread tick) and
    // confirm a third call now delivers a fresh VBlank.
    psyq_state().pendingVblanks.store(1, std::memory_order_release);
    ctx.r[A0] = static_cast<uint32_t>(-1);
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 3);
    EXPECT_EQ(psyq_state().pendingVblanks.load(std::memory_order_acquire), 0u);
}

// Negative case: if no VBlank ever fired, hle_VSync must not call
// deliverVBlankEvent at all.
TEST_F(PsyqHleTest, VSyncSkipsDeliveryWhenFlagNeverRaised) {
    int vblankDeliveries = 0;
    HleConfig cfg = getConfig();
    cfg.deliverVBlankEvent = [&vblankDeliveries]() { ++vblankDeliveries; };
    configure(cfg);

    // Flag stays false throughout.
    EXPECT_EQ(psyq_state().pendingVblanks.load(std::memory_order_acquire), 0u);

    ctx.r[A0] = static_cast<uint32_t>(-1);
    hle_VSync(&ctx);
    EXPECT_EQ(vblankDeliveries, 0);
}

// ClearOTag

TEST_F(PsyqHleTest, ClearOTagFillsEndMarker) {
    // Write a 4-entry OT starting at 0x1000
    const uint32_t base = 0x1000;
    ctx.r[A0] = base;
    ctx.r[A1] = 4;
    hle_ClearOTag(&ctx);

    // Last entry (ot[3]) must be end-of-list
    EXPECT_EQ(mem.read32(base + 3 * 4), 0x00FFFFFFu);
    // Return value = base
    EXPECT_EQ(ctx.r[V0], base);
}

TEST_F(PsyqHleTest, ClearOTagNZeroIsNop) {
    ctx.r[A0] = 0x2000;
    ctx.r[A1] = 0;
    hle_ClearOTag(&ctx);
    EXPECT_EQ(ctx.r[V0], 0x2000u);
}

// ClearOTagR

TEST_F(PsyqHleTest, ClearOTagRLastEntryIsEndMarker) {
    const uint32_t base = 0x2000;
    ctx.r[A0] = base;
    ctx.r[A1] = 4;
    hle_ClearOTagR(&ctx);

    // Last entry (ot[3]) = end-of-list
    EXPECT_EQ(mem.read32(base + 3 * 4), 0x00FFFFFFu);
}

TEST_F(PsyqHleTest, ClearOTagRLinksForward) {
    const uint32_t base = 0x2000;
    ctx.r[A0] = base;
    ctx.r[A1] = 3;
    hle_ClearOTagR(&ctx);

    // ot[0] should point to ot[1]
    EXPECT_EQ(mem.read32(base + 0 * 4), base + 1 * 4);
    // ot[1] should point to ot[2]
    EXPECT_EQ(mem.read32(base + 1 * 4), base + 2 * 4);
    // ot[2] = end
    EXPECT_EQ(mem.read32(base + 2 * 4), 0x00FFFFFFu);
}

TEST_F(PsyqHleTest, ClearOTagRNZeroIsNop) {
    ctx.r[A0] = 0x3000;
    ctx.r[A1] = 0;
    hle_ClearOTagR(&ctx);
    EXPECT_EQ(ctx.r[V0], 0x3000u);
}

// DrawOTag
//
// Audited 2026-08-06 against psx-spx's DMA2 linked-list documentation
// (docs/dmachannels.md:173-190) and OT worked example
// (docs/graphicsprocessingunitgpu.md:1084-1136), cross-referenced with the
// psyz decomp (sys.c:354-360, 850-855) confirming DrawOTag(p) is
// _cwc(p) -- a DMA2 linked-list kick, so the CPU-side traversal below is
// this project's software model of what the GPU's own DMA controller does
// on real hardware. Verdict: already correct, no source-shape divergence
// found. The four tests below pin: empty-list no-op, single-node word push,
// multi-node traversal order, and (new) that the header word itself is
// link-chain metadata and must never be pushed to GP0.

TEST_F(PsyqHleTest, DrawOTagEmptyListEmitsNothing) {
    // Write a single terminal node at 0x1000
    const uint32_t base = 0x1000;
    // header: 0 words, next = 0xFFFFFF (end)
    mem.write32(base, 0x00FFFFFFu);

    ctx.r[A0] = base;
    hle_DrawOTag(&ctx);

    EXPECT_TRUE(gp0Words.empty());
    EXPECT_EQ(ctx.r[V0], 0u);
}

TEST_F(PsyqHleTest, DrawOTagSubmitsSinglePrimitive) {
    // Node at 0x1000: 2 GP0 words, next = end
    const uint32_t base = 0x1000;
    // header: [31:24]=2 (word_count=2), [23:0]=0xFFFFFF (terminal)
    mem.write32(base + 0, (2u << 24) | 0x00FFFFFFu);
    mem.write32(base + 4, 0x01234567u); // GP0 word 0
    mem.write32(base + 8, 0x89ABCDEFu); // GP0 word 1

    ctx.r[A0] = base;
    hle_DrawOTag(&ctx);

    ASSERT_EQ(gp0Words.size(), 2u);
    EXPECT_EQ(gp0Words[0], 0x01234567u);
    EXPECT_EQ(gp0Words[1], 0x89ABCDEFu);
}

TEST_F(PsyqHleTest, DrawOTagBracketsWholeListAsOneGpuBatch) {
    std::vector<uint32_t> events;
    HleConfig cfg = getConfig();
    const auto trackedWriter = cfg.writeGP0Tracked;
    cfg.beginGP0Batch = [&events]() { events.push_back(1u); };
    cfg.writeGP0Tracked = [&events, trackedWriter](
                              uint32_t word, uint32_t source,
                              const PGXPValue *precision, uint32_t listStart,
                              uint32_t nodeAddress, uint32_t traversalOrdinal,
                              uint32_t orderingTableAnchorAddress,
                              bool orderingTableAnchorExact) {
        events.push_back(2u);
        trackedWriter(word, source, precision, listStart, nodeAddress,
                      traversalOrdinal, orderingTableAnchorAddress,
                      orderingTableAnchorExact);
    };
    cfg.endGP0Batch = [&events]() { events.push_back(3u); };
    configure(cfg);

    constexpr uint32_t base = 0x1000u;
    mem.write32(base, (2u << 24) | 0x00FFFFFFu);
    mem.write32(base + 4u, 0x01234567u);
    mem.write32(base + 8u, 0x89ABCDEFu);
    ctx.r[A0] = base;

    hle_DrawOTag(&ctx);

    EXPECT_EQ(events, (std::vector<uint32_t>{1u, 2u, 2u, 3u}));
}

TEST_F(PsyqHleTest, DrawOTagPreservesGuestAddressAndPgxpProvenance) {
    const uint32_t base = 0x1800;
    const uint32_t vertex = (40u << 16) | 20u;
    mem.write32(base, (1u << 24) | 0x00FFFFFFu);

    PGXPValue precision;
    precision.x = 20.25f;
    precision.y = 40.5f;
    precision.z = 128.0f;
    precision.value = vertex;
    precision.flags = PGXPValue::VALID_XY | PGXPValue::VALID_Z;
    mem.write32Tracked(base + 4, vertex, &precision);

    ctx.r[A0] = base;
    hle_DrawOTag(&ctx);

    ASSERT_EQ(gp0Words.size(), 1u);
    ASSERT_EQ(gp0Sources.size(), 1u);
    ASSERT_EQ(gp0Precisions.size(), 1u);
    ASSERT_EQ(gp0DmaProvenance.size(), 1u);
    EXPECT_EQ(gp0Words[0], vertex);
    EXPECT_EQ(gp0Sources[0], base + 4);
    EXPECT_TRUE(gp0Precisions[0].hasXYZ(vertex));
    EXPECT_FLOAT_EQ(gp0Precisions[0].x, 20.25f);
    EXPECT_FLOAT_EQ(gp0Precisions[0].y, 40.5f);
    EXPECT_EQ(gp0DmaProvenance[0],
              (std::array<uint32_t, 5>{base, base, 0u, 0u, 0u}));
}

TEST_F(PsyqHleTest, DrawOTagSeparatesVirtualWordSourceFromPhysicalDmaOwner) {
    constexpr uint32_t physicalBase = 0x1800u;
    constexpr uint32_t virtualBase = 0x80001800u;
    constexpr uint32_t vertex = (40u << 16) | 20u;
    mem.write32(physicalBase, (1u << 24) | 0x00FFFFFFu);
    mem.write32(physicalBase + 4u, vertex);

    ctx.r[A0] = virtualBase;
    hle_DrawOTag(&ctx);

    ASSERT_EQ(gp0Sources.size(), 1u);
    ASSERT_EQ(gp0DmaProvenance.size(), 1u);
    EXPECT_EQ(gp0Sources[0], virtualBase + 4u);
    EXPECT_EQ(gp0DmaProvenance[0],
              (std::array<uint32_t, 5>{physicalBase, physicalBase, 0u, 0u,
                                       0u}));
}

TEST_F(PsyqHleTest, DrawOTagTraversesChain) {
    // Node B at 0x2000 (tail, processed first): 1 word, next = end
    mem.write32(0x2000, (1u << 24) | 0x00FFFFFFu);
    mem.write32(0x2004, 0xAAAAAAAAu);

    // Node A at 0x1000 (head): 1 word, next = B (0x2000, without KSEG0 bit)
    mem.write32(0x1000, (1u << 24) | 0x002000u);
    mem.write32(0x1004, 0xBBBBBBBBu);

    ctx.r[A0] = 0x1000; // start at A
    hle_DrawOTag(&ctx);

    // A's word should come first, then B's
    ASSERT_EQ(gp0Words.size(), 2u);
    EXPECT_EQ(gp0Words[0], 0xBBBBBBBBu); // from A
    EXPECT_EQ(gp0Words[1], 0xAAAAAAAAu); // from B
}

TEST_F(PsyqHleTest, DrawOTagCarriesExactLastZeroWordOtAnchor) {
    // Canonical reverse-OT shape: traversal enters a zero-word bucket, then
    // follows the primitive chain attached to that bucket.
    constexpr uint32_t bucket = 0x1004u;
    constexpr uint32_t primitive = 0x2000u;
    constexpr uint32_t previousBucket = 0x1000u;
    mem.write32(bucket, primitive);
    mem.write32(primitive, (1u << 24) | previousBucket);
    mem.write32(primitive + 4u, 0x01234567u);
    mem.write32(previousBucket, 0x00FFFFFFu);

    ctx.r[A0] = bucket;
    hle_DrawOTag(&ctx);

    ASSERT_EQ(gp0DmaProvenance.size(), 1u);
    EXPECT_EQ(gp0DmaProvenance[0],
              (std::array<uint32_t, 5>{bucket, primitive, 1u, bucket, 1u}));
}

TEST_F(PsyqHleTest, DrawOTagTraceCarriesHleDmaNodeAndSubmissionIdentity) {
    const char *path = "/tmp/ps1_hle_ot_node_trace_test.jsonl";
    std::remove(path);
    setenv("PS1_TRACE_JSONL", path, 1);
    setenv("PS1_TRACE_EVENTS", "gpu.dma_node", 1);
    setenv("PS1_TRACE_GPU_WORDS", "1", 1);
    ps1::metrics::resetTraceForTesting();

    HleConfig cfg = getConfig();
    cfg.activeGP0SubmissionId = []() { return uint64_t{77}; };
    configure(cfg);

    mem.write32(0x80001000u, (1u << 24) | 0x002000u);
    mem.write32(0x80001004u, 0xBBBBBBBBu);
    mem.write32(0x80002000u, (1u << 24) | 0x00FFFFFFu);
    mem.write32(0x80002004u, 0xAAAAAAAAu);
    ctx.r[A0] = 0x80001000u;
    hle_DrawOTag(&ctx);

    ps1::metrics::resetTraceForTesting();
    std::ifstream in(path);
    ASSERT_TRUE(in.good());
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string body = buf.str();
    EXPECT_NE(body.find("\"list_start\":4096"), std::string::npos) << body;
    EXPECT_NE(body.find("\"node_address\":4096"), std::string::npos) << body;
    EXPECT_NE(body.find("\"node_address\":8192"), std::string::npos) << body;
    EXPECT_NE(body.find("\"submission_id\":77"), std::string::npos) << body;

    unsetenv("PS1_TRACE_JSONL");
    unsetenv("PS1_TRACE_EVENTS");
    unsetenv("PS1_TRACE_GPU_WORDS");
    ps1::metrics::resetTraceForTesting();
    std::remove(path);
}

// dmachannels.md:173-190 and the OT worked example in
// graphicsprocessingunitgpu.md:1084-1136 agree: only the N words AFTER the
// header are DMA'd to GP0. The header word (word_count | next_ptr) is
// link-chain metadata consumed by the DMA controller, never a GP0 command.
TEST_F(PsyqHleTest, DrawOTagHeaderWordItselfIsNeverPushedToGp0) {
    const uint32_t base = 0x1000;
    // Header word deliberately holds a recognisable "poison" pattern so the
    // test fails loudly if it ever leaks into gp0Words.
    mem.write32(base + 0, (1u << 24) | 0x00FFFFFFu);
    mem.write32(base + 4, 0xC0FFEEEEu); // the single real data word

    ctx.r[A0] = base;
    hle_DrawOTag(&ctx);

    ASSERT_EQ(gp0Words.size(), 1u);
    EXPECT_EQ(gp0Words[0], 0xC0FFEEEEu);
    for (uint32_t w : gp0Words) {
        EXPECT_NE(w, (1u << 24) | 0x00FFFFFFu)
            << "header word must never be pushed to GP0";
    }
}

// SetDefDispEnv

TEST_F(PsyqHleTest, SetDefDispEnvWritesFields) {
    const uint32_t env = 0x3000;
    ctx.r[A0] = env;
    ctx.r[A1] = 320; // x
    ctx.r[A2] = 0;   // y
    ctx.r[A3] = 320; // w
    // h = 240 passed on stack at SP+16
    mem.write32(ctx.r[SP] + 16, 240);

    hle_SetDefDispEnv(&ctx);

    EXPECT_EQ(mem.read16(env + 0), 320u); // disp.x
    EXPECT_EQ(mem.read16(env + 2), 0u);   // disp.y
    EXPECT_EQ(mem.read16(env + 4), 320u); // disp.w
    EXPECT_EQ(mem.read16(env + 6), 240u); // disp.h
    EXPECT_EQ(mem.read8(env + 16), 0u);   // isinter = 0
    EXPECT_EQ(mem.read8(env + 17), 0u);   // isrgb24 = 0
    EXPECT_EQ(ctx.r[V0], env);            // returns env ptr
}

// ext.c:72-86 zeroes all four DISPENV.screen fields; it does NOT seed them
// from w/h. This was inert until PutDispEnv started building GP1(06)/(07)
// from .screen, at which point `screen.w = w` suppressed the source's own
// screen.w == 0 default. Poison the struct first: the fields are asserted to
// be zero, and a zeroed Memory would let a no-write pass by accident.
TEST_F(PsyqHleTest, SetDefDispEnvZeroesAllScreenFields) {
    const uint32_t env = 0x3100;
    for (uint32_t i = 0; i < 20; i += 2)
        mem.write16(env + i, 0xBEEFu);

    ctx.r[A0] = env;
    ctx.r[A1] = 0;                    // x
    ctx.r[A2] = 0;                    // y
    ctx.r[A3] = 320;                  // w
    mem.write32(ctx.r[SP] + 16, 240); // h

    hle_SetDefDispEnv(&ctx);

    EXPECT_EQ(mem.read16(env + 8), 0u);  // screen.x
    EXPECT_EQ(mem.read16(env + 10), 0u); // screen.y
    EXPECT_EQ(mem.read16(env + 12), 0u); // screen.w -- NOT w
    EXPECT_EQ(mem.read16(env + 14), 0u); // screen.h -- NOT h
}

// Nothing in the suite composed the two functions: every PutDispEnv test
// hand-writes .screen to the reference's zeros, so SetDefDispEnv seeding it
// with w/h went unnoticed. Run them back to back on one struct, the way a
// game does, and pin the horizontal range that comes out.
//   reference: h_start = 0*10 + 0x260 = 608, screen.w == 0 -> h_end =
//              608 + 2560 = 3168 (sys.c:415-417)
//   pre-fix:   screen.w = 320 -> h_end = 608 + 3200 = 3808, clamped to 3290
// The vertical axis agrees either way (16 + 240 = 256), which hid half of it.
TEST_F(PsyqHleTest, SetDefDispEnvThenPutDispEnvUsesReferenceScreenDefaults) {
    const uint32_t env = 0x3200;
    ctx.r[A0] = env;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = 320;
    mem.write32(ctx.r[SP] + 16, 240);
    hle_SetDefDispEnv(&ctx);

    ctx.r[A0] = env;
    hle_PutDispEnv(&ctx);

    ASSERT_EQ(gp1Words.size(), 4u);
    const uint32_t gp1_06 = gp1Words[1];
    EXPECT_EQ(gp1_06 & 0xFFFu, 608u);          // h_start
    EXPECT_EQ((gp1_06 >> 12) & 0xFFFu, 3168u); // h_end

    const uint32_t gp1_07 = gp1Words[2];
    EXPECT_EQ(gp1_07 & 0x3FFu, 16u);
    EXPECT_EQ((gp1_07 >> 10) & 0x3FFu, 256u);
}

// PutDispEnv
//
// Audited 2026-08-06, amended 2026-08-06 after review, against the psyz
// decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/sys.c:399-461, retail/1MB-VRAM
// branch -- info.version 0/3, the same branch this project's PutDrawEnv audit
// established as applicable to this target). Cross-checked against psx-spx
// (docs/graphicsprocessingunitgpu.md: GP1(05h) at line 694-701, GP1(08h) at
// line 770-779, GPUSTAT mapping at 897-905 -- corrected after review; an
// earlier revision of this comment cited "line 377+", which is
// GP0(E1h) Draw Mode, a different register) and against gpu.cpp's own GP1
// consumer (case 0x05/0x08 in gpu.cpp, whose comment already states the
// correct GP1(08h) bit layout).

TEST_F(PsyqHleTest, PutDispEnvEmitsFourGP1Commands) {
    const uint32_t env = 0x4000;
    // Write a 320x240 display at (0,0)
    mem.write16(env + 0, 0);   // vx
    mem.write16(env + 2, 0);   // vy
    mem.write16(env + 4, 320); // vw
    mem.write16(env + 6, 240); // vh
    mem.write8(env + 16, 0);   // isinter
    mem.write8(env + 17, 0);   // isrgb24

    ctx.r[A0] = env;
    hle_PutDispEnv(&ctx);

    // GP1(0x05), GP1(0x06), GP1(0x07), GP1(0x08)
    ASSERT_EQ(gp1Words.size(), 4u);
    EXPECT_EQ(gp1Words[0] >> 24, 0x05u);
    EXPECT_EQ(gp1Words[1] >> 24, 0x06u);
    EXPECT_EQ(gp1Words[2] >> 24, 0x07u);
    EXPECT_EQ(gp1Words[3] >> 24, 0x08u);
}

// GP1(0x05): sys.c:409-412 (retail branch) masks BOTH disp.x and disp.y with
// 0x3FF before packing -- (disp.y & 0x3FF) << 10 | (disp.x & 0x3FF). The
// pre-audit implementation masked vx but not vy. That is latent for the
// small in-range values real callers pass, but for a large/negative disp.y
// the unmasked shift can bleed into the 0x05 command nibble itself (bits
// 24-26), corrupting the command. Pin the masking directly rather than
// relying on gpu.cpp's consumer-side re-mask to hide the producer bug.
TEST_F(PsyqHleTest, PutDispEnvMasksBothXAndYIntoDisplayStart) {
    const uint32_t env = 0x4100;
    mem.write16(env + 0, static_cast<uint16_t>(300));  // vx
    mem.write16(env + 2, static_cast<uint16_t>(-500)); // vy (out of 0-511 range)
    mem.write16(env + 4, 320);
    mem.write16(env + 6, 240);
    mem.write8(env + 16, 0);
    mem.write8(env + 17, 0);

    ctx.r[A0] = env;
    hle_PutDispEnv(&ctx);

    ASSERT_GE(gp1Words.size(), 1u);
    uint32_t gp1_05 = gp1Words[0];
    // Command nibble must stay 0x05 -- an unmasked negative vy shifted into
    // bits 10+ can otherwise corrupt bits 24-26 and change the command.
    EXPECT_EQ(gp1_05 >> 24, 0x05u);
    uint32_t x = gp1_05 & 0x3FFu;
    uint32_t y = (gp1_05 >> 10) & 0x3FFu;
    EXPECT_EQ(x, 300u);
    EXPECT_EQ(y, static_cast<uint16_t>(-500) & 0x3FFu);
}

// GP1(0x06)/(0x07): sys.c:413-427 (retail/NTSC branch, pad0=GetVideoMode()==0)
// compute the horizontal/vertical display range from DISPENV.screen (NOT
// .disp), with defaults (screen.w==0 -> 2560, screen.h==0 -> 240) and hard
// clamps. The pre-audit implementation fabricated a formula from disp.w/h
// instead (x1=0x260, x2=x1+disp.w*8; y1=0x88, y2=y1+disp.h) that does not
// match sys.c at all.
TEST_F(PsyqHleTest, PutDispEnvHorizontalVerticalRangeFromScreenDefaults) {
    const uint32_t env = 0x4200;
    mem.write16(env + 0, 0);
    mem.write16(env + 2, 0);
    mem.write16(env + 4, 320);
    mem.write16(env + 6, 240);
    mem.write16(env + 8, 0);  // screen.x
    mem.write16(env + 10, 0); // screen.y
    mem.write16(env + 12, 0); // screen.w = 0 -> default 2560 (sys.c:417)
    mem.write16(env + 14, 0); // screen.h = 0 -> default 240  (sys.c:418)
    mem.write8(env + 16, 0);
    mem.write8(env + 17, 0);

    ctx.r[A0] = env;
    hle_PutDispEnv(&ctx);

    ASSERT_EQ(gp1Words.size(), 4u);
    uint32_t gp1_06 = gp1Words[1];
    uint32_t hStart = gp1_06 & 0xFFFu;
    uint32_t hEnd = (gp1_06 >> 12) & 0xFFFu;
    // h_start = 0*10 + 0x260 = 608; h_end = 608 + 2560 = 3168 (both within clamp)
    EXPECT_EQ(hStart, 608u);
    EXPECT_EQ(hEnd, 3168u);

    uint32_t gp1_07 = gp1Words[2];
    uint32_t vStart = gp1_07 & 0x3FFu;
    uint32_t vEnd = (gp1_07 >> 10) & 0x3FFu;
    // v_start = 0 + 0x10 = 16; v_end = 16 + 240 = 256 (both within clamp)
    EXPECT_EQ(vStart, 16u);
    EXPECT_EQ(vEnd, 256u);
}

// GP1(0x08): sys.c:404, 428-457 build the mode word from isrgb24 (bit4),
// isinter (bit5), disp.w thresholds (bits 0-1 + bit6 "Hres2"), and disp.h
// (bit2+bit5 combo for >256-line modes). Matches psx-spx's documented
// GP1(08h) bit layout, i.e. the SAME layout gpu.cpp's case 0x08 comment
// already assumes. The pre-audit implementation put isrgb24 at bit5 and
// isinter at bit6 (both wrong slots) and never set the video-mode/Hres2
// bits, so the two implementations actively disagreed with each other.
TEST_F(PsyqHleTest, PutDispEnvDisplayModeBitsMatchGp1_08Layout) {
    auto modeFor = [&](int16_t w, int16_t h, uint8_t isrgb24, uint8_t isinter) {
        const uint32_t env = 0x4300;
        mem.write16(env + 0, 0);
        mem.write16(env + 2, 0);
        mem.write16(env + 4, static_cast<uint16_t>(w));
        mem.write16(env + 6, static_cast<uint16_t>(h));
        mem.write16(env + 8, 0);
        mem.write16(env + 10, 0);
        mem.write16(env + 12, 0);
        mem.write16(env + 14, 0);
        mem.write8(env + 16, isinter);
        mem.write8(env + 17, isrgb24);
        gp1Words.clear();
        ctx.r[A0] = env;
        hle_PutDispEnv(&ctx);
        return gp1Words.back();
    };

    // 256-wide, 240-tall, 15bpp, non-interlaced -> mode == 0 (no bits set)
    EXPECT_EQ(modeFor(256, 240, 0, 0) & 0xFFu, 0x00u);
    // 320-wide -> Hres1 bit0
    EXPECT_EQ(modeFor(320, 240, 0, 0) & 0xFFu, 0x01u);
    // 368-wide -> Hres2 bit6, Hres1 stays 0
    EXPECT_EQ(modeFor(368, 240, 0, 0) & 0xFFu, 0x40u);
    // 512-wide -> Hres1 bit1
    EXPECT_EQ(modeFor(512, 240, 0, 0) & 0xFFu, 0x02u);
    // 640-wide -> Hres1 bits0+1
    EXPECT_EQ(modeFor(640, 240, 0, 0) & 0xFFu, 0x03u);
    // isrgb24 -> bit4
    EXPECT_EQ(modeFor(256, 240, 1, 0) & 0xFFu, 0x10u);
    // isinter -> bit5
    EXPECT_EQ(modeFor(256, 240, 0, 1) & 0xFFu, 0x20u);
    // 480-tall -> bit2 + bit5 combo (0x24), needs interlace hardware-wise
    EXPECT_EQ(modeFor(256, 480, 0, 0) & 0xFFu, 0x24u);
    // Command nibble is always 0x08
    EXPECT_EQ(modeFor(320, 240, 0, 0) >> 24, 0x08u);
}

// Review finding (Important 1): sys.c's own `CLAMP(h_end, h_start + 0x50,
// 3290)` call (sys.c:417) can be reached with its min argument exceeding its
// max whenever the unclamped h_start is already close to the 3290 ceiling --
// reachable from screen.x >= 269, an in-range int16 value read straight out
// of game RAM (envPtr+8). CLAMP's ternary (common.h:25) is well-defined
// there (the `x < min` branch wins regardless of min-vs-max), but the
// pre-fix std::clamp() had a `!(hi < lo)` precondition and was UB on this
// input. This test exercises exactly that input and pins the well-defined,
// source-faithful (sourceClamp) result rather than crashing.
TEST_F(PsyqHleTest,
       PutDispEnvHandlesScreenXNearCeilingWithoutUndefinedBehavior) {
    const uint32_t env = 0x4400;
    mem.write16(env + 0, 0);
    mem.write16(env + 2, 0);
    mem.write16(env + 4, 320);
    mem.write16(env + 6, 240);
    mem.write16(
        env + 8,
        269);                 // screen.x -- unclamped h_start = 269*10+0x260 = 3298 > 3290
    mem.write16(env + 10, 0); // screen.y
    mem.write16(env + 12, 1); // screen.w = 1 -> unclamped h_end = 3298+10 = 3308
    mem.write16(env + 14, 0); // screen.h = 0 -> default
    mem.write8(env + 16, 0);
    mem.write8(env + 17, 0);

    ctx.r[A0] = env;
    hle_PutDispEnv(&ctx); // must not abort/UB

    ASSERT_EQ(gp1Words.size(), 4u);
    uint32_t gp1_06 = gp1Words[1];
    uint32_t hStart = gp1_06 & 0xFFFu;
    uint32_t hEnd = (gp1_06 >> 12) & 0xFFFu;
    // h_start: 3298 > 3290 -> clamped to 3290.
    EXPECT_EQ(hStart, 3290u);
    // h_end: unclamped 3308 < (clamped h_start + 0x50) = 3370, so CLAMP's
    // `x < min` branch returns min = 3370 -- which is itself > the 3290 max,
    // matching sys.c's own CLAMP macro exactly (bug-compatible by design).
    EXPECT_EQ(hEnd, 3370u);
}

// Review finding (Important 3): sys.c branches on GetVideoMode() (env->pad0)
// for the v_start offset (0x10/0x13), the v-range clamp ceiling (256,258 /
// 310,312), the video-mode bit (GP1(08h) bit3), and the Vres/interlace
// height threshold (256/288). A pre-amendment revision of this HLE assumed
// NTSC unconditionally; PAL is reachable via libetc_SetVideoMode
// (hle_libgpu_SetVideoMode, psyq_libgpu.cpp), already round-tripped by
// PsyqGpuTest.SetGetVideoModeRoundTrip (test_psyq_gpu.cpp).
TEST_F(PsyqHleTest, PutDispEnvHonoursPalVideoModeForRangeAndModeBit) {
    ctx.r[A0] = 1; // PAL
    hle_libgpu_SetVideoMode(&ctx);

    const uint32_t env = 0x4500;
    mem.write16(env + 0, 0);
    mem.write16(env + 2, 0);
    mem.write16(env + 4, 320);
    mem.write16(env + 6,
                280);         // disp.h: > 256 (NTSC threshold) but <= 288 (PAL threshold)
    mem.write16(env + 8, 0);  // screen.x
    mem.write16(env + 10, 0); // screen.y
    mem.write16(env + 12, 0); // screen.w = 0 -> default
    mem.write16(env + 14, 0); // screen.h = 0 -> default
    mem.write8(env + 16, 0);
    mem.write8(env + 17, 0);

    ctx.r[A0] = env;
    hle_PutDispEnv(&ctx);

    ASSERT_EQ(gp1Words.size(), 4u);
    uint32_t gp1_07 = gp1Words[2];
    uint32_t vStart = gp1_07 & 0x3FFu;
    // v_start = screen.y + 0x13 (PAL offset, not the NTSC 0x10) = 19.
    EXPECT_EQ(vStart, 19u);

    uint32_t mode = gp1Words[3] & 0xFFu;
    // Video mode bit (bit3) must be set for PAL.
    EXPECT_EQ(mode & 0x08u, 0x08u);
    // disp.h=280 is <= the PAL threshold (288), so the Vres/interlace combo
    // (bit2+bit5) must NOT be set here, unlike the NTSC threshold (256)
    // which PutDispEnvDisplayModeBitsMatchGp1_08Layout already pins at h=480.
    EXPECT_EQ(mode & 0x24u, 0u);

    // Restore to NTSC so other tests start clean (same pattern as
    // PsyqGpuTest.SetGetVideoModeRoundTrip).
    ctx.r[A0] = 0;
    hle_libgpu_SetVideoMode(&ctx);
}

// `DISPENV* PutDispEnv(DISPENV* env)` returns env (sys.c:399, 460). The HLE
// left V0 untouched, so a caller doing `disp = PutDispEnv(disp)` picked up
// whatever the previous call left in the register.
TEST_F(PsyqHleTest, PutDispEnvReturnsEnvPointer) {
    const uint32_t env = 0x4600;
    ctx.r[A0] = env;
    ctx.r[V0] = 0xDEADBEEFu;
    hle_PutDispEnv(&ctx);
    EXPECT_EQ(ctx.r[V0], env);
}

// SetDefDrawEnv
//
// Audited 2026-08-06 against the psyz decomp reference (workspace clone,
// PS1Recomp-workspace/psyz/decomp/src/libgpu/ext.c:46-69,
// `DRAWENV* SetDefDrawEnv(DRAWENV* env, int x, int y, int w, int h)`):
//   env->tpage = getTPage(0, 0, 640, 0)      -- NOT 0
//   env->dfe   = video_mode ? h<=288 : h<=256 -- NOT always 0
// This project has no PAL path (GetVideoMode() defaults to, and stays, 0 /
// NTSC -- see hle_libgpu_GetVideoMode), so the NTSC branch (h<=256) applies.
// getTPage(0,0,640,0), using this project's own already-audited GetTPage
// formula (psyq_libgpu.cpp, cited in the SetDrawMode audit), evaluates to
// (640 & 0x3FF) >> 6 = 0x0A -- the pre-audit implementation hardcoded 0.

TEST_F(PsyqHleTest, SetDefDrawEnvWritesTexturePageFromGetTPage640Origin) {
    const uint32_t env = 0x5100;
    ctx.r[A0] = env;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = 320;
    mem.write32(ctx.r[SP] + 16, 240);

    hle_SetDefDrawEnv(&ctx);

    // ext.c:67 -- env->tpage = getTPage(0, 0, 640, 0) = 0x0A, independent of
    // the x/y/w/h arguments passed to SetDefDrawEnv itself.
    EXPECT_EQ(mem.read16(env + 20), 0x0Au);
}

TEST_F(PsyqHleTest, SetDefDrawEnvSetsDfeFromHeightNtscThreshold) {
    // ext.c:60-64 -- NTSC branch: dfe = (h <= 256).
    const uint32_t envShort = 0x5200;
    ctx.r[A0] = envShort;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = 320;
    mem.write32(ctx.r[SP] + 16, 240); // h = 240 <= 256 -> dfe = 1
    hle_SetDefDrawEnv(&ctx);
    EXPECT_EQ(mem.read8(envShort + 23), 1u);

    const uint32_t envTall = 0x5300;
    ctx.r[A0] = envTall;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = 320;
    mem.write32(ctx.r[SP] + 16, 480); // h = 480 > 256 -> dfe = 0
    hle_SetDefDrawEnv(&ctx);
    EXPECT_EQ(mem.read8(envTall + 23), 0u);
}

// Review finding (Important 3): ext.c:60-64 branches on GetVideoMode() for
// the dfe threshold (video_mode ? h<=288 : h<=256). A pre-amendment revision
// assumed NTSC unconditionally; PAL is reachable via libetc_SetVideoMode.
TEST_F(PsyqHleTest, SetDefDrawEnvSetsDfeFromHeightPalThreshold) {
    ctx.r[A0] = 1; // PAL
    hle_libgpu_SetVideoMode(&ctx);

    const uint32_t env = 0x5400;
    ctx.r[A0] = env;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = 320;
    // h = 280: > NTSC threshold (256) but <= PAL threshold (288) -- if the
    // video-mode branch were still dropped this would wrongly read dfe = 0.
    mem.write32(ctx.r[SP] + 16, 280);
    hle_SetDefDrawEnv(&ctx);
    EXPECT_EQ(mem.read8(env + 23), 1u);

    // Restore to NTSC so other tests start clean.
    ctx.r[A0] = 0;
    hle_libgpu_SetVideoMode(&ctx);
}

TEST_F(PsyqHleTest, SetDefDrawEnvWritesClipAndOffset) {
    const uint32_t env = 0x5000;
    ctx.r[A0] = env;
    ctx.r[A1] = 0;                    // x
    ctx.r[A2] = 0;                    // y
    ctx.r[A3] = 320;                  // w
    mem.write32(ctx.r[SP] + 16, 240); // h

    hle_SetDefDrawEnv(&ctx);

    EXPECT_EQ(mem.read16(env + 0), 0u);   // clip.x
    EXPECT_EQ(mem.read16(env + 2), 0u);   // clip.y
    EXPECT_EQ(mem.read16(env + 4), 320u); // clip.w
    EXPECT_EQ(mem.read16(env + 6), 240u); // clip.h
    EXPECT_EQ(mem.read16(env + 8), 0u);   // ofs.x = clip.x
    EXPECT_EQ(mem.read16(env + 10), 0u);  // ofs.y = clip.y
    EXPECT_EQ(mem.read8(env + 22), 1u);   // dtd = 1 (dithering on by default)
    // dfe = h<=256 on the NTSC branch (ext.c:60-64); h=240 here -> dfe=1.
    // (Corrected 2026-08-06: this assertion previously pinned the pre-audit
    // implementation's hardcoded dfe=0, which did not match the source.)
    EXPECT_EQ(mem.read8(env + 23), 1u);
    EXPECT_EQ(ctx.r[V0], env);
}

// ext.c:56-58 and ext.c:68 also zero the last four bytes of the struct:
// `env->r0 = 0; env->g0 = 0; env->b0 = 0;` and `env->isbg = 0`. Offsets from
// the DRAWENV layout (chrono-cross-decomp/include/psyq/libgpu.h:361-371 --
// isbg at :368 (+24), r0/g0/b0 at :369 (+25..+27)), the same layout the
// tpage/dtd/dfe assertions above follow. The
// implementation wrote +0..+23 and stopped, so a stack-allocated DRAWENV kept
// stack garbage in the auto-clear flag and its background colour. Poison
// first -- Memory starts zeroed, so an unpoisoned struct would pass either
// way.
TEST_F(PsyqHleTest, SetDefDrawEnvZeroesIsbgAndBackgroundColour) {
    const uint32_t env = 0x5100;
    mem.write8(env + 24, 0xAAu); // isbg
    mem.write8(env + 25, 0xBBu); // r0
    mem.write8(env + 26, 0xCCu); // g0
    mem.write8(env + 27, 0xDDu); // b0

    ctx.r[A0] = env;
    ctx.r[A1] = 0;
    ctx.r[A2] = 0;
    ctx.r[A3] = 320;
    mem.write32(ctx.r[SP] + 16, 240);

    hle_SetDefDrawEnv(&ctx);

    EXPECT_EQ(mem.read8(env + 24), 0u); // isbg
    EXPECT_EQ(mem.read8(env + 25), 0u); // r0
    EXPECT_EQ(mem.read8(env + 26), 0u); // g0
    EXPECT_EQ(mem.read8(env + 27), 0u); // b0
}

// PutDrawEnv

TEST_F(PsyqHleTest, PutDrawEnvEmitsGP0Commands) {
    const uint32_t env = 0x6000;
    // 320x240 draw area at (0,0), no dithering
    mem.write16(env + 0, 0);   // clip.x
    mem.write16(env + 2, 0);   // clip.y
    mem.write16(env + 4, 320); // clip.w
    mem.write16(env + 6, 240); // clip.h
    mem.write16(env + 8, 0);   // ofs.x
    mem.write16(env + 10, 0);  // ofs.y
    mem.write16(env + 20, 0);  // tpage
    mem.write8(env + 22, 0);   // dtd = 0

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    // Expect GP0(0xE3), GP0(0xE4), GP0(0xE5), GP0(0xE1), GP0(0xE2), GP0(0xE6),
    // in SetDrawEnv2's order (sys.c:561-575); see
    // PutDrawEnvEmitsWordsInSetDrawEnv2Order below for the order rationale,
    // and PutDrawEnvTextureWindowEncoding for the GP0(0xE2) bit layout.
    ASSERT_EQ(gp0Words.size(), 6u);
    EXPECT_EQ(gp0Words[0] >> 24, 0xE3u); // draw area top-left
    EXPECT_EQ(gp0Words[1] >> 24, 0xE4u); // draw area bottom-right
    EXPECT_EQ(gp0Words[2] >> 24, 0xE5u); // draw offset
    EXPECT_EQ(gp0Words[3] >> 24, 0xE1u); // tpage
    EXPECT_EQ(gp0Words[4] >> 24, 0xE2u); // texture window
    EXPECT_EQ(gp0Words[5] >> 24, 0xE6u); // mask bit setting
}

// SetDrawEnv2 (sys.c:561-575) pushes GP0 words in this exact order:
//   get_cs -> E3, get_ce -> E4, get_ofs -> E5, get_mode -> E1, get_tw -> E2,
//   literal 0xE6000000 -> E6.
// On real hardware this order matters: the GPU applies each attribute as its
// word arrives, so E1 (draw mode) landing after E3/E4 (clip area) means the
// draw mode takes effect only once the clip area is already set, not before.
// The pre-fix implementation emitted E1 first (E1, E3, E4, E5) and never
// emitted E2/E6 at all.
TEST_F(PsyqHleTest, PutDrawEnvEmitsWordsInSetDrawEnv2Order) {
    const uint32_t env = 0x7300;
    mem.write16(env + 0, 0);
    mem.write16(env + 2, 0);
    mem.write16(env + 4, 320);
    mem.write16(env + 6, 240);
    mem.write16(env + 8, 0);
    mem.write16(env + 10, 0);
    mem.write16(env + 20, 0);
    mem.write8(env + 22, 0);

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    ASSERT_EQ(gp0Words.size(), 6u);
    EXPECT_EQ(gp0Words[0] >> 24, 0xE3u) << "get_cs first";
    EXPECT_EQ(gp0Words[1] >> 24, 0xE4u) << "get_ce second";
    EXPECT_EQ(gp0Words[2] >> 24, 0xE5u) << "get_ofs third";
    EXPECT_EQ(gp0Words[3] >> 24, 0xE1u) << "get_mode fourth";
    EXPECT_EQ(gp0Words[4] >> 24, 0xE2u) << "get_tw fifth";
    EXPECT_EQ(gp0Words[5] >> 24, 0xE6u)
        << "literal 0xE6000000 last, per sys.c:561-575";
}

// GP0(0xE2): sys.c's get_tw (662-673), called as get_tw(&env->tw) -- always
// the non-NULL branch here, since env->tw is a struct field, not a pointer:
//   code[0] = (rect->x & 0xFF) >> 3;       code[2] = (-rect->w & 0xFF) >> 3;
//   code[1] = (rect->y & 0xFF) >> 3;       code[3] = (-rect->h & 0xFF) >> 3;
//   return 0xE2000000 | (code[1]<<15) | (code[0]<<10) | (code[3]<<5) | code[2];
// env->tw is the RECT at DRAWENV offset +12 (x,y,w,h, each int16 -- the same
// offset this file's SetDefDrawEnv tests already exercise). Cross-checked
// against psx-spx's documented GP0(E2h) bit layout (workspace clone
// PS1Recomp-workspace/psx-spx.github.io/docs/graphicsprocessingunitgpu.md:
// 406-414): bits 0-4 Mask X, 5-9 Mask Y, 10-14 Offset X, 15-19 Offset Y --
// matching code[2]@0-4, code[3]@5-9, code[0]@10-14, code[1]@15-19 above. The
// pre-fix implementation never emitted this word at all.
TEST_F(PsyqHleTest, PutDrawEnvTextureWindowEncoding) {
    const uint32_t env = 0x7400;
    mem.write16(env + 0, 0);
    mem.write16(env + 2, 0);
    mem.write16(env + 4, 320);
    mem.write16(env + 6, 240);
    mem.write16(env + 8, 0);
    mem.write16(env + 10, 0);
    mem.write16(env + 12, 8);  // tw.x
    mem.write16(env + 14, 16); // tw.y
    mem.write16(env + 16, 32); // tw.w
    mem.write16(env + 18, 64); // tw.h
    mem.write16(env + 20, 0);
    mem.write8(env + 22, 0);

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    ASSERT_EQ(gp0Words.size(), 6u);
    // code[0]=(8&0xFF)>>3=1, code[1]=(16&0xFF)>>3=2,
    // code[2]=(-32&0xFF)>>3=28, code[3]=(-64&0xFF)>>3=24
    // -> 0xE2000000 | (2<<15) | (1<<10) | (24<<5) | 28 = 0xE201071C
    EXPECT_EQ(gp0Words[4], 0xE201071Cu);
}

// GP0(0xE6): SetDrawEnv2 (sys.c:575) always pushes the literal 0xE6000000 --
// no computation, no dependency on any DRAWENV field. Confirmed against
// psx-spx's documented GP0(E6h) layout (graphicsprocessingunitgpu.md:463-468):
// bits 0-1 are Set-mask-while-drawing / Check-mask-before-draw, both 0 here.
TEST_F(PsyqHleTest, PutDrawEnvMaskBitSettingIsAlwaysLiteralE6) {
    const uint32_t env = 0x7500;
    mem.write16(env + 0, 0);
    mem.write16(env + 2, 0);
    mem.write16(env + 4, 320);
    mem.write16(env + 6, 240);
    mem.write16(env + 8, 0);
    mem.write16(env + 10, 0);
    mem.write16(env + 20, 0);
    mem.write8(env + 22, 0);

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    ASSERT_EQ(gp0Words.size(), 6u);
    EXPECT_EQ(gp0Words[5], 0xE6000000u);
}

TEST_F(PsyqHleTest, PutDrawEnvBottomRightEncoding) {
    const uint32_t env = 0x7000;
    // 256x240 draw area at (0,0)
    mem.write16(env + 0, 0);
    mem.write16(env + 2, 0);
    mem.write16(env + 4, 256); // w
    mem.write16(env + 6, 240); // h
    mem.write16(env + 8, 0);
    mem.write16(env + 10, 0);
    mem.write16(env + 20, 0);
    mem.write8(env + 22, 0);

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    // GP0(0xE4): bottom-right = (w-1, h-1) = (255, 239). Index 1: emission
    // order is E3, E4, E5, E1 (sys.c:561-575).
    // bits: [19:10]=y, [9:0]=x
    uint32_t e4 = gp0Words[1];
    uint32_t bx = e4 & 0x3FF;
    uint32_t by = (e4 >> 10) & 0x1FF;
    EXPECT_EQ(bx, 255u);
    EXPECT_EQ(by, 239u);
}

// GP0(0xE3)/(0xE4): sys.c's get_cs (634-641) and get_ce (643-651), retail
// branch, both do `x = CLAMP(x, 0, info.w - 1); y = CLAMP(y, 0, info.h - 1);`
// before packing `(y & 0x3FF) << 10 | (x & 0x3FF)`, where info.w/info.h are
// 1024/512 for info.version 0/3 (sys.c:107-108, 170-171 -- the retail/
// 1MB-VRAM branch this project's SetDrawMode/PutDrawEnv audits already use).
// The pre-fix implementation neither clamped nor masked y correctly (0x1FF
// instead of 0x3FF), so an out-of-range clip rect leaked raw, unclamped bits
// into the GP0 words instead of being pinned to the VRAM edge like real
// hardware. clip.x/y here are negative and clip.w/h push the bottom-right
// corner past both VRAM edges, so every one of those bugs is reachable in a
// single call.
TEST_F(PsyqHleTest, PutDrawEnvClampsClipAreaToVramBounds) {
    const uint32_t env = 0x7100;
    mem.write16(env + 0, static_cast<uint16_t>(-10)); // clip.x (< 0)
    mem.write16(env + 2, static_cast<uint16_t>(-5));  // clip.y (< 0)
    mem.write16(env + 4, 2000);                       // clip.w -- pushes x2 = -10+2000-1 = 1989 > 1023
    mem.write16(env + 6, 2000);                       // clip.h -- pushes y2 = -5+2000-1  = 1994 > 511
    mem.write16(env + 8, 0);
    mem.write16(env + 10, 0);
    mem.write16(env + 20, 0);
    mem.write8(env + 22, 0);

    ctx.r[A0] = env;
    hle_PutDrawEnv(&ctx);

    ASSERT_GE(gp0Words.size(), 2u);
    // Emission order is E3, E4, E5, E1 (sys.c:561-575).
    uint32_t e3 = gp0Words[0];
    uint32_t e4 = gp0Words[1];
    // get_cs(-10, -5) clamps both to 0 -> top-left pinned to VRAM origin.
    EXPECT_EQ(e3 & 0x3FFu, 0u);
    EXPECT_EQ((e3 >> 10) & 0x3FFu, 0u);
    // get_ce(1989, 1994) clamps to (info.w-1, info.h-1) = (1023, 511).
    EXPECT_EQ(e4 & 0x3FFu, 1023u);
    EXPECT_EQ((e4 >> 10) & 0x3FFu, 511u);
}

// `DRAWENV* PutDrawEnv(DRAWENV* env)` returns env (sys.c:362, 372). The HLE
// left V0 untouched, so a caller doing `draw = PutDrawEnv(draw)` picked up
// whatever the previous call left in the register.
TEST_F(PsyqHleTest, PutDrawEnvReturnsEnvPointer) {
    const uint32_t env = 0x6600;
    ctx.r[A0] = env;
    ctx.r[V0] = 0xDEADBEEFu;
    hle_PutDrawEnv(&ctx);
    EXPECT_EQ(ctx.r[V0], env);
}
