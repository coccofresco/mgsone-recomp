#include "runtime/mips_construction_lineage.h"

#include "runtime/cpu_context.h"
#include "runtime/gte.h"
#include "runtime/mips_value_flow.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps1::mips {
namespace {

struct CompactValue {
  uint32_t value = 0;
  // One-based index into the current slice's lazy expression arena. Zero
  // means that construction is incomplete and must fail closed.
  uint32_t expression = 0;
  MipsValueOperation operation = MipsValueOperation::Boundary;
  int64_t immediate = 0;
  bool tracked = false;
};

struct CompactExpression {
  fingerprint::Pair fingerprint{};
  uint32_t lhs = 0;
  uint32_t rhs = 0;
  uint32_t pc = 0;
  uint32_t raw = 0;
  uint32_t value = 0;
  uint32_t address = UINT32_MAX;
  int64_t immediate = 0;
  MipsValueOperation operation = MipsValueOperation::Boundary;
  uint8_t width = 0;
  bool lhsPresent = false;
  bool rhsPresent = false;
  bool signedValue = false;
  bool resolved = false;
};

struct StoredValue {
  uint32_t address = 0;
  uint8_t width = 0;
  uint32_t value = 0;
  CompactValue lineage{};
};

struct PendingInstruction {
  bool valid = false;
  CPUContext *ctx = nullptr;
  uint32_t pc = 0;
  uint32_t raw = 0;
  uint32_t rsValue = 0;
  uint32_t rtValue = 0;
  CompactValue rs{};
  CompactValue rt{};
  CompactValue hi{};
  CompactValue lo{};
  uint32_t effectiveAddress = 0;
  std::array<CompactValue, 16> gteDependencies{};
  uint8_t gteDependencyCount = 0;
};

struct ConstructionLineageState {
  CPUContext *ctx = nullptr;
  bool active = false;
  // Unlike active, this remains true after the normal end of a slice so
  // diagnostics can inspect its final roots. Admission changes invalidate it
  // explicitly, preventing a newly admitted projection from observing stale
  // roots before the next instrumented slice begins.
  bool captureValid = false;
  std::array<CompactValue, 32> gpr{};
  CompactValue hi{};
  CompactValue lo{};
  std::array<CompactValue, 32> gteData{};
  std::array<CompactValue, 32> gteControl{};
  std::unordered_map<uint64_t, StoredValue> storedValues;
  std::vector<CompactExpression> expressions;
  PendingInstruction pending{};
  uint64_t observedControlWriteSerial = 0;
};

struct TransformLineageFilter {
  std::vector<uint64_t> lineages;
  uint64_t generation = 0;
};

struct ConstructionSeedFilter {
  std::vector<ConstructionLineageSeed> seeds;
  uint64_t generation = 0;
  uint32_t minimumPc = UINT32_MAX;
  uint32_t maximumPc = 0;
};

std::atomic<int> g_enabled{-1};
std::atomic<uint64_t> g_transformFilterGeneration{0};
std::atomic<std::shared_ptr<const TransformLineageFilter>> g_transformFilter;
std::atomic<uint64_t> g_seedFilterGeneration{0};
std::atomic<std::shared_ptr<const ConstructionSeedFilter>> g_seedFilter;
thread_local ConstructionLineageState t_state;
thread_local std::shared_ptr<const TransformLineageFilter>
    t_transformFilterCache;
thread_local uint64_t t_transformFilterGeneration = 0;
thread_local bool t_transformAdmitted = true;
thread_local std::shared_ptr<const ConstructionSeedFilter> t_seedFilterCache;
thread_local uint64_t t_seedFilterGeneration = 0;
thread_local bool t_seedAdmitted = true;

constexpr uint32_t kNoAddress = UINT32_MAX;

int32_t signExtend16(uint32_t raw) {
  return static_cast<int16_t>(raw & 0xffffu);
}

uint32_t registerValue(const CPUContext *ctx, uint8_t reg) {
  return reg == 0 ? 0 : ctx->r[reg & 31u];
}

uint64_t storageKey(uint32_t address, uint8_t width) {
  return (static_cast<uint64_t>(fingerprint::physicalAddress(address)) << 8) |
         width;
}

uint32_t appendExpression(CompactExpression expression) {
  if (t_state.expressions.size() >= UINT32_MAX)
    return 0;
  t_state.expressions.push_back(std::move(expression));
  return static_cast<uint32_t>(t_state.expressions.size());
}

uint32_t appendResolvedExpression(fingerprint::Pair pair) {
  if (!pair.complete)
    return 0;
  CompactExpression expression;
  expression.fingerprint = pair;
  expression.resolved = true;
  return appendExpression(std::move(expression));
}

fingerprint::Pair resolveExpression(uint32_t id) {
  if (id == 0 || id > t_state.expressions.size())
    return {};
  CompactExpression &expression = t_state.expressions[id - 1u];
  if (expression.resolved)
    return expression.fingerprint;

  const fingerprint::Pair lhs =
      expression.lhsPresent ? resolveExpression(expression.lhs)
                            : fingerprint::Pair{};
  const fingerprint::Pair rhs =
      expression.rhsPresent ? resolveExpression(expression.rhs)
                            : fingerprint::Pair{};
  if ((expression.lhsPresent && !lhs.complete) ||
      (expression.rhsPresent && !rhs.complete)) {
    return {};
  }
  expression.fingerprint = fingerprint::expression(
      expression.operation, expression.pc, expression.raw, expression.value,
      expression.lhsPresent, lhs, expression.rhsPresent, rhs,
      expression.immediate, expression.address, expression.width,
      expression.signedValue, true);
  expression.resolved = expression.fingerprint.complete;
  return expression.fingerprint;
}

bool expressionComplete(const CompactValue &value) {
  return value.expression != 0;
}

CompactValue makeExpression(MipsValueOperation operation, uint32_t pc,
                            uint32_t raw, uint32_t value,
                            const CompactValue *lhs = nullptr,
                            const CompactValue *rhs = nullptr,
                            int64_t immediate = 0,
                            uint32_t address = kNoAddress, uint8_t width = 0,
                            bool signedValue = false, bool complete = false) {
  CompactValue result;
  result.value = value;
  result.operation = operation;
  result.immediate = immediate;
  result.tracked = true;
  if (complete) {
    CompactExpression expression;
    expression.lhs = lhs ? lhs->expression : 0;
    expression.rhs = rhs ? rhs->expression : 0;
    expression.pc = pc;
    expression.raw = raw;
    expression.value = value;
    expression.address = address;
    expression.immediate = immediate;
    expression.operation = operation;
    expression.width = width;
    expression.lhsPresent = lhs != nullptr;
    expression.rhsPresent = rhs != nullptr;
    expression.signedValue = signedValue;
    if ((!expression.lhsPresent || expression.lhs != 0) &&
        (!expression.rhsPresent || expression.rhs != 0)) {
      result.expression = appendExpression(std::move(expression));
    }
  }
  return result;
}

CompactValue boundaryValue(uint32_t value, uint32_t pc,
                           MipsValueBoundary boundary, uint32_t slot) {
  return makeExpression(MipsValueOperation::Boundary, pc, 0, value, nullptr,
                        nullptr, static_cast<int64_t>(boundary), slot);
}

CompactValue constantValue(uint32_t value, uint32_t pc, uint32_t raw = 0) {
  return makeExpression(MipsValueOperation::Constant, pc, raw, value, nullptr,
                        nullptr, static_cast<int64_t>(value), kNoAddress, 0,
                        false, true);
}

CompactValue unaryValue(MipsValueOperation operation, uint32_t pc, uint32_t raw,
                        uint32_t value, const CompactValue &source,
                        int64_t immediate = 0) {
  return makeExpression(operation, pc, raw, value, &source, nullptr, immediate,
                        kNoAddress, 0, false, expressionComplete(source));
}

CompactValue binaryValue(MipsValueOperation operation, uint32_t pc,
                         uint32_t raw, uint32_t value, const CompactValue &lhs,
                         const CompactValue &rhs, int64_t immediate = 0) {
  return makeExpression(operation, pc, raw, value, &lhs, &rhs, immediate,
                        kNoAddress, 0, false,
                        expressionComplete(lhs) && expressionComplete(rhs));
}

CompactValue &validatedGpr(CPUContext *ctx, uint8_t reg, uint32_t pc) {
  reg &= 31u;
  CompactValue &tracked = t_state.gpr[reg];
  const uint32_t current = registerValue(ctx, reg);
  if (!tracked.tracked || tracked.value != current) {
    tracked = reg == 0
                  ? constantValue(0, pc)
                  : boundaryValue(current, pc,
                                  MipsValueBoundary::UntrackedMutation, reg);
  }
  return tracked;
}

CompactValue &validatedSpecial(CompactValue &tracked, uint32_t current,
                               uint32_t pc, uint32_t slot) {
  if (!tracked.tracked || tracked.value != current) {
    tracked =
        boundaryValue(current, pc, MipsValueBoundary::UntrackedMutation, slot);
  }
  return tracked;
}

CompactValue &validatedGte(CompactValue &tracked, uint32_t current, uint32_t pc,
                           uint32_t slot) {
  if (!tracked.tracked || tracked.value != current) {
    tracked =
        boundaryValue(current, pc, MipsValueBoundary::ExistingGteState, slot);
  }
  return tracked;
}

void assignGpr(CPUContext *ctx, uint8_t reg, CompactValue value) {
  reg &= 31u;
  if (reg == 0)
    return;
  value.value = ctx->r[reg];
  t_state.gpr[reg] = value;
}

void invalidateOverlappingStorage(uint32_t address, uint8_t width) {
  const uint32_t first = fingerprint::physicalAddress(address);
  const uint32_t last = first + std::max<uint8_t>(width, 1u) - 1u;

  // The tracker only records byte, halfword and word stores. Looking up the
  // small set of starts which can overlap this write is exact and bounded;
  // scanning the complete hash table on every interpreted store made the
  // presentation identity path quadratic in long dynamic renderer slices.
  for (const uint8_t storedWidth : {uint8_t{1}, uint8_t{2}, uint8_t{4}}) {
    const uint32_t reach = storedWidth - 1u;
    const uint32_t candidateFirst = first > reach ? first - reach : 0u;
    for (uint32_t candidate = candidateFirst; candidate <= last;
         ++candidate) {
      const auto found =
          t_state.storedValues.find(storageKey(candidate, storedWidth));
      if (found == t_state.storedValues.end())
        continue;
      const StoredValue &stored = found->second;
      const uint32_t storedFirst =
          fingerprint::physicalAddress(stored.address);
      const uint32_t storedLast = storedFirst + stored.width - 1u;
      if (first <= storedLast && storedFirst <= last)
        t_state.storedValues.erase(found);
    }
  }
}

void rememberStore(uint32_t address, uint8_t width, uint32_t value,
                   const CompactValue &lineage) {
  invalidateOverlappingStorage(address, width);
  t_state.storedValues.emplace(storageKey(address, width),
                               StoredValue{address, width, value, lineage});
}

CompactValue memoryLoad(uint32_t pc, uint32_t raw, uint32_t address,
                        uint8_t width, bool signedValue, uint32_t value) {
  const CompactValue *stored = nullptr;
  const auto found = t_state.storedValues.find(storageKey(address, width));
  if (found != t_state.storedValues.end() && found->second.value == value)
    stored = &found->second.lineage;
  const bool complete = stored == nullptr || expressionComplete(*stored);
  return makeExpression(MipsValueOperation::MemoryLoad, pc, raw, value, stored,
                        nullptr, 0, address, width, signedValue, complete);
}

CompactValue wrapGteWrite(MipsValueOperation operation, uint32_t pc,
                          uint32_t raw, uint32_t value,
                          const CompactValue &source, uint8_t gteRegister) {
  return makeExpression(operation, pc, raw, value, &source, nullptr,
                        gteRegister, kNoAddress, 0, false,
                        expressionComplete(source));
}

bool constructionLineageRequested() noexcept {
  int enabled = g_enabled.load(std::memory_order_acquire);
  if (enabled < 0) {
    const char *value = std::getenv("PS1_MIPS_CONSTRUCTION_LINEAGE");
    enabled = value != nullptr && *value != '\0' && *value != '0' ? 1 : 0;
    g_enabled.store(enabled, std::memory_order_release);
  }
  return enabled != 0;
}

std::shared_ptr<const TransformLineageFilter>
synchronizeTransformFilter() noexcept {
  const uint64_t generation =
      g_transformFilterGeneration.load(std::memory_order_acquire);
  if (t_transformFilterGeneration == generation)
    return t_transformFilterCache;
  std::shared_ptr<const TransformLineageFilter> filter =
      g_transformFilter.load(std::memory_order_acquire);

  t_transformFilterCache = filter;
  t_transformFilterGeneration = generation;
  t_transformAdmitted = !filter || filter->lineages.empty();
  t_state.active = false;
  t_state.captureValid = false;
  return filter;
}

std::shared_ptr<const ConstructionSeedFilter>
synchronizeSeedFilter() noexcept {
  const uint64_t generation =
      g_seedFilterGeneration.load(std::memory_order_acquire);
  if (t_seedFilterGeneration == generation)
    return t_seedFilterCache;
  std::shared_ptr<const ConstructionSeedFilter> filter =
      g_seedFilter.load(std::memory_order_acquire);

  t_seedFilterCache = filter;
  t_seedFilterGeneration = generation;
  t_seedAdmitted = !filter || filter->seeds.empty();
  t_state.active = false;
  t_state.captureValid = false;
  return filter;
}

} // namespace

