#include "runtime/gpu/display_geometry.h"
#include <gtest/gtest.h>

using namespace ps1::gpu;

namespace {

GPU::DisplayState crashPalDisplayState() {
  GPU::DisplayState state;
  state.xStart = 512;
  state.yStart = 0;
  state.gpuStat = (2u << 17) | (1u << 20); // 512-wide PAL
  state.modeSet = true;
  state.areaSet = true;
  state.displayX1 = 608;
  state.displayX2 = 3168;
  state.displayY1 = 37;
  state.displayY2 = 293;
  state.horizontalRangeSet = true;
  state.verticalRangeSet = true;
  return state;
}

} // namespace

TEST(GpuDisplayGeometryTest, UsesCrashPalProgrammedApertureWithoutCropping) {
  const GPU::DisplayState state = crashPalDisplayState();

  const DisplayAperture aperture = deriveDisplayAperture(state);
  EXPECT_EQ(aperture.x, 512u);
  EXPECT_EQ(aperture.y, 0u);
  EXPECT_EQ(aperture.width, 512u);
  EXPECT_EQ(aperture.height, 256u);
  EXPECT_TRUE(aperture.usedHorizontalRange);
  EXPECT_TRUE(aperture.usedVerticalRange);
  EXPECT_TRUE(aperture.enabled);
}

TEST(GpuDisplayGeometryTest, PlansCrashPalProgrammedCanvasWithoutBorders) {
  const RawDisplayCrtcInput input = rawDisplayCrtcInput(crashPalDisplayState());
  const DisplayScanoutPlan plan =
      deriveDisplayScanoutPlan(input, DisplayAreaPolicy::Programmed);

  EXPECT_EQ(plan.canvas.width, 512u);
  EXPECT_EQ(plan.canvas.height, 256u);
  EXPECT_EQ(plan.destinationOrigin.x, 0u);
  EXPECT_EQ(plan.destinationOrigin.y, 0u);
  EXPECT_EQ(plan.activeExtent.width, 512u);
  EXPECT_EQ(plan.activeExtent.height, 256u);
  EXPECT_EQ(plan.sourceByteX, 1024u);
  EXPECT_EQ(plan.sourceY, 0u);
  EXPECT_EQ(plan.rowStep, 1u);
  EXPECT_EQ(plan.format, DisplayScanoutFormat::RGB555);
  EXPECT_TRUE(plan.progressive);
  EXPECT_EQ(plan.field, DisplayScanoutField::NotApplicable);
  EXPECT_TRUE(plan.enabled);
}

TEST(GpuDisplayGeometryTest, PlacesCrashPalInSwanStationDonorCanvas) {
  RawDisplayCrtcInput input = rawDisplayCrtcInput(crashPalDisplayState());
  input.signalProfile = DisplaySignalProfile::SwanStationNoCrop;
  const DisplayScanoutPlan plan =
      deriveDisplayScanoutPlan(input, DisplayAreaPolicy::DonorCanvas);

  // PAL active signal 487..3282 / dot-divider 5 and lines 20..308.
  EXPECT_EQ(plan.canvas.width, 559u);
  EXPECT_EQ(plan.canvas.height, 288u);
  EXPECT_EQ(plan.destinationOrigin.x, 23u);
  EXPECT_EQ(plan.destinationOrigin.y, 17u);
  EXPECT_EQ(plan.activeExtent.width, 512u);
  EXPECT_EQ(plan.activeExtent.height, 256u);
}

TEST(GpuDisplayGeometryTest, PlacesCrashPalInBeetleDonorCanvas) {
  RawDisplayCrtcInput input = rawDisplayCrtcInput(crashPalDisplayState());
  input.signalProfile = DisplaySignalProfile::BeetleUncropped;
  const DisplayScanoutPlan plan =
      deriveDisplayScanoutPlan(input, DisplayAreaPolicy::DonorCanvas);

  EXPECT_EQ(plan.canvas.width, 560u);
  EXPECT_EQ(plan.canvas.height, 288u);
  EXPECT_EQ(plan.destinationOrigin.x, 24u);
  EXPECT_EQ(plan.destinationOrigin.y, 17u);
  EXPECT_EQ(plan.activeExtent.width, 512u);
  EXPECT_EQ(plan.activeExtent.height, 256u);
}

