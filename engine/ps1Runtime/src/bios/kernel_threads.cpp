#include "runtime/bios/kernel_threads.h"
#include "runtime/memory.h"
#include "runtime/metrics.h"
#include <array>
#include <exception>
#include <stdexcept>
#include <vector>
#include <system_error>
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__unix__) || defined(__APPLE__)
#include <ucontext.h>
#endif
extern void recomp_dispatch(uint8_t *, recomp_context *, uint32_t);
extern void recomp_resume_dispatch(uint8_t *, recomp_context *, uint32_t);

namespace ps1::bios {
#if defined(_WIN32)
namespace {
// Conversion belongs to the host thread, not to a particular saved timeline.
// A restart replaces guest continuations while retaining this root stack.
void *windowsRootFiber() {
  struct Root {
    void *handle = nullptr;
    bool converted = false;
    Root() {
      converted = !IsThreadAFiber();
      handle = converted ? ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH)
                         : GetCurrentFiber();
      if (!handle)
        throw std::system_error(GetLastError(), std::system_category(), "ConvertThreadToFiberEx");
    }
    ~Root() {
      if (converted && IsThreadAFiber() && GetCurrentFiber() == handle)
        ConvertFiberToThread();
    }
  };
  thread_local Root root;
  return root.handle;
}
}
#endif
struct KernelThreads::Impl {
  recomp_context &ctx;
  struct Fiber {
#if defined(__unix__) || defined(__APPLE__)
    ucontext_t host{};
#elif defined(_WIN32)
    void *host = nullptr;
    bool ownsHost = false;
#endif
    std::vector<uint8_t> stack;
    Impl *owner = nullptr;
    uint32_t tcb = 0;
    uint32_t resumePc = 0;
    bool initialized = false;
  };
  std::array<Fiber, 64> fibers;
  Fiber *current = nullptr;
  Fiber *root = nullptr;
  std::exception_ptr failure;
  explicit Impl(recomp_context &c) : ctx(c) {}
  static void release(Fiber &fiber) {
#if defined(_WIN32)
    if (fiber.ownsHost && fiber.host) DeleteFiber(fiber.host);
    fiber.host = nullptr;
    fiber.ownsHost = false;
#endif
  }
  ~Impl() { for (auto &fiber : fibers) release(fiber); }
  uint32_t pcb() { return ctx.mem->read32(0x108); }
  uint32_t base() { return ctx.mem->read32(0x110); }
  uint32_t count() { return ctx.mem->read32(0x114) / 0xc0; }
  void validate(uint32_t tcb) {
    const uint32_t first = base();
    if (count() > fibers.size() || tcb < first || (tcb - first) % 0xc0 ||
        (tcb - first) / 0xc0 >= count() ||
        (tcb & 0x1fffffff) > Memory::RAM_SIZE - 0xc0)
      throw std::runtime_error("invalid BIOS TCB address");
  }
  void save(Fiber &fiber, uint32_t resumePc) {
    fiber.resumePc = resumePc;
    auto &m = *ctx.mem;
    for (unsigned i = 1; i < 32; i++)
      m.write32(fiber.tcb + 8 + i * 4, ctx.r[i]);
    m.write32(fiber.tcb + 0x88, resumePc ? resumePc : ctx.r31);
    m.write32(fiber.tcb + 0x8c, ctx.hi);
    m.write32(fiber.tcb + 0x90, ctx.lo);
    const uint32_t sr = ctx.cop0[ps1::COP0_SR];
    m.write32(fiber.tcb + 0x94, (sr & ~0x3fu) | ((sr & 0xfu) << 2));
    m.write32(fiber.tcb + 0x98, ctx.cop0[ps1::COP0_CAUSE]);
  }
  void load(Fiber &fiber) {
    auto &m = *ctx.mem;
    for (unsigned i = 1; i < 32; i++)
      ctx.r[i] = m.read32(fiber.tcb + 8 + i * 4);
    ctx.r0 = 0;
    ctx.pc = m.read32(fiber.tcb + 0x88);
    ctx.hi = m.read32(fiber.tcb + 0x8c);
    ctx.lo = m.read32(fiber.tcb + 0x90);
    const uint32_t sr = m.read32(fiber.tcb + 0x94);
    ctx.cop0[ps1::COP0_SR] = (sr & ~0xfu) | ((sr >> 2) & 0xfu);
    for (auto &p : ctx.pgxpGpr)
      p = {};
  }
  static void run(Fiber *fiber) {
    auto &self = *fiber->owner;
    try {
      if (fiber->resumePc)
        recomp_resume_dispatch(self.ctx.mem->ramPtr(), &self.ctx, fiber->resumePc);
      else
        recomp_dispatch(self.ctx.mem->ramPtr(), &self.ctx, self.ctx.pc);
      throw std::runtime_error("BIOS thread entry unexpectedly returned");
    } catch (...) {
      self.failure = std::current_exception();
      self.current = self.root;
#if defined(__unix__) || defined(__APPLE__)
      setcontext(&self.root->host);
#elif defined(_WIN32)
      SwitchToFiber(self.root->host);
#endif
      std::terminate();
    }
  }
  static void entry(uint32_t low, uint32_t high) {
    run(reinterpret_cast<Fiber *>((uintptr_t(high) << 32) | low));
  }
#if defined(_WIN32)
  static void WINAPI windowsEntry(void *data) { run(static_cast<Fiber *>(data)); }
#endif
  void select(uint32_t address, uint32_t resumePc) {
    validate(address);
    if (!current) {
      const uint32_t old = ctx.mem->read32(pcb());
      validate(old);
      root = current = &fibers[(old - base()) / 0xc0];
      current->tcb = old;
      current->owner = this;
      current->initialized = true;
#if defined(_WIN32)
      current->host = windowsRootFiber();
#endif
    }
    if (current->tcb == address)
      return;
    Fiber &target = fibers[(address - base()) / 0xc0];
    if (ctx.mem->read32(address) != 0x4000)
      throw std::runtime_error("ChangeTh to free TCB");
#if defined(__unix__) || defined(__APPLE__)
    if (!target.initialized) {
      target.stack.resize(4 * 1024 * 1024);
      target.owner = this;
      target.tcb = address;
      if (getcontext(&target.host) != 0)
        throw std::runtime_error("getcontext failed");
      target.host.uc_stack.ss_sp = target.stack.data();
      target.host.uc_stack.ss_size = target.stack.size();
      target.host.uc_link = nullptr;
      const uintptr_t pointer = reinterpret_cast<uintptr_t>(&target);
      makecontext(&target.host, reinterpret_cast<void (*)()>(&Impl::entry), 2,
                  uint32_t(pointer), uint32_t(pointer >> 32));
      target.initialized = true;
    }
#elif defined(_WIN32)
    if (!target.initialized) {
      target.owner = this;
      target.tcb = address;
      target.host = CreateFiberEx(64 * 1024, 4 * 1024 * 1024,
                                 FIBER_FLAG_FLOAT_SWITCH, windowsEntry, &target);
      if (!target.host)
        throw std::system_error(GetLastError(), std::system_category(), "CreateFiberEx");
      target.ownsHost = true;
      target.initialized = true;
    }
#else
    throw std::runtime_error("Native BIOS thread continuations require a supported host fiber backend");
#endif
    Fiber *previous = current;
    save(*previous, resumePc);
    load(target);
    current = &target;
    ctx.mem->write32(pcb(), address);
    metrics::count("bios.thread_switch");
    metrics::trace("bios.thread_switch",
                   {{"from", previous->tcb}, {"to", address}, {"pc", ctx.pc}});
#if defined(__unix__) || defined(__APPLE__)
    if (swapcontext(&previous->host, &target.host) != 0)
      throw std::runtime_error("swapcontext failed");
#elif defined(_WIN32)
    SwitchToFiber(target.host);
#endif
    if (failure)
      std::rethrow_exception(failure);
  }
};
KernelThreads::KernelThreads(recomp_context &c)
    : impl_(std::make_unique<Impl>(c)) {}
