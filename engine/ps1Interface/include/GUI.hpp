#pragma once

#include "StudioState.hpp"

namespace GUI {
    void DrawStudio(StudioState& state);
    void RebuildFontsIfNeeded(StudioState& state);
    void ApplySettings(StudioState& state);
    bool WantsQuit();
}
