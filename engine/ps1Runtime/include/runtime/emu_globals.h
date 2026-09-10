#pragma once
/**
 * @file emu_globals.h
 * @brief Macros for declaring typed accessors for fixed PS1 BSS addresses.
 *
 * Game-side global variables live at fixed PS1 virtual addresses (usually
 * in BSS, after the `__bss_end` marker). Hand-written HLE bodies need to
 * read and write them as if they were ordinary C++ variables.
 *
 * `EMUGLOBALVAR(type, name, addr)` declares an inline accessor that
 * returns a reference into the bound RAM buffer:
 *
 *     // declares: inline uint32_t& hash_table_base();
 *     EMUGLOBALVAR(uint32_t, hash_table_base, 0x8005C530);
 *
 *     // usage:
 *     hash_table_base() = nspage_ptr;
 *
 * `EMUGLOBALARR(type, name, addr)` is the array form — returns a `T*`
 * so callers can index normally:
 *
 *     EMUGLOBALARR(emuptr<nspageinfo>, nspageinfo_pool, 0x800573A0);
 *     auto p = nspageinfo_pool()[7];
 *
 * The accessors share the same RAM binding as `emuptr<T>`; both require
 * `emuptr_set_ram()` to have been called.
 */

#include "runtime/emuptr.h"

/// Declares `inline type& name()` that returns the value stored at the
/// fixed PS1 virtual address `addr`. Use for individual globals.
#define EMUGLOBALVAR(type, name, addr)                                         \
  [[gnu::always_inline]] inline type &name() noexcept {                        \
    return *static_cast<type *>(::ps1::emuptr_translate(                       \
        static_cast<uint32_t>(addr)));                                         \
  }

/// Declares `inline type* name()` that returns a pointer to the array
/// starting at the fixed PS1 virtual address `addr`. Use for tables.
#define EMUGLOBALARR(type, name, addr)                                         \
  [[gnu::always_inline]] inline type *name() noexcept {                        \
    return static_cast<type *>(                                                \
        ::ps1::emuptr_translate(static_cast<uint32_t>(addr)));                 \
  }