bool constructionLineageEnabled() noexcept {
  if (valueFlowEnabled())
    return true;
  if (!constructionLineageRequested())
    return false;
  const std::shared_ptr<const TransformLineageFilter> filter =
      synchronizeTransformFilter();
  const std::shared_ptr<const ConstructionSeedFilter> seedFilter =
      synchronizeSeedFilter();
  const bool transformAdmitted =
      !filter || filter->lineages.empty() || t_transformAdmitted;
  const bool seedAdmitted =
      !seedFilter || seedFilter->seeds.empty() || t_seedAdmitted;
  return transformAdmitted && seedAdmitted;
}

void setConstructionLineageEnabled(bool enabled) noexcept {
  g_enabled.store(enabled ? 2 : 0, std::memory_order_release);
  if (!enabled) {
    t_state.active = false;
    t_state.captureValid = false;
    t_seedAdmitted = false;
  }
}

void setConstructionLineageEnabledForTesting(bool enabled) noexcept {
  setConstructionLineageEnabled(enabled);
}

void resetConstructionLineageForTesting() noexcept {
  t_state = {};
  t_transformFilterGeneration = 0;
  t_transformFilterCache.reset();
  t_transformAdmitted = true;
  t_seedFilterGeneration = 0;
  t_seedFilterCache.reset();
  t_seedAdmitted = true;
  g_transformFilter.store(nullptr, std::memory_order_release);
  g_transformFilterGeneration.store(0, std::memory_order_release);
  g_seedFilter.store(nullptr, std::memory_order_release);
  g_seedFilterGeneration.store(0, std::memory_order_release);
  g_enabled.store(-1, std::memory_order_release);
}

