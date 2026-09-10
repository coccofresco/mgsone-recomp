#include "runtime/mips_value_flow.h"

#include "runtime/cpu_context.h"
#include "runtime/gte.h"
#include "runtime/metrics.h"
#include "runtime/mips_fingerprint.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps1::mips {
namespace {

struct TrackedValue {
  MipsValueExpressionId expression = 0;
  uint32_t value = 0;
};

struct StoredValue {
  uint32_t address = 0;
  uint8_t width = 0;
  uint32_t value = 0;
  MipsValueExpressionId expression = 0;
};

struct PendingInstruction {
  bool valid = false;
  CPUContext *ctx = nullptr;
  uint32_t pc = 0;
  uint32_t raw = 0;
  uint32_t rsValue = 0;
  uint32_t rtValue = 0;
  MipsValueExpressionId rsExpression = 0;
  MipsValueExpressionId rtExpression = 0;
  MipsValueExpressionId hiExpression = 0;
  MipsValueExpressionId loExpression = 0;
  uint32_t effectiveAddress = 0;
  std::array<MipsValueExpressionId, 16> gteDependencies{};
  uint8_t gteDependencyCount = 0;
};

struct ValueFlowState {
  CPUContext *ctx = nullptr;
  bool active = false;
  uint32_t entryPc = 0;
  std::vector<MipsValueExpression> expressions;
  std::unordered_map<MipsValueExpressionId, std::size_t> expressionIndices;
  std::array<TrackedValue, 32> gpr{};
  TrackedValue hi{};
  TrackedValue lo{};
  std::array<TrackedValue, 32> gteData{};
  std::array<TrackedValue, 32> gteControl{};
  std::unordered_map<uint64_t, StoredValue> storedValues;
  std::unordered_set<MipsValueExpressionId> emitted;
  PendingInstruction pending{};
  uint64_t observedControlWriteSerial = 0;
};

std::atomic<int> g_enabled{-1};
std::atomic<int> g_frameBoundsResolved{0};
std::atomic<uint64_t> g_frameFrom{0};
std::atomic<uint64_t> g_frameTo{std::numeric_limits<uint64_t>::max()};
std::atomic<MipsValueExpressionId> g_nextExpression{1};
thread_local ValueFlowState t_state;

constexpr uint32_t kNoAddress = std::numeric_limits<uint32_t>::max();

uint64_t parseFrameBound(const char *name, uint64_t fallback) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    return fallback;
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  return end != value && *end == '\0' ? static_cast<uint64_t>(parsed)
                                      : fallback;
}

int32_t signExtend16(uint32_t raw) {
  return static_cast<int16_t>(raw & 0xffffu);
}

uint32_t registerValue(const CPUContext *ctx, uint8_t reg) {
  return reg == 0 ? 0 : ctx->r[reg & 31u];
}

uint32_t physicalAddress(uint32_t address) { return address & 0x1fffffffu; }

uint64_t storageKey(uint32_t address, uint8_t width) {
  return (static_cast<uint64_t>(physicalAddress(address)) << 8) | width;
}

MipsValueExpression *findExpression(MipsValueExpressionId id) {
  if (id == 0)
    return nullptr;
  const auto found = t_state.expressionIndices.find(id);
  return found == t_state.expressionIndices.end()
             ? nullptr
             : &t_state.expressions[found->second];
}

const MipsValueExpression *findExpressionConst(MipsValueExpressionId id) {
  return findExpression(id);
}

bool expressionComplete(MipsValueExpressionId id) {
  const MipsValueExpression *expression = findExpressionConst(id);
  return expression != nullptr && expression->complete;
}

uint64_t expressionFingerprint(const MipsValueExpression &expression,
                               bool includeMemoryAddress) {
  const MipsValueExpression *lhs = findExpressionConst(expression.lhs);
  const MipsValueExpression *rhs = findExpressionConst(expression.rhs);
  const fingerprint::Pair pair = fingerprint::expression(
      expression.operation, expression.pc, expression.rawInstruction,
      expression.value, expression.lhs != 0,
      {lhs ? lhs->sourceFingerprint : 0, lhs ? lhs->programFingerprint : 0,
       lhs != nullptr && lhs->complete},
      expression.rhs != 0,
      {rhs ? rhs->sourceFingerprint : 0, rhs ? rhs->programFingerprint : 0,
       rhs != nullptr && rhs->complete},
      expression.immediate, expression.address, expression.width,
      expression.signedValue, expression.complete);
  return includeMemoryAddress ? pair.source : pair.program;
}

MipsValueExpressionId
addExpression(MipsValueOperation operation, uint32_t pc, uint32_t raw,
              uint32_t value, MipsValueExpressionId lhs = 0,
              MipsValueExpressionId rhs = 0, int64_t immediate = 0,
              uint32_t address = kNoAddress, uint8_t width = 0,
              bool signedValue = false, bool complete = false) {
  const MipsValueExpressionId id =
      g_nextExpression.fetch_add(1, std::memory_order_relaxed);
  MipsValueExpression expression{id,      operation, pc,          raw,
                                 value,   lhs,       rhs,         immediate,
                                 address, width,     signedValue, complete};
  if (complete) {
    expression.sourceFingerprint = expressionFingerprint(expression, true);
    expression.programFingerprint = expressionFingerprint(expression, false);
  }
  t_state.expressions.push_back(expression);
  t_state.expressionIndices.emplace(id, t_state.expressions.size() - 1u);
  return id;
}

