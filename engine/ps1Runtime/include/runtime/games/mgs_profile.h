#pragma once
#include <string_view>
struct recomp_context;
namespace ps1::games::mgs {
inline constexpr std::string_view PROFILE_NAME = "mgs_usa_v1_0";
void configure(bool enabled, bool widescreen) noexcept;
bool widescreen16x9() noexcept;
} // namespace ps1::games::mgs
// Called at the fingerprint-verified DG_AdjustOverscan entry in the USA recipe.
void mgsAdjustProjection(recomp_context *ctx);