TEST(GpuDisplayGeometryTest, DonorCanvasRoundsProgrammedEndpointToFourPixels) {
  GPU::DisplayState state = crashPalDisplayState();
  // Both endpoints first align to the 5-tick dot clock.  This is 511 pixels
  // before the CRTC's nearest-four-pixel output rule expands it to 512.
  state.displayX2 = 3161;

  const DisplayScanoutPlan plan = deriveDisplayScanoutPlan(
      rawDisplayCrtcInput(state), DisplayAreaPolicy::DonorCanvas);
  EXPECT_EQ(plan.activeExtent.width, 512u);
}

TEST(GpuDisplayGeometryTest, PackedRgb888KeepsBytePhaseAfterSignalCrop) {
  GPU::DisplayState state = crashPalDisplayState();
  state.xStart = 9; // GP1(05) bit zero is ignored: effective word X is 8.
  state.gpuStat |= 1u << 21;
  state.displayX1 = 477; // floor to 475: two pixels before PAL active start.
  state.displayX2 = 607;

  const RawDisplayCrtcInput input = rawDisplayCrtcInput(state);
  ASSERT_EQ(input.sourceWordX, 8u);
  const DisplayScanoutPlan plan =
      deriveDisplayScanoutPlan(input, DisplayAreaPolicy::DonorCanvas);

  EXPECT_EQ(plan.format, DisplayScanoutFormat::PackedRGB888);
  EXPECT_EQ(plan.sourceByteX, 22u); // 8 words * 2 + 2 pixels * 3.
  EXPECT_EQ(plan.sourceByteX % 3u, 1u);
}

TEST(GpuDisplayGeometryTest, SourceCoordinatesWrapAtVramByteAndRowEdges) {
  GPU::DisplayState state = crashPalDisplayState();
  state.xStart = 1023; // effective X=1022 -> byte 2044.
  state.yStart = 511;
  state.gpuStat |= 1u << 21;
  state.displayX1 = 477; // two skipped pixels: 2044 + 6 wraps to 2.
  state.displayY1 = 19;  // one skipped row: 511 + 1 wraps to 0.

  const DisplayScanoutPlan plan = deriveDisplayScanoutPlan(
      rawDisplayCrtcInput(state), DisplayAreaPolicy::DonorCanvas);
  EXPECT_EQ(plan.sourceByteX, 2u);
  EXPECT_EQ(plan.sourceY, 0u);
}

TEST(GpuDisplayGeometryTest, DisplayOffRetainsGeometryButDisablesScanout) {
  GPU::DisplayState state = crashPalDisplayState();
  state.gpuStat |= 1u << 23;

  const DisplayScanoutPlan plan = deriveDisplayScanoutPlan(
      rawDisplayCrtcInput(state), DisplayAreaPolicy::DonorCanvas);
  EXPECT_FALSE(plan.enabled);
  EXPECT_EQ(plan.canvas.width, 559u);
  EXPECT_EQ(plan.activeExtent.width, 512u);
  EXPECT_FALSE(deriveDisplayAperture(state).enabled);
}

TEST(GpuDisplayGeometryTest,
     ProgrammedEmptyRangesDisableInsteadOfGoingNominal) {
  for (const bool inverted : {false, true}) {
    SCOPED_TRACE(inverted ? "inverted" : "equal");
    GPU::DisplayState state = crashPalDisplayState();
    state.displayX2 = inverted ? state.displayX1 - 1u : state.displayX1;
    state.displayY2 = inverted ? state.displayY1 - 1u : state.displayY1;

    const RawDisplayCrtcInput input = rawDisplayCrtcInput(state);
    const DisplayScanoutPlan programmed =
        deriveDisplayScanoutPlan(input, DisplayAreaPolicy::Programmed);
    EXPECT_EQ(programmed.activeExtent.width, 0u);
    EXPECT_EQ(programmed.activeExtent.height, 0u);

    const DisplayScanoutPlan fullSignal =
        deriveDisplayScanoutPlan(input, DisplayAreaPolicy::DonorCanvas);
    EXPECT_EQ(fullSignal.activeExtent.width, 0u);
    EXPECT_EQ(fullSignal.activeExtent.height, 0u);
    EXPECT_EQ(fullSignal.canvas.width, 559u);
    EXPECT_EQ(fullSignal.canvas.height, 288u);

    const DisplayAperture aperture =
        deriveDisplayAperture(state, DisplayAreaPolicy::Programmed);
    EXPECT_FALSE(aperture.enabled);
    EXPECT_EQ(aperture.width, 1u);
    EXPECT_EQ(aperture.height, 1u);
    EXPECT_TRUE(aperture.usedHorizontalRange);
    EXPECT_TRUE(aperture.usedVerticalRange);
  }
}