MipsValueExpressionId addBoundary(uint32_t value, uint32_t pc,
                                  MipsValueBoundary boundary, uint32_t slot) {
  return addExpression(MipsValueOperation::Boundary, pc, 0, value, 0, 0,
                       static_cast<int64_t>(boundary), slot, 0, false, false);
}

MipsValueExpressionId addConstant(uint32_t value, uint32_t pc,
                                  uint32_t raw = 0) {
  return addExpression(MipsValueOperation::Constant, pc, raw, value, 0, 0,
                       static_cast<int64_t>(value), kNoAddress, 0, false, true);
}

MipsValueExpressionId addUnary(MipsValueOperation operation, uint32_t pc,
                               uint32_t raw, uint32_t value,
                               MipsValueExpressionId source,
                               int64_t immediate = 0) {
  return addExpression(operation, pc, raw, value, source, 0, immediate,
                       kNoAddress, 0, false, expressionComplete(source));
}

MipsValueExpressionId addBinary(MipsValueOperation operation, uint32_t pc,
                                uint32_t raw, uint32_t value,
                                MipsValueExpressionId lhs,
                                MipsValueExpressionId rhs,
                                int64_t immediate = 0) {
  return addExpression(operation, pc, raw, value, lhs, rhs, immediate,
                       kNoAddress, 0, false,
                       expressionComplete(lhs) && expressionComplete(rhs));
}

TrackedValue boundaryValue(uint32_t value, uint32_t pc,
                           MipsValueBoundary boundary, uint32_t slot) {
  return {addBoundary(value, pc, boundary, slot), value};
}

TrackedValue &validatedGpr(CPUContext *ctx, uint8_t reg, uint32_t pc) {
  reg &= 31u;
  TrackedValue &tracked = t_state.gpr[reg];
  const uint32_t current = registerValue(ctx, reg);
  if (tracked.expression == 0 || tracked.value != current) {
    tracked = reg == 0
                  ? TrackedValue{addConstant(0, pc), 0}
                  : boundaryValue(current, pc,
                                  MipsValueBoundary::UntrackedMutation, reg);
  }
  return tracked;
}

TrackedValue &validatedSpecial(TrackedValue &tracked, uint32_t current,
                               uint32_t pc, uint32_t slot) {
  if (tracked.expression == 0 || tracked.value != current)
    tracked =
        boundaryValue(current, pc, MipsValueBoundary::UntrackedMutation, slot);
  return tracked;
}

TrackedValue &validatedGte(TrackedValue &tracked, uint32_t current, uint32_t pc,
                           uint32_t slot) {
  if (tracked.expression == 0 || tracked.value != current)
    tracked =
        boundaryValue(current, pc, MipsValueBoundary::ExistingGteState, slot);
  return tracked;
}

void assignGpr(CPUContext *ctx, uint8_t reg, MipsValueExpressionId expression) {
  reg &= 31u;
  if (reg == 0)
    return;
  t_state.gpr[reg] = {expression, ctx->r[reg]};
}

void invalidateOverlappingStorage(uint32_t address, uint8_t width) {
  const uint32_t first = physicalAddress(address);
  const uint32_t last = first + std::max<uint8_t>(width, 1u) - 1u;
  for (auto iterator = t_state.storedValues.begin();
       iterator != t_state.storedValues.end();) {
    const StoredValue &stored = iterator->second;
    const uint32_t storedFirst = physicalAddress(stored.address);
    const uint32_t storedLast = storedFirst + stored.width - 1u;
    if (first <= storedLast && storedFirst <= last)
      iterator = t_state.storedValues.erase(iterator);
    else
      ++iterator;
  }
}

void rememberStore(uint32_t address, uint8_t width, uint32_t value,
                   MipsValueExpressionId expression) {
  invalidateOverlappingStorage(address, width);
  t_state.storedValues.emplace(storageKey(address, width),
                               StoredValue{address, width, value, expression});
}

MipsValueExpressionId addMemoryLoad(uint32_t pc, uint32_t raw, uint32_t address,
                                    uint8_t width, bool signedValue,
                                    uint32_t value) {
  MipsValueExpressionId storedExpression = 0;
  const auto found = t_state.storedValues.find(storageKey(address, width));
  if (found != t_state.storedValues.end() && found->second.value == value)
    storedExpression = found->second.expression;
  const bool complete =
      storedExpression == 0 || expressionComplete(storedExpression);
  return addExpression(MipsValueOperation::MemoryLoad, pc, raw, value,
                       storedExpression, 0, 0, address, width, signedValue,
                       complete);
}

MipsValueExpressionId wrapGteWrite(MipsValueOperation operation, uint32_t pc,
                                   uint32_t raw, uint32_t value,
                                   MipsValueExpressionId source,
                                   uint8_t gteRegister) {
  return addExpression(operation, pc, raw, value, source, 0, gteRegister,
                       kNoAddress, 0, false, expressionComplete(source));
}

