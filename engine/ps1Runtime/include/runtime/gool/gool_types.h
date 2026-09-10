#pragma once
/**
 * @file gool_types.h
 * @brief PS1-memory layout structs for the GOOL engine, ported verbatim
 *        from the `c1c` hand-decompilation (src/gfx.h, src/ns.h,
 *        src/gool.h).
 *
 * These mirror the byte layout of structures that live in the Crash
 * Bandicoot game's RAM, so the ported GOOL interpreter can manipulate
 * them through `ps1::emuptr<T>` exactly as the original MIPS code did.
 *
 * **Byte-exactness is load-bearing.** Every field type and order matches
 * the decomp. `EMUPTR(T)` resolves to `ps1::emuptr<T>` (a 4-byte trivial
 * wrapper, ABI-identical to the PS1's 32-bit pointer). The `static_assert`
 * block at the end pins the field offsets the decomp annotated (0x104,
 * 0x130, ...); if a future edit perturbs the layout the build breaks
 * loudly instead of corrupting game state silently.
 *
 * Source of truth: ../PS1Recomp-workspace/c1c/src/{gfx,ns,gool}.h
 */

#include "runtime/emuptr.h"

#include <cstddef>
#include <cstdint>

/// In C++ the c1c `EMUPTR(x)` macro expands to `emuptr<x>`; here it maps
/// to this project's `ps1::emuptr<x>`. Kept as a macro so the ported
/// struct bodies stay textually faithful to the decomp.
#ifndef EMUPTR
#define EMUPTR(T) ::ps1::emuptr<T>
#endif

namespace ps1::gool {

// "unknown-purpose" scalar aliases from the decomp (c1c/src/emu.h).
using unk8_t = uint8_t;
using unk16_t = uint16_t;
using unk32_t = uint32_t;

// ---------------------------------------------------------------------
// gfx.h — geometry / colour primitives
// ---------------------------------------------------------------------

struct vector {
  int32_t x;
  int32_t y;
  int32_t z;
};
using point = vector; // c1c: #define point vector

struct svector {
  int16_t x;
  int16_t y;
  int16_t z;
};
using spoint = svector; // c1c: #define spoint svector

struct angle {
  int32_t y;
  int32_t x;
  int32_t z;
};

struct sangle {
  int16_t y;
  int16_t x;
  int16_t z;
};

struct bound {
  point p1;
  point p2;
};
using space = bound; // c1c: #define space bound

struct dimension {
  int32_t w;
  int32_t h;
  int32_t d;
};

struct volume {
  point pos;
  dimension dim;
};

struct matrix {
  vector v1;
  vector v2;
  vector v3;
};

struct smatrix {
  svector v1;
  svector v2;
  svector v3;
};
using slightmatrix = smatrix; // c1c: #define slightmatrix smatrix

struct scolor {
  uint16_t r;
  uint16_t g;
  uint16_t b;
};

struct scolormatrix {
  scolor v1;
  scolor v2;
  scolor v3;
};

// ---------------------------------------------------------------------
// ns.h — NaughtyStorage (paging) entries
// ---------------------------------------------------------------------

using nsitem = uint8_t;

struct nsentry {
  unk32_t magic;
  unk32_t id;
  unk32_t type;
  int32_t itemcount;
  EMUPTR(nsitem) items[]; // flexible array (GCC/Clang extension)
};

struct nspage {
  uint16_t magic;
  uint16_t type;
  unk32_t pagenum;
  int32_t entrycount;
  unk32_t checksum;
  EMUPTR(nsentry) entries[];
};

struct nspageinfo {
  EMUPTR(nspage) page;
  int16_t status;
  unk16_t off_6;
  int16_t unk_8;
  unk16_t off_10;
  int16_t savings;
  int16_t compressed;
  EMUPTR(nspage) nextpage;
  int32_t pagenum;
  unk32_t off_24;
  int32_t pagecount;
  unk32_t off_32;
  unk32_t off_36;
  unk32_t off_40;
};

// ---------------------------------------------------------------------
// gool.h — object engine structures
// ---------------------------------------------------------------------

struct goolobj; // self-referential

struct gooldbllink {
  EMUPTR(goolobj) prev;
  EMUPTR(goolobj) next;
  unk32_t unk_8;
};

struct goolobj {
  uint32_t header;
  union {
    uint32_t proctype;
    EMUPTR(goolobj) listchildren;
  };
  struct bound bound; // elaborated: member shares the type's name (C++)
  EMUPTR(nsentry) local;
  EMUPTR(nsentry) external;
  EMUPTR(nsentry) zone;
  uint32_t state;

  union {
    slightmatrix colors;
    slightmatrix lightmatrix;
  };
  scolor color;
  scolormatrix colormatrix;
  scolor intensity;

  EMUPTR(goolobj) self;
  EMUPTR(goolobj) parent;
  EMUPTR(goolobj) sibling;
  EMUPTR(goolobj) children;
  EMUPTR(goolobj) creator;
  EMUPTR(goolobj) player;
  EMUPTR(goolobj) collider;
  EMUPTR(goolobj) invoker;

  union {
    vector vectors;
    vector trans;
  };
  angle rot;
  vector scale;
  union {
    vector velocity;
    gooldbllink link;
  };
  angle targetrot;
  union {
    angle angvelocity;
    struct {
      uint32_t modeflagsa;
      uint32_t modeflagsb;
      uint32_t modeflagsc;
    };
  };

