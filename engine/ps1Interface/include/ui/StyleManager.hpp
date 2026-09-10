#pragma once

#include "StudioState.hpp"

namespace StyleManager {
    void SetupFonts(const AppSettings& settings);
    void ApplyTheme(ThemeMode theme, const AppSettings& settings);
}
