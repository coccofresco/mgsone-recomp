#pragma once
/**
 * @file psyq_libgpu.h
 * @brief Additional libgpu HLE entries detected on the Rayman boot path
 *        (Sessao 0.7).
 *
 * The 10 built-in display/OT helpers (`hle_VSync`, `hle_DrawSync`, etc.) live
 * in `psyq_hle.h`; this header covers primitive-init macros (SetPoly*,
 * SetSprt*, SetShadeTex) and the GetClut() helper that the hash matcher
 * detects in compiled libgpu.lib.
 */

#include "runtime/cpu_context.h"

namespace ps1::psyq {

/// GetClut(x, y) -> 16-bit CLUT id used in the GP0 textured-polygon command.
void hle_libgpu_GetClut(recomp_context *ctx);

/// SetShadeTex(p, tge): toggle bit 0 of the primitive's code byte
/// (raw-texture vs. modulated-texture).
void hle_libgpu_SetShadeTex(recomp_context *ctx);

void hle_libgpu_SetPolyF4(recomp_context *ctx);
void hle_libgpu_SetPolyFT4(recomp_context *ctx);
void hle_libgpu_SetSprt(recomp_context *ctx);
void hle_libgpu_SetSprt8(recomp_context *ctx);
void hle_libgpu_SetSprt16(recomp_context *ctx);

// Group 1.A -- display-area / VRAM-transfer / video-mode HLEs

/// SetDispMask(mask): GP1(0x03, mask) -- enable (1) / disable (0) display.
void hle_libgpu_SetDispMask(recomp_context *ctx);

/// LoadImage(rect*, src*): CPU->VRAM transfer via GP0(0xA0).
/// rect: int16 x,y,w,h.  src: packed 16bpp pixels, 2 per 32-bit word.
void hle_libgpu_LoadImage(recomp_context *ctx);

/// StoreImage(rect*, dst*): VRAM->CPU transfer via GP0(0xC0).
/// Drains GPUREAD into PS1 RAM at dst.
void hle_libgpu_StoreImage(recomp_context *ctx);

/// MoveImage(rect*, x, y): VRAM->VRAM blit via GP0(0x80).
void hle_libgpu_MoveImage(recomp_context *ctx);

/// ClearImage(rect*, r, g, b): GP0(0x02) FillRect with the given RGB color.
void hle_libgpu_ClearImage(recomp_context *ctx);

/// DrawSyncCallback(fn): install a "GPU drain done" callback.
/// Stubbed -- runtime GPU is synchronous, callback never fires.
/// Returns the previously installed callback (always 0).
void hle_libgpu_DrawSyncCallback(recomp_context *ctx);

/// VSyncCallback(fn): install a vertical-blank user callback.
/// Stubbed -- main_host owns the VBlank thread directly. Returns 0.
void hle_libgpu_VSyncCallback(recomp_context *ctx);

/// SetVideoMode(mode): 0=NTSC, 1=PAL. Stores in module state, returns prev.
void hle_libgpu_SetVideoMode(recomp_context *ctx);

/// GetVideoMode(): returns current video mode (0=NTSC, 1=PAL).
void hle_libgpu_GetVideoMode(recomp_context *ctx);

/// C++-side accessor for the current video mode (0=NTSC, 1=PAL), for other
/// HLE modules (PutDispEnv, SetDefDrawEnv) that must follow the same
/// PAL/NTSC branch the PsyQ source takes internally via GetVideoMode().
int getVideoMode();

/// Host-side region initialization before guest execution begins.
void setVideoMode(int mode);

// Group 1.A -- libgs scene-graph wrappers (NOP stubs)
//
// Both Rayman and Crash bypass libgs and drive libgpu directly. These stubs
// keep the build linkable for games that *do* link libgs (e.g. Tomba) and
// log a one-shot warning so we know if a real impl is needed later.

void hle_libgs_GsInitGraph(recomp_context *ctx);
void hle_libgs_GsDefDispBuff(recomp_context *ctx);
void hle_libgs_GsSetWorkBase(recomp_context *ctx);
void hle_libgs_GsSortClear(recomp_context *ctx);

void psyq_register_libgpu_extras();

} // namespace ps1::psyq