void emitExpressionTree(MipsValueExpressionId root) {
  if (root == 0)
    return;
  std::vector<MipsValueExpressionId> pending{root};
  std::vector<MipsValueExpressionId> order;
  std::unordered_set<MipsValueExpressionId> visited;
  while (!pending.empty()) {
    const MipsValueExpressionId id = pending.back();
    pending.pop_back();
    if (!visited.insert(id).second || t_state.emitted.count(id) != 0)
      continue;
    const MipsValueExpression *expression = findExpressionConst(id);
    if (expression == nullptr)
      continue;
    order.push_back(id);
    if (expression->lhs != 0)
      pending.push_back(expression->lhs);
    if (expression->rhs != 0)
      pending.push_back(expression->rhs);
  }

  for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator) {
    const MipsValueExpression *expression = findExpressionConst(*iterator);
    if (expression == nullptr || !t_state.emitted.insert(*iterator).second)
      continue;
    metrics::trace(
        "mips.value_expr",
        {{"expression_id", static_cast<int64_t>(expression->id)},
         {"op", static_cast<int64_t>(expression->operation)},
         {"pc", expression->pc},
         {"raw", expression->rawInstruction},
         {"value", expression->value},
         {"lhs", static_cast<int64_t>(expression->lhs)},
         {"rhs", static_cast<int64_t>(expression->rhs)},
         {"immediate", expression->immediate},
         {"address", expression->address == kNoAddress
                         ? -1
                         : static_cast<int64_t>(expression->address)},
         {"width", expression->width},
         {"signed", expression->signedValue ? 1 : 0},
         {"complete", expression->complete ? 1 : 0}});
  }
}

} // namespace

bool valueFlowEnabled() noexcept {
  int enabled = g_enabled.load(std::memory_order_acquire);
  if (enabled == 2)
    return true;
  if (enabled < 0) {
    const char *value = std::getenv("PS1_TRACE_MIPS_VALUE_FLOW");
    enabled = value != nullptr && *value != '\0' && *value != '0' ? 1 : 0;
    g_enabled.store(enabled, std::memory_order_release);
  }
  if (enabled != 1 || !metrics::traceEnabled() || !metrics::guestTraceArmed() ||
      metrics::projectionTraceComplete())
    return false;

  // A semantic GTE trigger is a stronger boundary than a host/VBlank window.
  // Keep the expensive interpreter sidecar completely dormant until the same
  // PC has armed GTE capture; otherwise tracing work earlier in that VBlank
  // can move animation/camera state relative to the light timing oracle. Once
  // armed, the next top-level interpreter slice starts the exact DAG. The GTE
  // producer also turns false after the matched GP0 submission, preserving the
  // existing bounded-stop contract.
  static const bool semanticGteGate = [] {
    const char *value = std::getenv("PS1_TRACE_GTE_TRIGGER_PC");
    return value != nullptr && *value != '\0';
  }();
  if (semanticGteGate && !metrics::gteTraceEnabled())
    return false;

  if (g_frameBoundsResolved.load(std::memory_order_acquire) == 0) {
    g_frameFrom.store(
        parseFrameBound("PS1_TRACE_MIPS_VALUE_FLOW_FRAME_FROM", 0),
        std::memory_order_relaxed);
    g_frameTo.store(parseFrameBound("PS1_TRACE_MIPS_VALUE_FLOW_FRAME_TO",
                                    std::numeric_limits<uint64_t>::max()),
                    std::memory_order_relaxed);
    g_frameBoundsResolved.store(1, std::memory_order_release);
  }
  const uint64_t frame = metrics::traceFrame();
  return frame >= g_frameFrom.load(std::memory_order_relaxed) &&
         frame <= g_frameTo.load(std::memory_order_relaxed);
}

void setValueFlowEnabledForTesting(bool enabled) noexcept {
  g_enabled.store(enabled ? 2 : 0, std::memory_order_release);
}

void resetValueFlowForTesting() noexcept {
  t_state = {};
  g_enabled.store(-1, std::memory_order_release);
  g_frameBoundsResolved.store(0, std::memory_order_relaxed);
  g_frameFrom.store(0, std::memory_order_relaxed);
  g_frameTo.store(std::numeric_limits<uint64_t>::max(),
                  std::memory_order_relaxed);
  g_nextExpression.store(1, std::memory_order_relaxed);
}