void setConstructionLineageTransformFilter(
    std::span<const uint64_t> transformLineages) {
  auto next = std::make_shared<TransformLineageFilter>();
  next->lineages.assign(transformLineages.begin(), transformLineages.end());
  std::sort(next->lineages.begin(), next->lineages.end());
  next->lineages.erase(
      std::unique(next->lineages.begin(), next->lineages.end()),
      next->lineages.end());
  next->generation =
      g_transformFilterGeneration.load(std::memory_order_relaxed) + 1;
  const uint64_t generation = next->generation;
  g_transformFilter.store(std::move(next), std::memory_order_release);
  g_transformFilterGeneration.store(generation, std::memory_order_release);
}

void setConstructionLineageSeedFilter(
    std::span<const ConstructionLineageSeed> seeds) {
  auto next = std::make_shared<ConstructionSeedFilter>();
  next->seeds.assign(seeds.begin(), seeds.end());
  for (ConstructionLineageSeed &seed : next->seeds)
    seed.sourceAddress = fingerprint::physicalAddress(seed.sourceAddress);
  std::sort(next->seeds.begin(), next->seeds.end(),
            [](const ConstructionLineageSeed &lhs,
               const ConstructionLineageSeed &rhs) {
              return std::tie(lhs.pc, lhs.rawInstruction, lhs.sourceRegister,
                              lhs.sourceAddress) <
                     std::tie(rhs.pc, rhs.rawInstruction, rhs.sourceRegister,
                              rhs.sourceAddress);
            });
  next->seeds.erase(std::unique(next->seeds.begin(), next->seeds.end()),
                    next->seeds.end());
  if (!next->seeds.empty()) {
    next->minimumPc = next->seeds.front().pc;
    next->maximumPc = next->seeds.back().pc;
  }
  next->generation =
      g_seedFilterGeneration.load(std::memory_order_relaxed) + 1;
  const uint64_t generation = next->generation;
  g_seedFilter.store(std::move(next), std::memory_order_release);
  g_seedFilterGeneration.store(generation, std::memory_order_release);
}