TEST(GpuDisplayGeometryTest, InterlacedGuestFieldRemainsExplicitlyUnknown) {
  GPU::DisplayState state = crashPalDisplayState();
  state.gpuStat |= (1u << 19) | (1u << 22);

  const DisplayScanoutPlan plan = deriveDisplayScanoutPlan(
      rawDisplayCrtcInput(state), DisplayAreaPolicy::Programmed);
  EXPECT_FALSE(plan.progressive);
  EXPECT_EQ(plan.field, DisplayScanoutField::Unknown);
  EXPECT_EQ(plan.rowStep, 2u);
}

TEST(GpuDisplayGeometryTest, FallsBackWhenRangesWereNotProgrammed) {
  GPU::DisplayState state;
  state.gpuStat = 1u << 17; // 320-wide
  state.modeSet = true;
  state.displayX1 = 0;
  state.displayX2 = 256;
  state.displayY1 = 0;
  state.displayY2 = 240;

  const DisplayAperture aperture = deriveDisplayAperture(state);
  EXPECT_EQ(aperture.width, 320u);
  EXPECT_EQ(aperture.height, 240u);
  EXPECT_FALSE(aperture.usedHorizontalRange);
  EXPECT_FALSE(aperture.usedVerticalRange);
}

TEST(GpuDisplayGeometryTest, DoublesProgrammedScanlinesFor480Interlace) {
  GPU::DisplayState state;
  state.gpuStat = (1u << 19) | (1u << 22);
  state.modeSet = true;
  state.displayY1 = 20;
  state.displayY2 = 260;
  state.verticalRangeSet = true;

  const DisplayAperture aperture = deriveDisplayAperture(state);
  EXPECT_EQ(aperture.height, 480u);
}

TEST(GpuDisplayGeometryTest, NominalPolicyIgnoresProgrammedOverscan) {
  GPU::DisplayState state;
  state.gpuStat = 2u << 17;
  state.modeSet = true;
  state.displayY1 = 0;
  state.displayY2 = 256;
  state.verticalRangeSet = true;

  const DisplayAperture aperture =
      deriveDisplayAperture(state, DisplayAreaPolicy::Nominal);
  EXPECT_EQ(aperture.width, 512u);
  EXPECT_EQ(aperture.height, 240u);
}

TEST(GpuDisplayGeometryTest, FitsFourByThreeIntoWideDrawable) {
  const OutputViewport viewport = fitDisplayViewport(1280, 720);
  EXPECT_EQ(viewport.x, 160);
  EXPECT_EQ(viewport.y, 0);
  EXPECT_EQ(viewport.width, 960);
  EXPECT_EQ(viewport.height, 720);
}

TEST(GpuDisplayGeometryTest, FitsFourByThreeIntoTallDrawable) {
  const OutputViewport viewport = fitDisplayViewport(800, 800);
  EXPECT_EQ(viewport.x, 0);
  EXPECT_EQ(viewport.y, 100);
  EXPECT_EQ(viewport.width, 800);
  EXPECT_EQ(viewport.height, 600);
}

TEST(GpuDisplayGeometryTest, FitsSixteenByNineWithoutPillarboxing) {
  const OutputViewport viewport = fitDisplayViewport(1280, 720, 16, 9);
  EXPECT_EQ(viewport.x, 0);
  EXPECT_EQ(viewport.y, 0);
  EXPECT_EQ(viewport.width, 1280);
  EXPECT_EQ(viewport.height, 720);
}