void beginValueFlowSlice(recomp_context *ctx, uint32_t entryPc) {
  if (!valueFlowEnabled() || ctx == nullptr)
    return;

  // GTE input/control registers persist across guest calls. Retain only their
  // reachable expression DAG, not the full instruction history. A changed
  // control-write serial means an untracked writer (including state restore)
  // crossed the boundary, so all old control provenance is discarded even if
  // the numeric register values happen to match.
  const bool preserveGte =
      t_state.ctx == ctx && !t_state.expressions.empty() &&
      t_state.observedControlWriteSerial == ctx->gteControlWriteSerial;
  const std::array<TrackedValue, 32> previousData = t_state.gteData;
  const std::array<TrackedValue, 32> previousControl = t_state.gteControl;
  std::unordered_set<MipsValueExpressionId> retainedIds;
  std::vector<MipsValueExpressionId> pendingIds;
  if (preserveGte) {
    for (const TrackedValue &data : previousData)
      pendingIds.push_back(data.expression);
    for (const TrackedValue &control : previousControl)
      pendingIds.push_back(control.expression);
  }
  while (!pendingIds.empty()) {
    const MipsValueExpressionId id = pendingIds.back();
    pendingIds.pop_back();
    if (id == 0 || !retainedIds.insert(id).second)
      continue;
    const MipsValueExpression *expression = findExpressionConst(id);
    if (expression == nullptr) {
      retainedIds.erase(id);
      continue;
    }
    pendingIds.push_back(expression->lhs);
    pendingIds.push_back(expression->rhs);
  }
  std::vector<MipsValueExpression> retainedExpressions;
  if (preserveGte) {
    retainedExpressions.reserve(retainedIds.size());
    for (const MipsValueExpression &expression : t_state.expressions) {
      if (retainedIds.count(expression.id) != 0)
        retainedExpressions.push_back(expression);
    }
  }
  std::unordered_set<MipsValueExpressionId> emitted =
      preserveGte ? std::move(t_state.emitted)
                  : std::unordered_set<MipsValueExpressionId>{};

  t_state = {};
  t_state.ctx = ctx;
  t_state.active = true;
  t_state.entryPc = entryPc;
  t_state.expressions = std::move(retainedExpressions);
  for (std::size_t index = 0; index < t_state.expressions.size(); ++index)
    t_state.expressionIndices.emplace(t_state.expressions[index].id, index);
  t_state.emitted = std::move(emitted);
  t_state.observedControlWriteSerial = ctx->gteControlWriteSerial;
  for (uint8_t reg = 0; reg < 32; ++reg) {
    const uint32_t value = registerValue(ctx, reg);
    t_state.gpr[reg] =
        reg == 0 ? TrackedValue{addConstant(0, entryPc), 0}
                 : boundaryValue(value, entryPc,
                                 MipsValueBoundary::InterpreterEntry, reg);
    const bool preserveDataRegister =
        preserveGte && previousData[reg].value == ctx->cop2d[reg] &&
        findExpressionConst(previousData[reg].expression) != nullptr;
    t_state.gteData[reg] =
        preserveDataRegister
            ? previousData[reg]
            : boundaryValue(ctx->cop2d[reg], entryPc,
                            MipsValueBoundary::ExistingGteState, 0x100u + reg);
    const bool preserveControlRegister =
        preserveGte && previousControl[reg].value == ctx->cop2c[reg] &&
        findExpressionConst(previousControl[reg].expression) != nullptr;
    t_state.gteControl[reg] =
        preserveControlRegister
            ? previousControl[reg]
            : boundaryValue(ctx->cop2c[reg], entryPc,
                            MipsValueBoundary::ExistingGteState, 0x200u + reg);
  }
  t_state.hi =
      boundaryValue(ctx->hi, entryPc, MipsValueBoundary::InterpreterEntry, 32);
  t_state.lo =
      boundaryValue(ctx->lo, entryPc, MipsValueBoundary::InterpreterEntry, 33);
}

void endValueFlowSlice(recomp_context *ctx) {
  if (t_state.ctx == ctx)
    t_state.active = false;
}

void beforeValueFlowInstruction(recomp_context *ctx, uint32_t pc,
                                uint32_t raw) {
  // A semantic trigger can arm while an interpreter island is already in
  // flight. Start a fresh, explicitly bounded slice at that exact instruction
  // boundary instead of waiting for another top-level call (which may be
  // after every projection in the selected render submission).
  if ((!t_state.active || t_state.ctx != ctx) && valueFlowEnabled())
    beginValueFlowSlice(ctx, pc);
  if (!t_state.active || t_state.ctx != ctx)
    return;
  const uint8_t rs = static_cast<uint8_t>((raw >> 21) & 31u);
  const uint8_t rt = static_cast<uint8_t>((raw >> 16) & 31u);
  const TrackedValue rsTracked = validatedGpr(ctx, rs, pc);
  const TrackedValue rtTracked = validatedGpr(ctx, rt, pc);
  const TrackedValue hiTracked = validatedSpecial(t_state.hi, ctx->hi, pc, 32);
  const TrackedValue loTracked = validatedSpecial(t_state.lo, ctx->lo, pc, 33);
  t_state.pending = {true,
                     ctx,
                     pc,
                     raw,
                     registerValue(ctx, rs),
                     registerValue(ctx, rt),
                     rsTracked.expression,
                     rtTracked.expression,
                     hiTracked.expression,
                     loTracked.expression,
                     registerValue(ctx, rs) +
                         static_cast<uint32_t>(signExtend16(raw))};
  if ((raw >> 26) == 0x12 && (raw & 0x02000000u) != 0 &&
      (raw & 0x3fu) == 0x12) {
    auto addDataDependency = [&](uint8_t reg) {
      if (t_state.pending.gteDependencyCount >=
          t_state.pending.gteDependencies.size())
        return;
      t_state.pending.gteDependencies[t_state.pending.gteDependencyCount++] =
          validatedGte(t_state.gteData[reg], ctx->cop2d[reg], pc, 0x100u + reg)
              .expression;
    };
    auto addControlDependency = [&](uint8_t reg) {
      if (t_state.pending.gteDependencyCount >=
          t_state.pending.gteDependencies.size())
        return;
      t_state.pending.gteDependencies[t_state.pending.gteDependencyCount++] =
          validatedGte(t_state.gteControl[reg], ctx->cop2c[reg], pc,
                       0x200u + reg)
              .expression;
    };

    const uint8_t matrix = static_cast<uint8_t>((raw >> 17) & 3u);
    const uint8_t vector = static_cast<uint8_t>((raw >> 15) & 3u);
    const uint8_t translation = static_cast<uint8_t>((raw >> 13) & 3u);
    if (matrix < 3) {
      const uint8_t base = static_cast<uint8_t>(matrix * 8u);
      for (uint8_t offset = 0; offset < 5; ++offset)
        addControlDependency(static_cast<uint8_t>(base + offset));
    }
    if (vector < 3) {
      addDataDependency(static_cast<uint8_t>(GTE_VXY0 + vector * 2u));
      addDataDependency(static_cast<uint8_t>(GTE_VZ0 + vector * 2u));
    } else {
      addDataDependency(GTE_IR1);
      addDataDependency(GTE_IR2);
      addDataDependency(GTE_IR3);
    }
    if (translation < 3) {
      const uint8_t base = translation == 0   ? GTE_TRX
                           : translation == 1 ? GTE_RBK
                                              : GTE_RFC;
      addControlDependency(base);
      addControlDependency(static_cast<uint8_t>(base + 1u));
      addControlDependency(static_cast<uint8_t>(base + 2u));
    }
  }
}