void beginConstructionLineageAdmissionSlice() {
  if (valueFlowEnabled())
    return;
  const std::shared_ptr<const ConstructionSeedFilter> filter =
      synchronizeSeedFilter();
  if (!filter || filter->seeds.empty()) {
    t_seedAdmitted = true;
    return;
  }
  t_seedAdmitted = false;
  t_state.active = false;
  t_state.captureValid = false;
}

bool constructionLineageSeedProbeRequired() noexcept {
  if (valueFlowEnabled() || !constructionLineageRequested())
    return false;
  const std::shared_ptr<const ConstructionSeedFilter> seedFilter =
      synchronizeSeedFilter();
  return seedFilter && !seedFilter->seeds.empty() && !t_seedAdmitted;
}

bool constructionLineageSeedAdmitted() noexcept {
  return t_seedAdmitted;
}

bool tryAdmitConstructionLineageSeed(CPUContext *ctx, uint32_t pc,
                                     uint32_t rawInstruction) noexcept {
  if (ctx == nullptr || valueFlowEnabled() || !constructionLineageRequested() ||
      t_seedAdmitted)
    return false;
  const std::shared_ptr<const ConstructionSeedFilter> filter =
      synchronizeSeedFilter();
  if (!filter || filter->seeds.empty() || pc < filter->minimumPc ||
      pc > filter->maximumPc)
    return false;

  const auto first = std::lower_bound(
      filter->seeds.begin(), filter->seeds.end(), pc,
      [](const ConstructionLineageSeed &seed, uint32_t candidatePc) {
        return seed.pc < candidatePc;
      });
  for (auto seed = first; seed != filter->seeds.end() && seed->pc == pc;
       ++seed) {
    if (seed->rawInstruction != rawInstruction ||
        seed->sourceRegister == 0 || seed->sourceRegister >= 32)
      continue;
    const uint32_t source =
        fingerprint::physicalAddress(ctx->r[seed->sourceRegister]);
    if (source != seed->sourceAddress)
      continue;
    // The seed precedes model construction and may also precede the control
    // writes for this object. Admit provisionally here; the projection
    // endpoint performs the exact transform-lineage check and invalidates all
    // captured roots on a mismatch before they can reach a replay owner.
    t_transformAdmitted = true;
    t_seedAdmitted = true;
    t_state.active = false;
    t_state.captureValid = false;
    return true;
  }
  return false;
}

void refreshConstructionLineageTransformAdmission(CPUContext *ctx) {
  if (valueFlowEnabled())
    return;
  const std::shared_ptr<const TransformLineageFilter> filter =
      synchronizeTransformFilter();
  if (!filter || filter->lineages.empty())
    return;
  // An incomplete context supplies no contradictory evidence. A newly
  // installed non-empty filter already starts disarmed; preserve a prior
  // exact projection admission until a complete replacement exists.
  if (ctx == nullptr || !GTE::projectionTransformLineageComplete(ctx))
    return;
  noteConstructionLineageProjectionTransform(
      GTE::projectionTransformLineage(ctx));
}

void noteConstructionLineageProjectionTransform(uint64_t transformLineage) {
  if (valueFlowEnabled())
    return;
  const std::shared_ptr<const TransformLineageFilter> filter =
      synchronizeTransformFilter();
  if (!filter)
    return;

  const bool admitted =
      filter->lineages.empty() ||
      std::binary_search(filter->lineages.begin(), filter->lineages.end(),
                         transformLineage);
  t_transformAdmitted = admitted;
  if (!admitted) {
    t_state.active = false;
    t_state.captureValid = false;
  }
}

