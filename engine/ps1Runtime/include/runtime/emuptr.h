#pragma once
/**
 * @file emuptr.h
 * @brief Typed wrapper around a PS1 virtual address.
 *
 * `emuptr<T>` holds a 32-bit PS1 virtual address and forwards `->`, `*`,
 * and `[]` to the live RAM buffer. It exists to make hand-written HLE
 * bodies look like ordinary C++ when manipulating PS1-side structs:
 *
 *     emuptr<nspage> p = ...;
 *     if (p->magic != 0x1234) return 0;
 *     p->entries[i] = ...;
 *
 * The bound RAM buffer is a process-wide pointer set once at runtime
 * startup via `emuptr_set_ram(memory.ramPtr())`. Tests call the same
 * setter against a fake buffer.
 *
 * **Scope:** RAM only. `emuptr<T>` translates virtual addresses through
 * the standard KUSEG/KSEG0/KSEG1 mirrors and the 2 MB RAM mask, but it
 * does *not* route to MMIO, scratchpad, or BIOS ROM. For non-RAM I/O,
 * keep using `ctx->mem->read32(addr)` / `write32(addr, val)`.
 *
 * **Convention:** `emuptr<T>(0)` is treated as null. `operator->` and
 * `operator*` on a null `emuptr` are undefined behaviour (debug builds
 * may assert). Test predicates via `null()` or `operator bool()`.
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ps1 {

namespace detail {
/// Process-wide RAM base pointer. Set by `emuptr_set_ram` at runtime
/// startup. Reads / writes through `emuptr<T>` resolve against this.
inline uint8_t *g_emuptr_ram = nullptr;
} // namespace detail

/// Bind the RAM buffer that all `emuptr<T>` instances will translate
/// against. Typically called once with `memory.ramPtr()`.
inline void emuptr_set_ram(uint8_t *ram) { detail::g_emuptr_ram = ram; }

/// Translate a PS1 virtual address into a host pointer inside the bound
/// RAM buffer. KUSEG/KSEG0/KSEG1 mirrors are folded; addresses below
/// 8 MB are masked to the 2 MB RAM window.
[[gnu::always_inline]]
inline void *emuptr_translate(uint32_t addr) {
  assert(detail::g_emuptr_ram != nullptr &&
         "emuptr_set_ram() must be called before any emuptr<T> dereference");
  uint32_t phys = addr & 0x1FFFFFFFu;
  if (phys < 0x00800000u) {
    phys &= 0x001FFFFFu;
  }
  return detail::g_emuptr_ram + phys;
}

/**
 * Typed PS1-RAM pointer. Stores a virtual address; resolves to a host
 * pointer on dereference.
 *
 * @tparam T POD-like layout type that maps to a PS1 struct or scalar.
 */
template <class T> class emuptr {
public:
  constexpr emuptr() noexcept = default;
  constexpr explicit emuptr(uint32_t addr) noexcept : addr_(addr) {}

  /// Raw address (KUSEG/KSEG0 form, exactly as stored).
  constexpr uint32_t addr() const noexcept { return addr_; }

  /// Explicit conversion to address. Use `static_cast<uint32_t>(p)` or
  /// `p.addr()` when an HLE body needs the raw virtual address (e.g. to
  /// pass to `ctx->mem->write32`). Explicit (not implicit) because the
  /// implicit form collides with built-in arithmetic when combined with
  /// the typed `operator+` / `operator-` overloads below.
  constexpr explicit operator uint32_t() const noexcept { return addr_; }

  /// True when the stored address is zero (project convention for null).
  constexpr bool null() const noexcept { return addr_ == 0; }
  constexpr explicit operator bool() const noexcept { return addr_ != 0; }

  /// Member access via the resolved host pointer.
  [[gnu::always_inline]] T *operator->() const noexcept {
    assert(addr_ != 0 && "emuptr<T>::operator->() on null pointer");
    return static_cast<T *>(emuptr_translate(addr_));
  }

  /// Dereference to a reference.
  [[gnu::always_inline]] T &operator*() const noexcept {
    assert(addr_ != 0 && "emuptr<T>::operator*() on null pointer");
    return *static_cast<T *>(emuptr_translate(addr_));
  }

  /// Array indexing (element stride is `sizeof(T)`).
  [[gnu::always_inline]] T &operator[](std::ptrdiff_t i) const noexcept {
    assert(addr_ != 0 && "emuptr<T>::operator[]() on null pointer");
    return *static_cast<T *>(
        emuptr_translate(addr_ + static_cast<uint32_t>(i * sizeof(T))));
  }

  // Pointer arithmetic. Strides are in units of `sizeof(T)`.

  constexpr emuptr operator+(std::ptrdiff_t n) const noexcept {
    return emuptr(addr_ + static_cast<uint32_t>(n * sizeof(T)));
  }
  constexpr emuptr operator-(std::ptrdiff_t n) const noexcept {
    return emuptr(addr_ - static_cast<uint32_t>(n * sizeof(T)));
  }
  constexpr emuptr &operator+=(std::ptrdiff_t n) noexcept {
    addr_ += static_cast<uint32_t>(n * sizeof(T));
    return *this;
  }
  constexpr emuptr &operator-=(std::ptrdiff_t n) noexcept {
    addr_ -= static_cast<uint32_t>(n * sizeof(T));
    return *this;
  }
  constexpr emuptr &operator++() noexcept {
    addr_ += sizeof(T);
    return *this;
  }
  constexpr emuptr operator++(int) noexcept {
    emuptr tmp(*this);
    addr_ += sizeof(T);
    return tmp;
  }
  constexpr emuptr &operator--() noexcept {
    addr_ -= sizeof(T);
    return *this;
  }
  constexpr emuptr operator--(int) noexcept {
    emuptr tmp(*this);
    addr_ -= sizeof(T);
    return tmp;
  }

  // Equality / ordering operate on the raw address.

  friend constexpr bool operator==(emuptr a, emuptr b) noexcept {
    return a.addr_ == b.addr_;
  }
  friend constexpr bool operator!=(emuptr a, emuptr b) noexcept {
    return a.addr_ != b.addr_;
  }
  friend constexpr bool operator<(emuptr a, emuptr b) noexcept {
    return a.addr_ < b.addr_;
  }

private:
  uint32_t addr_ = 0;
};

} // namespace ps1