void afterValueFlowInstruction(recomp_context *ctx, uint32_t pc, uint32_t raw) {
  if (!t_state.active || t_state.ctx != ctx || !t_state.pending.valid ||
      t_state.pending.ctx != ctx || t_state.pending.pc != pc ||
      t_state.pending.raw != raw)
    return;

  const PendingInstruction pending = t_state.pending;
  t_state.pending.valid = false;
  const uint32_t op = raw >> 26;
  const uint8_t rs = static_cast<uint8_t>((raw >> 21) & 31u);
  const uint8_t rt = static_cast<uint8_t>((raw >> 16) & 31u);
  const uint8_t rd = static_cast<uint8_t>((raw >> 11) & 31u);
  const uint8_t shamt = static_cast<uint8_t>((raw >> 6) & 31u);
  const uint8_t funct = static_cast<uint8_t>(raw & 0x3fu);
  MipsValueExpressionId immediateSigned = 0;
  MipsValueExpressionId immediateUnsigned = 0;
  auto signedImmediate = [&]() {
    if (immediateSigned == 0)
      immediateSigned =
          addConstant(static_cast<uint32_t>(signExtend16(raw)), pc, raw);
    return immediateSigned;
  };
  auto unsignedImmediate = [&]() {
    if (immediateUnsigned == 0)
      immediateUnsigned = addConstant(raw & 0xffffu, pc, raw);
    return immediateUnsigned;
  };

  auto binaryGpr = [&](uint8_t destination, MipsValueOperation operation,
                       MipsValueExpressionId lhs,
                       MipsValueExpressionId rhsExpression,
                       int64_t immediate = 0) {
    assignGpr(ctx, destination,
              addBinary(operation, pc, raw, registerValue(ctx, destination),
                        lhs, rhsExpression, immediate));
  };
  auto unaryGpr = [&](uint8_t destination, MipsValueOperation operation,
                      MipsValueExpressionId source, int64_t immediate = 0) {
    assignGpr(ctx, destination,
              addUnary(operation, pc, raw, registerValue(ctx, destination),
                       source, immediate));
  };

  if (op == 0) {
    switch (funct) {
    case 0x00:
      if (shamt == 0)
        assignGpr(ctx, rd, pending.rtExpression);
      else
        unaryGpr(rd, MipsValueOperation::ShiftLeft, pending.rtExpression,
                 shamt);
      return;
    case 0x02:
      unaryGpr(rd, MipsValueOperation::ShiftRightLogical, pending.rtExpression,
               shamt);
      return;
    case 0x03:
      unaryGpr(rd, MipsValueOperation::ShiftRightArithmetic,
               pending.rtExpression, shamt);
      return;
    case 0x04:
      binaryGpr(rd, MipsValueOperation::ShiftLeft, pending.rtExpression,
                pending.rsExpression, pending.rsValue & 31u);
      return;
    case 0x06:
      binaryGpr(rd, MipsValueOperation::ShiftRightLogical, pending.rtExpression,
                pending.rsExpression, pending.rsValue & 31u);
      return;
    case 0x07:
      binaryGpr(rd, MipsValueOperation::ShiftRightArithmetic,
                pending.rtExpression, pending.rsExpression,
                pending.rsValue & 31u);
      return;
    case 0x0c:
      if (ctx->r[4] == 1)
        t_state.gpr[2] = boundaryValue(ctx->r[2], pc,
                                       MipsValueBoundary::UntrackedMutation, 2);
      return;
    case 0x10:
      assignGpr(ctx, rd, pending.hiExpression);
      return;
    case 0x11:
      t_state.hi = {pending.rsExpression, ctx->hi};
      return;
    case 0x12:
      assignGpr(ctx, rd, pending.loExpression);
      return;
    case 0x13:
      t_state.lo = {pending.rsExpression, ctx->lo};
      return;
    case 0x18:
      t_state.lo = {addBinary(MipsValueOperation::MultiplySignedLow, pc, raw,
                              ctx->lo, pending.rsExpression,
                              pending.rtExpression),
                    ctx->lo};
      t_state.hi = {addBinary(MipsValueOperation::MultiplySignedHigh, pc, raw,
                              ctx->hi, pending.rsExpression,
                              pending.rtExpression),
                    ctx->hi};
      return;
    case 0x19:
      t_state.lo = {addBinary(MipsValueOperation::MultiplyUnsignedLow, pc, raw,
                              ctx->lo, pending.rsExpression,
                              pending.rtExpression),
                    ctx->lo};
      t_state.hi = {addBinary(MipsValueOperation::MultiplyUnsignedHigh, pc, raw,
                              ctx->hi, pending.rsExpression,
                              pending.rtExpression),
                    ctx->hi};
      return;
    case 0x1a:
      t_state.lo = {addBinary(MipsValueOperation::DivideSignedQuotient, pc, raw,
                              ctx->lo, pending.rsExpression,
                              pending.rtExpression),
                    ctx->lo};
      t_state.hi = {addBinary(MipsValueOperation::DivideSignedRemainder, pc,
                              raw, ctx->hi, pending.rsExpression,
                              pending.rtExpression),
                    ctx->hi};
      return;
    case 0x1b:
      t_state.lo = {addBinary(MipsValueOperation::DivideUnsignedQuotient, pc,
                              raw, ctx->lo, pending.rsExpression,
                              pending.rtExpression),
                    ctx->lo};
      t_state.hi = {addBinary(MipsValueOperation::DivideUnsignedRemainder, pc,
                              raw, ctx->hi, pending.rsExpression,
                              pending.rtExpression),
                    ctx->hi};
      return;
    case 0x20:
    case 0x21:
      if (rs == 0)
        assignGpr(ctx, rd, pending.rtExpression);
      else if (rt == 0)
        assignGpr(ctx, rd, pending.rsExpression);
      else
        binaryGpr(rd, MipsValueOperation::Add, pending.rsExpression,
                  pending.rtExpression);
      return;
    case 0x22:
    case 0x23:
      binaryGpr(rd, MipsValueOperation::Subtract, pending.rsExpression,
                pending.rtExpression);
      return;
    case 0x24:
      binaryGpr(rd, MipsValueOperation::BitAnd, pending.rsExpression,
                pending.rtExpression);
      return;
    case 0x25:
      if (rs == 0)
        assignGpr(ctx, rd, pending.rtExpression);
      else if (rt == 0)
        assignGpr(ctx, rd, pending.rsExpression);
      else
        binaryGpr(rd, MipsValueOperation::BitOr, pending.rsExpression,
                  pending.rtExpression);
      return;
    case 0x26:
      binaryGpr(rd, MipsValueOperation::BitXor, pending.rsExpression,
                pending.rtExpression);
      return;
    case 0x27:
      binaryGpr(rd, MipsValueOperation::BitNor, pending.rsExpression,
                pending.rtExpression);
      return;
    case 0x2a:
      binaryGpr(rd, MipsValueOperation::SetLessThanSigned, pending.rsExpression,
                pending.rtExpression);
      return;
    case 0x2b:
      binaryGpr(rd, MipsValueOperation::SetLessThanUnsigned,
                pending.rsExpression, pending.rtExpression);
      return;
    default:
      return;
    }
  }

  switch (op) {
  case 0x08:
  case 0x09:
    if (rs == 0)
      assignGpr(ctx, rt, addConstant(registerValue(ctx, rt), pc, raw));
    else
      binaryGpr(rt, MipsValueOperation::Add, pending.rsExpression,
                signedImmediate(), signExtend16(raw));
    return;
  case 0x0a:
    binaryGpr(rt, MipsValueOperation::SetLessThanSigned, pending.rsExpression,
              signedImmediate(), signExtend16(raw));
    return;
  case 0x0b:
    binaryGpr(rt, MipsValueOperation::SetLessThanUnsigned, pending.rsExpression,
              signedImmediate(), signExtend16(raw));
    return;
  case 0x0c:
    binaryGpr(rt, MipsValueOperation::BitAnd, pending.rsExpression,
              unsignedImmediate(), raw & 0xffffu);
    return;
  case 0x0d:
    if (rs == 0)
      assignGpr(ctx, rt, addConstant(registerValue(ctx, rt), pc, raw));
    else
      binaryGpr(rt, MipsValueOperation::BitOr, pending.rsExpression,
                unsignedImmediate(), raw & 0xffffu);
    return;
  case 0x0e:
    binaryGpr(rt, MipsValueOperation::BitXor, pending.rsExpression,
              unsignedImmediate(), raw & 0xffffu);
    return;
  case 0x0f:
    assignGpr(ctx, rt, addConstant(registerValue(ctx, rt), pc, raw));
    return;
  case 0x10:
    if (rs == 0)
      assignGpr(ctx, rt,
                addBoundary(registerValue(ctx, rt), pc,
                            MipsValueBoundary::UntrackedMutation, rt));
    return;
  case 0x12: {
    if (raw & 0x02000000u) {
      const uint8_t command = static_cast<uint8_t>(raw & 0x3fu);
      // FLAG is command output rather than persistent input provenance.
      t_state.gteControl[GTE_FLAG] = boundaryValue(
          ctx->cop2c[GTE_FLAG], pc, MipsValueBoundary::UntrackedMutation,
          0x200u + GTE_FLAG);
      if (command == 0x12) {
        MipsValueExpressionId dependencies = 0;
        for (uint8_t index = 0; index < pending.gteDependencyCount; ++index) {
          const MipsValueExpressionId dependency =
              pending.gteDependencies[index];
          dependencies =
              dependencies == 0
                  ? dependency
                  : addBinary(MipsValueOperation::GteCommandInput, pc, raw, 0,
                              dependencies, dependency, index);
        }
        for (const uint8_t output :
             {static_cast<uint8_t>(GTE_IR1), static_cast<uint8_t>(GTE_IR2),
              static_cast<uint8_t>(GTE_IR3), static_cast<uint8_t>(GTE_MAC1),
              static_cast<uint8_t>(GTE_MAC2), static_cast<uint8_t>(GTE_MAC3)}) {
          const MipsValueExpressionId result =
              addUnary(MipsValueOperation::GteCommandResult, pc, raw,
                       ctx->cop2d[output], dependencies, output);
          t_state.gteData[output] = {result, ctx->cop2d[output]};
        }
      } else {
        // Until each remaining command has an exact dependency table, never
        // allow an old token to survive an architectural GTE output write.
        for (uint8_t reg = GTE_OTZ; reg < 32; ++reg)
          t_state.gteData[reg] =
              boundaryValue(ctx->cop2d[reg], pc,
                            MipsValueBoundary::UntrackedMutation, 0x100u + reg);
      }
      return;
    }
    if (rs == 0) {
      TrackedValue &source =
          validatedGte(t_state.gteData[rd], ctx->cop2d[rd], pc, 0x100u + rd);
      assignGpr(ctx, rt, source.expression);
    } else if (rs == 2) {
      TrackedValue &source =
          validatedGte(t_state.gteControl[rd], ctx->cop2c[rd], pc, 0x200u + rd);
      assignGpr(ctx, rt, source.expression);
    } else if (rs == 4) {
      const MipsValueExpressionId expression =
          wrapGteWrite(MipsValueOperation::GteDataWrite, pc, raw,
                       ctx->cop2d[rd], pending.rtExpression, rd);
      t_state.gteData[rd] = {expression, ctx->cop2d[rd]};
    } else if (rs == 6) {
      const MipsValueExpressionId expression =
          wrapGteWrite(MipsValueOperation::GteControlWrite, pc, raw,
                       ctx->cop2c[rd], pending.rtExpression, rd);
      t_state.gteControl[rd] = {expression, ctx->cop2c[rd]};
      t_state.observedControlWriteSerial = ctx->gteControlWriteSerial;
    }
    return;
  }
  case 0x20:
  case 0x21:
  case 0x23:
  case 0x24:
  case 0x25: {
    const uint8_t width = op == 0x20 || op == 0x24 ? 1 : op == 0x23 ? 4 : 2;
    const bool signedValue = op == 0x20 || op == 0x21;
    assignGpr(ctx, rt,
              addMemoryLoad(pc, raw, pending.effectiveAddress, width,
                            signedValue, registerValue(ctx, rt)));
    return;
  }
  case 0x22:
  case 0x26: {
    // Infer exactly the byte fragment consumed by executeSimple from the
    // architectural merged result. This avoids a second read from possibly
    // memory-mapped I/O while retaining the precise RAM address and width.
    const uint32_t result = registerValue(ctx, rt);
    const uint32_t aligned = pending.effectiveAddress & ~3u;
    const uint8_t offset = pending.effectiveAddress & 3u;
    uint32_t fragment = 0;
    uint32_t fragmentAddress = aligned;
    uint8_t fragmentWidth = 4;
    if (op == 0x22) {
      fragmentWidth = static_cast<uint8_t>(offset + 1u);
      fragment = result >> ((3u - offset) * 8u);
    } else {
      fragmentWidth = static_cast<uint8_t>(4u - offset);
      fragmentAddress = aligned + offset;
      const uint32_t mask =
          fragmentWidth == 4 ? 0xffffffffu : (1u << (fragmentWidth * 8u)) - 1u;
      fragment = result & mask;
    }
    const MipsValueExpressionId memory =
        addMemoryLoad(pc, raw, fragmentAddress, fragmentWidth, false, fragment);
    const MipsValueOperation mergeOperation =
        op == 0x22 ? MipsValueOperation::LoadWordLeft
                   : MipsValueOperation::LoadWordRight;
    const uint32_t writeMask =
        op == 0x22
            ? (0xffffffffu << ((3u - offset) * 8u))
            : (offset == 0 ? 0xffffffffu : (1u << ((4u - offset) * 8u)) - 1u);
    bool complete = writeMask == 0xffffffffu;
    const MipsValueExpression *previous =
        findExpressionConst(pending.rtExpression);
    if (!complete && previous != nullptr &&
        (previous->operation == MipsValueOperation::LoadWordLeft ||
         previous->operation == MipsValueOperation::LoadWordRight) &&
        expressionComplete(previous->rhs)) {
      const uint8_t previousOffset =
          static_cast<uint8_t>(previous->immediate) & 3u;
      const uint32_t previousMask =
          previous->operation == MipsValueOperation::LoadWordLeft
              ? (0xffffffffu << ((3u - previousOffset) * 8u))
              : (previousOffset == 0
                     ? 0xffffffffu
                     : (1u << ((4u - previousOffset) * 8u)) - 1u);
      complete = (writeMask | previousMask) == 0xffffffffu;
    }
    complete |= expressionComplete(pending.rtExpression);
    const MipsValueExpressionId merged = addExpression(
        mergeOperation, pc, raw, result, pending.rtExpression, memory, offset,
        kNoAddress, 0, false, complete && expressionComplete(memory));
    assignGpr(ctx, rt, merged);
    return;
  }
  case 0x28:
    rememberStore(pending.effectiveAddress, 1, pending.rtValue & 0xffu,
                  pending.rtExpression);
    return;
  case 0x29:
    rememberStore(pending.effectiveAddress, 2, pending.rtValue & 0xffffu,
                  pending.rtExpression);
    return;
  case 0x2a:
  case 0x2e:
    invalidateOverlappingStorage(pending.effectiveAddress & ~3u, 4);
    return;
  case 0x2b:
    rememberStore(pending.effectiveAddress, 4, pending.rtValue,
                  pending.rtExpression);
    return;
  case 0x32: {
    const MipsValueExpressionId load = addMemoryLoad(
        pc, raw, pending.effectiveAddress, 4, false, ctx->cop2d[rt]);
    const MipsValueExpressionId expression = wrapGteWrite(
        MipsValueOperation::GteDataWrite, pc, raw, ctx->cop2d[rt], load, rt);
    t_state.gteData[rt] = {expression, ctx->cop2d[rt]};
    return;
  }
  case 0x3a: {
    TrackedValue &source =
        validatedGte(t_state.gteData[rt], ctx->cop2d[rt], pc, 0x100u + rt);
    rememberStore(pending.effectiveAddress, 4, ctx->cop2d[rt],
                  source.expression);
    return;
  }
  default:
    return;
  }
}