void beginConstructionLineageSlice(recomp_context *ctx, uint32_t entryPc) {
  if (!constructionLineageEnabled() || ctx == nullptr)
    return;

  const bool preserveGte =
      t_state.ctx == ctx &&
      t_state.observedControlWriteSerial == ctx->gteControlWriteSerial;
  const std::array<CompactValue, 32> previousData = t_state.gteData;
  const std::array<CompactValue, 32> previousControl = t_state.gteControl;
  std::array<fingerprint::Pair, 32> previousDataFingerprint{};
  std::array<fingerprint::Pair, 32> previousControlFingerprint{};
  if (preserveGte) {
    for (uint8_t reg = 0; reg < 32; ++reg) {
      if (previousData[reg].tracked &&
          previousData[reg].value == ctx->cop2d[reg] &&
          expressionComplete(previousData[reg])) {
        previousDataFingerprint[reg] =
            resolveExpression(previousData[reg].expression);
      }
      if (previousControl[reg].tracked &&
          previousControl[reg].value == ctx->cop2c[reg] &&
          expressionComplete(previousControl[reg])) {
        previousControlFingerprint[reg] =
            resolveExpression(previousControl[reg].expression);
      }
    }
  }

  // Reset live values without replacing storedValues. clear() retains its
  // buckets, avoiding an allocator round trip for every dynamic island call.
  // The expression arena is slice-local: only resolved GTE roots cross the
  // boundary, so arithmetic which never reaches a projection is never hashed.
  t_state.ctx = nullptr;
  t_state.active = false;
  t_state.captureValid = false;
  t_state.gpr = {};
  t_state.hi = {};
  t_state.lo = {};
  t_state.storedValues.clear();
  if (t_state.storedValues.bucket_count() == 1)
    t_state.storedValues.reserve(256);
  t_state.expressions.clear();
  if (t_state.expressions.capacity() < 4096)
    t_state.expressions.reserve(4096);
  t_state.pending = {};
  t_state.observedControlWriteSerial = 0;
  t_state.ctx = ctx;
  t_state.active = true;
  t_state.captureValid = true;
  t_state.observedControlWriteSerial = ctx->gteControlWriteSerial;

  // Incomplete boundaries are copied as compact metadata; they deliberately
  // do not allocate expression nodes. Keep separate templates for the two
  // kinds so a future fingerprint schema can distinguish them safely.
  const CompactValue interpreterEntry = boundaryValue(
      0, entryPc, MipsValueBoundary::InterpreterEntry, 0);
  const CompactValue existingGteState = boundaryValue(
      0, entryPc, MipsValueBoundary::ExistingGteState, 0x100u);
  for (uint8_t reg = 0; reg < 32; ++reg) {
    const uint32_t value = registerValue(ctx, reg);
    t_state.gpr[reg] = reg == 0 ? constantValue(0, entryPc)
                                : interpreterEntry;
    t_state.gpr[reg].value = value;

    const bool preserveData = preserveGte && previousData[reg].tracked &&
                              previousData[reg].value == ctx->cop2d[reg];
    if (preserveData) {
      t_state.gteData[reg] = previousData[reg];
      t_state.gteData[reg].expression =
          appendResolvedExpression(previousDataFingerprint[reg]);
    } else {
      t_state.gteData[reg] = existingGteState;
      t_state.gteData[reg].value = ctx->cop2d[reg];
    }
    const bool preserveControl =
        preserveGte && previousControl[reg].tracked &&
        previousControl[reg].value == ctx->cop2c[reg];
    if (preserveControl) {
      t_state.gteControl[reg] = previousControl[reg];
      t_state.gteControl[reg].expression =
          appendResolvedExpression(previousControlFingerprint[reg]);
    } else {
      t_state.gteControl[reg] = existingGteState;
      t_state.gteControl[reg].value = ctx->cop2c[reg];
    }
  }
  t_state.hi = interpreterEntry;
  t_state.hi.value = ctx->hi;
  t_state.lo = interpreterEntry;
  t_state.lo.value = ctx->lo;
}

void endConstructionLineageSlice(recomp_context *ctx) {
  if (t_state.ctx == ctx)
    t_state.active = false;
}

