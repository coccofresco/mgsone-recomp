#pragma once
#include <cstdint>
#include <string_view>
struct recomp_context;
namespace ps1 { class Memory; }
namespace ps1::cdrom { class CdromController; }
namespace ps1::games::mgs {
inline constexpr std::string_view PROFILE_NAME = "mgs_usa_v1_0";
void configure(bool enabled, bool widescreen) noexcept;
bool active() noexcept;
bool widescreen16x9() noexcept;
// Cooperatively service one audio interrupt at a time through the retail MTS
// receiver. Pending sample credit stays in the SPU until SdInt can accept it.
bool audioClockReady(const Memory &memory, uint32_t irqCallback) noexcept;
void configureFastLoading(bool enabled, cdrom::CdromController *drive) noexcept;
bool fastDataAllowed(std::string_view path) noexcept;
} // namespace ps1::games::mgs
// Called at the fingerprint-verified DG_AdjustOverscan entry in the USA recipe.
void mgsAdjustProjection(recomp_context *ctx);
bool mgsTryFastStageSync(recomp_context *ctx);
void mgsReleaseFastPcxScratch(recomp_context *ctx);