void noteValueFlowRegisterConstant(recomp_context *ctx, uint8_t reg,
                                   uint32_t value, uint32_t pc) {
  if (!t_state.active || t_state.ctx != ctx || reg == 0)
    return;
  t_state.gpr[reg & 31u] = {addConstant(value, pc), value};
}

void noteValueFlowExternalCall(recomp_context *ctx, uint32_t targetPc) {
  if (!t_state.active || t_state.ctx != ctx)
    return;
  const bool controlsUnchanged =
      t_state.observedControlWriteSerial == ctx->gteControlWriteSerial;
  t_state.storedValues.clear();
  for (uint8_t reg = 1; reg < 32; ++reg)
    t_state.gpr[reg] = boundaryValue(ctx->r[reg], targetPc,
                                     MipsValueBoundary::ExternalCall, reg);
  t_state.hi =
      boundaryValue(ctx->hi, targetPc, MipsValueBoundary::ExternalCall, 32);
  t_state.lo =
      boundaryValue(ctx->lo, targetPc, MipsValueBoundary::ExternalCall, 33);
  for (uint8_t reg = 0; reg < 32; ++reg) {
    t_state.gteData[reg] =
        boundaryValue(ctx->cop2d[reg], targetPc,
                      MipsValueBoundary::ExternalCall, 0x100u + reg);
    if (reg == GTE_FLAG || !controlsUnchanged ||
        t_state.gteControl[reg].value != ctx->cop2c[reg]) {
      t_state.gteControl[reg] =
          boundaryValue(ctx->cop2c[reg], targetPc,
                        MipsValueBoundary::ExternalCall, 0x200u + reg);
    }
  }
  t_state.observedControlWriteSerial = ctx->gteControlWriteSerial;
}