  uint32_t statusa;
  uint32_t statusb;
  uint32_t statusc;
  uint32_t subtype;
  uint32_t id;
  EMUPTR(uint32_t) sp;
  EMUPTR(uint32_t) pc;
  EMUPTR(uint32_t) fp;
  EMUPTR(uint32_t) pctrans;
  EMUPTR(uint32_t) pcevent;
  EMUPTR(uint32_t) pchead;
  union {
    uint32_t misc_flag;
    EMUPTR(goolobj) misc_child;
    EMUPTR(nsentry) misc_entry;
    uint32_t misc_memcard;
  };
  uint32_t unk_F8;
  uint32_t stampanim;
  uint32_t stampstate;
  uint32_t animcounter;     // 0x104
  EMUPTR(uint32_t) animseq; // 0x108
  uint32_t animframe;       // 0x10C

  EMUPTR(nsitem) entity;
  int32_t pathprogress;
  uint32_t pathcount;
  uint32_t groundy;
  uint32_t stateflags;
  int32_t speed;
  uint32_t displaymode;
  uint32_t unk_12C;
  uint32_t stampland;  // 0x130
  int32_t landyvel;    // 0x134
  uint32_t zindex;     // 0x138
  uint32_t event;      // 0x13C
  int32_t camzoom;     // 0x140
  uint32_t approachyz; // 0x144
  int32_t hotspotclip; // 0x148
  int32_t unk_14C;
  unk32_t unk_150;
  unk32_t unk_154;
  int32_t node;
  uint32_t memory[0x40];
};

struct goolobjlist {
  uint32_t header;
  EMUPTR(goolobj) children;
};

struct goolentity {
  EMUPTR(nsentry) parententry;
  uint16_t spawnflags;
  uint16_t proctype;
  uint16_t id;
  uint16_t pathlen;
  union {
    sangle rot;
    struct {
      uint16_t modeflagsa;
      uint16_t modeflagsb;
      uint16_t modeflagsc;
    };
  };
  uint8_t type;
  uint8_t subtype;
  spoint path[];
};

struct goolstateinfo {
  uint32_t flags;
  uint32_t statusc;
  uint16_t externeidrel;
  uint16_t event;
  uint16_t trans;
  uint16_t code;
};

struct goolstateref {
  uint32_t state;
  uint32_t guard;
};

struct gooleventpoll {
  union {
    uint32_t categories;
    struct {
      uint32_t unused_flags : 25;
      uint32_t sendtoenemiesc : 1;
      uint32_t sendtoenemiesb : 1;
      uint32_t sendtosprites : 1;
      uint32_t sendtoenemiesa : 1;
      uint32_t sendtopausemenu : 1;
      uint32_t sendtoplayer : 1;
      uint32_t unk_flag : 1;
    };
  };
  EMUPTR(goolobj) sender;
  EMUPTR(goolobj) collider;
  uint32_t distnearest;
  uint32_t event;
};

#define GOOL_OBJECT_LEVELSPAWNLISTSIZE 3592
#define GOOL_OBJECT_SPAWNLISTSIZE 304
#define GOOL_OBJECT_POOLSIZE 96

struct levelstate {
  vector playertrans;
  angle playerrot;
  vector playerscale;
  uint32_t currentzone;
  uint32_t currentsection;
  uint32_t currentprogress;
  uint32_t levelid;
  uint32_t flag;
  uint32_t spawnlist[GOOL_OBJECT_SPAWNLISTSIZE];
  uint32_t boxesbroken;
};

// ---------------------------------------------------------------------
// Layout guards — pin the offsets the decomp annotated. Both the PS1
// (MIPS o32) and host (x86-64) are little-endian with identical scalar
// sizes and natural alignment, so a verbatim port reproduces the layout.
// If any of these fire, the struct diverged from game memory.
// ---------------------------------------------------------------------

static_assert(sizeof(::ps1::emuptr<int>) == 4,
              "emuptr<T> must be 4 bytes to match the PS1 pointer width");
static_assert(sizeof(vector) == 12);
static_assert(sizeof(bound) == 24);
static_assert(sizeof(smatrix) == 18);
static_assert(sizeof(scolor) == 6);
static_assert(sizeof(scolormatrix) == 18);
static_assert(offsetof(goolobj, animcounter) == 0x104);
static_assert(offsetof(goolobj, animseq) == 0x108);
static_assert(offsetof(goolobj, animframe) == 0x10C);
static_assert(offsetof(goolobj, stampland) == 0x130);
static_assert(offsetof(goolobj, landyvel) == 0x134);
static_assert(offsetof(goolobj, zindex) == 0x138);
static_assert(offsetof(goolobj, event) == 0x13C);
static_assert(offsetof(goolobj, camzoom) == 0x140);
static_assert(offsetof(goolobj, approachyz) == 0x144);
static_assert(offsetof(goolobj, hotspotclip) == 0x148);
static_assert(offsetof(goolobj, node) == 0x158);
static_assert(offsetof(goolobj, memory) == 0x15C);
static_assert(sizeof(goolobj) == 0x25C, "goolobj must match PS1 layout (no trailing padding)");

} // namespace ps1::gool