void beforeConstructionLineageInstruction(recomp_context *ctx, uint32_t pc,
                                          uint32_t raw) {
  if ((!t_state.active || t_state.ctx != ctx) && constructionLineageEnabled())
    beginConstructionLineageSlice(ctx, pc);
  if (!t_state.active || t_state.ctx != ctx)
    return;

  const uint8_t rs = static_cast<uint8_t>((raw >> 21) & 31u);
  const uint8_t rt = static_cast<uint8_t>((raw >> 16) & 31u);
  PendingInstruction &pending = t_state.pending;
  // Do not value-initialize the 16-entry GTE dependency array for every MIPS
  // instruction. Its prefix is overwritten below only for an RTPS command.
  // Copying/clearing that array was the dominant fixed cost even for branches
  // and integer instructions unrelated to a projected vertex.
  pending.valid = true;
  pending.ctx = ctx;
  pending.pc = pc;
  pending.raw = raw;
  pending.rsValue = registerValue(ctx, rs);
  pending.rtValue = registerValue(ctx, rt);
  pending.rs = validatedGpr(ctx, rs, pc);
  pending.rt = validatedGpr(ctx, rt, pc);
  const uint32_t op = raw >> 26;
  const uint8_t funct = static_cast<uint8_t>(raw & 0x3fu);
  if (op == 0 && funct == 0x10)
    pending.hi = validatedSpecial(t_state.hi, ctx->hi, pc, 32);
  else if (op == 0 && funct == 0x12)
    pending.lo = validatedSpecial(t_state.lo, ctx->lo, pc, 33);
  pending.effectiveAddress =
      registerValue(ctx, rs) + static_cast<uint32_t>(signExtend16(raw));
  pending.gteDependencyCount = 0;

  if ((raw >> 26) != 0x12 || (raw & 0x02000000u) == 0 ||
      (raw & 0x3fu) != 0x12) {
    return;
  }
  auto addDataDependency = [&](uint8_t reg) {
    if (pending.gteDependencyCount >= pending.gteDependencies.size())
      return;
    pending.gteDependencies[pending.gteDependencyCount++] =
        validatedGte(t_state.gteData[reg], ctx->cop2d[reg], pc, 0x100u + reg);
  };
  auto addControlDependency = [&](uint8_t reg) {
    if (pending.gteDependencyCount >= pending.gteDependencies.size())
      return;
    pending.gteDependencies[pending.gteDependencyCount++] =
        validatedGte(t_state.gteControl[reg], ctx->cop2c[reg], pc,
                     0x200u + reg);
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

void afterConstructionLineageInstruction(recomp_context *ctx, uint32_t pc,
                                         uint32_t raw) {
  if (!t_state.active || t_state.ctx != ctx || !t_state.pending.valid ||
      t_state.pending.ctx != ctx || t_state.pending.pc != pc ||
      t_state.pending.raw != raw) {
    return;
  }

  // afterConstructionLineageInstruction does not recurse, so the pending
  // storage remains stable until this function returns. Referencing it avoids
  // copying the large GTE dependency array on every interpreted instruction.
  PendingInstruction &pending = t_state.pending;
  pending.valid = false;
  const uint32_t op = raw >> 26;
  const uint8_t rs = static_cast<uint8_t>((raw >> 21) & 31u);
  const uint8_t rt = static_cast<uint8_t>((raw >> 16) & 31u);
  const uint8_t rd = static_cast<uint8_t>((raw >> 11) & 31u);
  const uint8_t shamt = static_cast<uint8_t>((raw >> 6) & 31u);
  const uint8_t funct = static_cast<uint8_t>(raw & 0x3fu);
  CompactValue immediateSigned{};
  CompactValue immediateUnsigned{};
  auto signedImmediate = [&]() -> const CompactValue & {
    if (!immediateSigned.tracked) {
      immediateSigned =
          constantValue(static_cast<uint32_t>(signExtend16(raw)), pc, raw);
    }
    return immediateSigned;
  };
  auto unsignedImmediate = [&]() -> const CompactValue & {
    if (!immediateUnsigned.tracked)
      immediateUnsigned = constantValue(raw & 0xffffu, pc, raw);
    return immediateUnsigned;
  };
  auto binaryGpr = [&](uint8_t destination, MipsValueOperation operation,
                       const CompactValue &lhs, const CompactValue &rhsValue,
                       int64_t immediate = 0) {
    assignGpr(ctx, destination,
              binaryValue(operation, pc, raw, registerValue(ctx, destination),
                          lhs, rhsValue, immediate));
  };
  auto unaryGpr = [&](uint8_t destination, MipsValueOperation operation,
                      const CompactValue &source, int64_t immediate = 0) {
    assignGpr(ctx, destination,
              unaryValue(operation, pc, raw, registerValue(ctx, destination),
                         source, immediate));
  };

  if (op == 0) {
    switch (funct) {
    case 0x00:
      if (shamt == 0)
        assignGpr(ctx, rd, pending.rt);
      else
        unaryGpr(rd, MipsValueOperation::ShiftLeft, pending.rt, shamt);
      return;
    case 0x02:
      unaryGpr(rd, MipsValueOperation::ShiftRightLogical, pending.rt, shamt);
      return;
    case 0x03:
      unaryGpr(rd, MipsValueOperation::ShiftRightArithmetic, pending.rt, shamt);
      return;
    case 0x04:
      binaryGpr(rd, MipsValueOperation::ShiftLeft, pending.rt, pending.rs,
                pending.rsValue & 31u);
      return;
    case 0x06:
      binaryGpr(rd, MipsValueOperation::ShiftRightLogical, pending.rt,
                pending.rs, pending.rsValue & 31u);
      return;
    case 0x07:
      binaryGpr(rd, MipsValueOperation::ShiftRightArithmetic, pending.rt,
                pending.rs, pending.rsValue & 31u);
      return;
    case 0x0c:
      if (ctx->r[4] == 1) {
        t_state.gpr[2] = boundaryValue(ctx->r[2], pc,
                                       MipsValueBoundary::UntrackedMutation, 2);
      }
      return;
    case 0x10:
      assignGpr(ctx, rd, pending.hi);
      return;
    case 0x11:
      t_state.hi = pending.rs;
      t_state.hi.value = ctx->hi;
      return;
    case 0x12:
      assignGpr(ctx, rd, pending.lo);
      return;
    case 0x13:
      t_state.lo = pending.rs;
      t_state.lo.value = ctx->lo;
      return;
    case 0x18:
      t_state.lo = binaryValue(MipsValueOperation::MultiplySignedLow, pc, raw,
                               ctx->lo, pending.rs, pending.rt);
      t_state.hi = binaryValue(MipsValueOperation::MultiplySignedHigh, pc, raw,
                               ctx->hi, pending.rs, pending.rt);
      return;
    case 0x19:
      t_state.lo = binaryValue(MipsValueOperation::MultiplyUnsignedLow, pc, raw,
                               ctx->lo, pending.rs, pending.rt);
      t_state.hi = binaryValue(MipsValueOperation::MultiplyUnsignedHigh, pc,
                               raw, ctx->hi, pending.rs, pending.rt);
      return;
    case 0x1a:
      t_state.lo = binaryValue(MipsValueOperation::DivideSignedQuotient, pc,
                               raw, ctx->lo, pending.rs, pending.rt);
      t_state.hi = binaryValue(MipsValueOperation::DivideSignedRemainder, pc,
                               raw, ctx->hi, pending.rs, pending.rt);
      return;
    case 0x1b:
      t_state.lo = binaryValue(MipsValueOperation::DivideUnsignedQuotient, pc,
                               raw, ctx->lo, pending.rs, pending.rt);
      t_state.hi = binaryValue(MipsValueOperation::DivideUnsignedRemainder, pc,
                               raw, ctx->hi, pending.rs, pending.rt);
      return;
    case 0x20:
    case 0x21:
      if (rs == 0)
        assignGpr(ctx, rd, pending.rt);
      else if (rt == 0)
        assignGpr(ctx, rd, pending.rs);
      else
        binaryGpr(rd, MipsValueOperation::Add, pending.rs, pending.rt);
      return;
    case 0x22:
    case 0x23:
      binaryGpr(rd, MipsValueOperation::Subtract, pending.rs, pending.rt);
      return;
    case 0x24:
      binaryGpr(rd, MipsValueOperation::BitAnd, pending.rs, pending.rt);
      return;
    case 0x25:
      if (rs == 0)
        assignGpr(ctx, rd, pending.rt);
      else if (rt == 0)
        assignGpr(ctx, rd, pending.rs);
      else
        binaryGpr(rd, MipsValueOperation::BitOr, pending.rs, pending.rt);
      return;
    case 0x26:
      binaryGpr(rd, MipsValueOperation::BitXor, pending.rs, pending.rt);
      return;
    case 0x27:
      binaryGpr(rd, MipsValueOperation::BitNor, pending.rs, pending.rt);
      return;
    case 0x2a:
      binaryGpr(rd, MipsValueOperation::SetLessThanSigned, pending.rs,
                pending.rt);
      return;
    case 0x2b:
      binaryGpr(rd, MipsValueOperation::SetLessThanUnsigned, pending.rs,
                pending.rt);
      return;
    default:
      return;
    }
  }

  switch (op) {
  case 0x08:
  case 0x09:
    if (rs == 0)
      assignGpr(ctx, rt, constantValue(registerValue(ctx, rt), pc, raw));
    else
      binaryGpr(rt, MipsValueOperation::Add, pending.rs, signedImmediate(),
                signExtend16(raw));
    return;
  case 0x0a:
    binaryGpr(rt, MipsValueOperation::SetLessThanSigned, pending.rs,
              signedImmediate(), signExtend16(raw));
    return;
  case 0x0b:
    binaryGpr(rt, MipsValueOperation::SetLessThanUnsigned, pending.rs,
              signedImmediate(), signExtend16(raw));
    return;
  case 0x0c:
    binaryGpr(rt, MipsValueOperation::BitAnd, pending.rs, unsignedImmediate(),
              raw & 0xffffu);
    return;
  case 0x0d:
    if (rs == 0)
      assignGpr(ctx, rt, constantValue(registerValue(ctx, rt), pc, raw));
    else
      binaryGpr(rt, MipsValueOperation::BitOr, pending.rs, unsignedImmediate(),
                raw & 0xffffu);
    return;
  case 0x0e:
    binaryGpr(rt, MipsValueOperation::BitXor, pending.rs, unsignedImmediate(),
              raw & 0xffffu);
    return;
  case 0x0f:
    assignGpr(ctx, rt, constantValue(registerValue(ctx, rt), pc, raw));
    return;
  case 0x10:
    if (rs == 0) {
      assignGpr(ctx, rt,
                boundaryValue(registerValue(ctx, rt), pc,
                              MipsValueBoundary::UntrackedMutation, rt));
    }
    return;
  case 0x12: {
    if (raw & 0x02000000u) {
      const uint8_t command = static_cast<uint8_t>(raw & 0x3fu);
      t_state.gteControl[GTE_FLAG] = boundaryValue(
          ctx->cop2c[GTE_FLAG], pc, MipsValueBoundary::UntrackedMutation,
          0x200u + GTE_FLAG);
      if (command == 0x12) {
        CompactValue dependencies{};
        for (uint8_t index = 0; index < pending.gteDependencyCount; ++index) {
          const CompactValue &dependency = pending.gteDependencies[index];
          dependencies =
              !dependencies.tracked
                  ? dependency
                  : binaryValue(MipsValueOperation::GteCommandInput, pc, raw, 0,
                                dependencies, dependency, index);
        }
        for (const uint8_t output :
             {static_cast<uint8_t>(GTE_IR1), static_cast<uint8_t>(GTE_IR2),
              static_cast<uint8_t>(GTE_IR3), static_cast<uint8_t>(GTE_MAC1),
              static_cast<uint8_t>(GTE_MAC2), static_cast<uint8_t>(GTE_MAC3)}) {
          t_state.gteData[output] =
              unaryValue(MipsValueOperation::GteCommandResult, pc, raw,
                         ctx->cop2d[output], dependencies, output);
        }
      } else {
        for (uint8_t reg = GTE_OTZ; reg < 32; ++reg) {
          t_state.gteData[reg] =
              boundaryValue(ctx->cop2d[reg], pc,
                            MipsValueBoundary::UntrackedMutation, 0x100u + reg);
        }
      }
      return;
    }
    if (rs == 0) {
      assignGpr(
          ctx, rt,
          validatedGte(t_state.gteData[rd], ctx->cop2d[rd], pc, 0x100u + rd));
    } else if (rs == 2) {
      assignGpr(ctx, rt,
                validatedGte(t_state.gteControl[rd], ctx->cop2c[rd], pc,
                             0x200u + rd));
    } else if (rs == 4) {
      t_state.gteData[rd] = wrapGteWrite(MipsValueOperation::GteDataWrite, pc,
                                         raw, ctx->cop2d[rd], pending.rt, rd);
    } else if (rs == 6) {
      t_state.gteControl[rd] =
          wrapGteWrite(MipsValueOperation::GteControlWrite, pc, raw,
                       ctx->cop2c[rd], pending.rt, rd);
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
              memoryLoad(pc, raw, pending.effectiveAddress, width, signedValue,
                         registerValue(ctx, rt)));
    return;
  }
  case 0x22:
  case 0x26: {
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
    const CompactValue memory =
        memoryLoad(pc, raw, fragmentAddress, fragmentWidth, false, fragment);
    const MipsValueOperation mergeOperation =
        op == 0x22 ? MipsValueOperation::LoadWordLeft
                   : MipsValueOperation::LoadWordRight;
    const uint32_t writeMask =
        op == 0x22
            ? (0xffffffffu << ((3u - offset) * 8u))
            : (offset == 0 ? 0xffffffffu : (1u << ((4u - offset) * 8u)) - 1u);
    bool complete = writeMask == 0xffffffffu;
    if (!complete &&
        (pending.rt.operation == MipsValueOperation::LoadWordLeft ||
         pending.rt.operation == MipsValueOperation::LoadWordRight) &&
        expressionComplete(pending.rt)) {
      const uint8_t previousOffset =
          static_cast<uint8_t>(pending.rt.immediate) & 3u;
      const uint32_t previousMask =
          pending.rt.operation == MipsValueOperation::LoadWordLeft
              ? (0xffffffffu << ((3u - previousOffset) * 8u))
              : (previousOffset == 0
                     ? 0xffffffffu
                     : (1u << ((4u - previousOffset) * 8u)) - 1u);
      complete = (writeMask | previousMask) == 0xffffffffu;
    }
    complete |= expressionComplete(pending.rt);
    const CompactValue merged = makeExpression(
        mergeOperation, pc, raw, result, &pending.rt, &memory, offset,
        kNoAddress, 0, false, complete && expressionComplete(memory));
    assignGpr(ctx, rt, merged);
    return;
  }
  case 0x28:
    rememberStore(pending.effectiveAddress, 1, pending.rtValue & 0xffu,
                  pending.rt);
    return;
  case 0x29:
    rememberStore(pending.effectiveAddress, 2, pending.rtValue & 0xffffu,
                  pending.rt);
    return;
  case 0x2a:
  case 0x2e:
    invalidateOverlappingStorage(pending.effectiveAddress & ~3u, 4);
    return;
  case 0x2b:
    rememberStore(pending.effectiveAddress, 4, pending.rtValue, pending.rt);
    return;
  case 0x32: {
    const CompactValue load =
        memoryLoad(pc, raw, pending.effectiveAddress, 4, false, ctx->cop2d[rt]);
    t_state.gteData[rt] = wrapGteWrite(MipsValueOperation::GteDataWrite, pc,
                                       raw, ctx->cop2d[rt], load, rt);
    return;
  }
  case 0x3a:
    rememberStore(
        pending.effectiveAddress, 4, ctx->cop2d[rt],
        validatedGte(t_state.gteData[rt], ctx->cop2d[rt], pc, 0x100u + rt));
    return;
  default:
    return;
  }
}

void noteConstructionLineageRegisterConstant(recomp_context *ctx, uint8_t reg,
                                             uint32_t value, uint32_t pc) {
  if (!t_state.active || t_state.ctx != ctx || reg == 0)
    return;
  t_state.gpr[reg & 31u] = constantValue(value, pc);
}

void noteConstructionLineageExternalCall(recomp_context *ctx,
                                         uint32_t targetPc) {
  if (!t_state.active || t_state.ctx != ctx)
    return;
  const bool controlsUnchanged =
      t_state.observedControlWriteSerial == ctx->gteControlWriteSerial;
  t_state.storedValues.clear();
  for (uint8_t reg = 1; reg < 32; ++reg) {
    t_state.gpr[reg] = boundaryValue(ctx->r[reg], targetPc,
                                     MipsValueBoundary::ExternalCall, reg);
  }
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

GteProjectionConstructionLineage
captureGteProjectionConstructionLineage(CPUContext *ctx, uint8_t vectorIndex) {
  GteProjectionConstructionLineage result;
  if (!constructionLineageEnabled() || !t_state.captureValid ||
      t_state.ctx != ctx)
    return result;
  vectorIndex = std::min<uint8_t>(vectorIndex, 2);
  const uint8_t xyRegister = static_cast<uint8_t>(GTE_VXY0 + vectorIndex * 2);
  const uint8_t zRegister = static_cast<uint8_t>(xyRegister + 1u);
  const CompactValue &xy =
      validatedGte(t_state.gteData[xyRegister], ctx->cop2d[xyRegister], ctx->pc,
                   0x100u + xyRegister);
  const CompactValue &z =
      validatedGte(t_state.gteData[zRegister], ctx->cop2d[zRegister], ctx->pc,
                   0x100u + zRegister);
  const fingerprint::Pair model = fingerprint::combineModel(
      resolveExpression(xy.expression), resolveExpression(z.expression));
  result.modelSourceFingerprint = model.source;
  result.modelProgramFingerprint = model.program;
  result.complete = model.complete;
  return result;
}

} // namespace ps1::mips