GteProjectionValueFlow captureGteProjectionValueFlow(CPUContext *ctx,
                                                     uint8_t vectorIndex,
                                                     bool emitTrace) {
  GteProjectionValueFlow result;
  if (!valueFlowEnabled() || t_state.ctx != ctx || t_state.expressions.empty())
    return result;
  vectorIndex = std::min<uint8_t>(vectorIndex, 2);
  const uint8_t xyRegister = static_cast<uint8_t>(GTE_VXY0 + vectorIndex * 2);
  const uint8_t zRegister = static_cast<uint8_t>(xyRegister + 1);
  result.modelXy =
      validatedGte(t_state.gteData[xyRegister], ctx->cop2d[xyRegister], ctx->pc,
                   0x100u + xyRegister)
          .expression;
  result.modelZ =
      validatedGte(t_state.gteData[zRegister], ctx->cop2d[zRegister], ctx->pc,
                   0x100u + zRegister)
          .expression;
  result.modelFingerprintComplete =
      expressionComplete(result.modelXy) && expressionComplete(result.modelZ);
  if (result.modelFingerprintComplete) {
    const MipsValueExpression *xy = findExpressionConst(result.modelXy);
    const MipsValueExpression *z = findExpressionConst(result.modelZ);
    const fingerprint::Pair model = fingerprint::combineModel(
        {xy->sourceFingerprint, xy->programFingerprint, xy->complete},
        {z->sourceFingerprint, z->programFingerprint, z->complete});
    result.modelSourceFingerprint = model.source;
    result.modelProgramFingerprint = model.program;
  }
  result.complete = result.modelFingerprintComplete;
  for (uint8_t reg = GTE_RT11RT12; reg <= GTE_TRZ; ++reg) {
    result.transform[reg] = validatedGte(t_state.gteControl[reg],
                                         ctx->cop2c[reg], ctx->pc, 0x200u + reg)
                                .expression;
    result.complete &= expressionComplete(result.transform[reg]);
  }
  if (emitTrace) {
    emitExpressionTree(result.modelXy);
    emitExpressionTree(result.modelZ);
    for (const MipsValueExpressionId expression : result.transform)
      emitExpressionTree(expression);
  }
  return result;
}

bool inspectMipsValueExpression(MipsValueExpressionId id,
                                MipsValueExpression &expression) noexcept {
  const MipsValueExpression *found = findExpressionConst(id);
  if (found == nullptr)
    return false;
  expression = *found;
  return true;
}

} // namespace ps1::mips