KernelThreads::~KernelThreads() = default;
bool KernelThreads::active() const { return impl_->current != nullptr; }
uint32_t KernelThreads::open(uint32_t entry, uint32_t sp, uint32_t gp) {
  auto &s = *impl_;
  auto &m = *s.ctx.mem;
  for (uint32_t i = 0; i < s.count() && i < s.fibers.size(); i++) {
    const uint32_t t = s.base() + i * 0xc0;
    s.validate(t);
    if (m.read32(t) != 0x1000)
      continue;
    if (s.current == &s.fibers[i])
      throw std::runtime_error("reopening active native TCB");
    Impl::release(s.fibers[i]);
    s.fibers[i] = Impl::Fiber{};
    m.write32(t, 0x4000);
    m.write32(t + 4, 0x1000);
    m.write32(t + 0x88, entry);
    m.write32(t + 8 + 29 * 4, sp);
    m.write32(t + 8 + 28 * 4, gp);
    return 0xff000000u | i;
  }
  return 0xffffffffu;
}
uint32_t KernelThreads::close(uint32_t handle) {
  auto &s = *impl_;
  const uint32_t t = s.base() + (handle & 0xffff) * 0xc0;
  s.validate(t);
  s.ctx.mem->write32(t, 0x1000);
  return 1;
}
void KernelThreads::change(uint32_t handle) {
  auto &s = *impl_;
  s.ctx.r2 = 1;
  s.select(s.base() + (handle & 0xffff) * 0xc0, s.ctx.r31);
}
void KernelThreads::captureCurrent(uint32_t resumePc) {
  if (impl_->current)
    impl_->save(*impl_->current, resumePc);
}
void KernelThreads::scheduleSelected(uint32_t resumePc) {
  if (impl_->current)
    impl_->select(impl_->ctx.mem->read32(impl_->pcb()), resumePc);
}
bool KernelThreads::canSnapshot() const {
  if (!impl_->current) return true;
  for (const auto &fiber : impl_->fibers)
    if (fiber.initialized && impl_->ctx.mem->read32(fiber.tcb) == 0x4000 && !fiber.resumePc)
      return false;
  return true;
}
KernelThreads::Snapshot KernelThreads::snapshot() const {
  Snapshot result;
  if (!impl_->current) return result;
  if (!canSnapshot()) throw std::runtime_error("native thread outside resumable boundary");
  result.currentTcb = impl_->current->tcb;
  for (unsigned i = 0; i < impl_->fibers.size(); ++i) {
    const auto &fiber = impl_->fibers[i];
    // A restored thread may remain dormant across several saves. Its guest
    // continuation exists even before we allocate a new host stack for it.
    if ((fiber.initialized || fiber.resumePc) &&
        impl_->ctx.mem->read32(fiber.tcb) == 0x4000)
      result.resumePcs[i] = fiber.resumePc;
  }
  return result;
}
void KernelThreads::restart(const Snapshot &state) {
  // Called only after unwinding onto the host game-thread stack. No old
  // continuation can reference these allocations after this replacement.
  auto fresh = std::make_unique<Impl>(impl_->ctx);
  if (state.currentTcb) {
    fresh->validate(state.currentTcb);
    for (unsigned i = 0; i < fresh->fibers.size(); ++i) {
      auto &fiber = fresh->fibers[i];
      fiber.resumePc = state.resumePcs[i];
      fiber.tcb = fresh->base() + i * 0xc0;
      fiber.owner = fresh.get();
      // The BIOS TCB is also the architectural authority for a dormant
      // thread. Older snapshots omitted the metadata above when a restored
      // thread had not run again; recover its PC without replaying a prologue.
      if (!fiber.resumePc && i < fresh->count() &&
          fresh->ctx.mem->read32(fiber.tcb) == 0x4000)
        fiber.resumePc = fresh->ctx.mem->read32(fiber.tcb + 0x88);
    }
    fresh->root = fresh->current = &fresh->fibers[(state.currentTcb - fresh->base()) / 0xc0];
    fresh->current->initialized = true;
#if defined(_WIN32)
    fresh->current->host = windowsRootFiber();
#endif
  }
  impl_ = std::move(fresh);
}
} // namespace ps1::bios
