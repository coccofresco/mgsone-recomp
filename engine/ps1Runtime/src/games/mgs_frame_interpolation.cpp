#include "runtime/games/mgs_frame_interpolation.h"
#include "runtime/metrics.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace ps1::games::mgs {
namespace {
using Uniforms = gpu::replay::vulkan_abi::DrawUniforms;
using Draw = gpu::replay::VulkanReplayBakedDraw;
bool compatibleCamera(const std::array<float, 16> &a,
                      const std::array<float, 16> &b) {
  // Conservative scene-wide barrier: cuts, reflection changes and teleports
  // snap atomically. Small camera motion is included in the source matrices.
  for (unsigned row = 0; row < 3; ++row) {
    double dot = 0, aa = 0, bb = 0;
    for (unsigned col = 0; col < 3; ++col) {
      const double x = a[col*4+row], y = b[col*4+row];
      if (!std::isfinite(x) || !std::isfinite(y)) return false;
      dot += x*y; aa += x*x; bb += y*y;
    }
    if (aa == 0 || bb == 0 || dot < .94 * std::sqrt(aa*bb)) return false;
    if (!std::isfinite(a[12+row]) || !std::isfinite(b[12+row]) ||
        std::abs(a[12+row]-b[12+row]) > 2000.f) return false;
  }
  return true;
}
bool compatibleFace(const Uniforms &a, const Uniforms &b) {
  if (a.mgsPointBytes != b.mgsPointBytes || a.mgsWeights != b.mgsWeights ||
      a.mgsProjection[3] != b.mgsProjection[3] ||
      a.mgsTextureData != b.mgsTextureData ||
      a.mgsAmbient[3] != b.mgsAmbient[3]) return false;
  if (std::abs(a.mgsProjection[0]-b.mgsProjection[0]) > 32) return false;
  for (unsigned corner = 0; corner < 4; ++corner)
    for (unsigned axis = 0; axis < 3; ++axis) {
      const float x=a.mgsWorldPositions[corner][axis], y=b.mgsWorldPositions[corner][axis];
      if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x-y)>1000.f) return false;
    }
  for (unsigned row=0; row<12; ++row)
    for (unsigned col=0; col<4; ++col)
      if (!std::isfinite(a.mgsViewRows[row][col]) ||
          !std::isfinite(b.mgsViewRows[row][col])) return false;
  return true;
}
template <size_t N> void blend(std::array<float,N> &b, const std::array<float,N> &a, float t) {
  for (size_t i=0;i<N;++i) b[i]=a[i]+(b[i]-a[i])*t;
}
template <size_t N> void blend(std::array<std::array<float,4>,N> &b,
                               const std::array<std::array<float,4>,N> &a,float t) {
  for (size_t i=0;i<N;++i) blend(b[i],a[i],t);
}
void blendFace(Uniforms &b, const Uniforms &a, float t) {
  blend(b.mgsViewRows,a.mgsViewRows,t);
  blend(b.mgsNormals,a.mgsNormals,t);
  blend(b.mgsWorldPositions,a.mgsWorldPositions,t);
  blend(b.mgsLightDirections,a.mgsLightDirections,t);
  blend(b.mgsLightColors,a.mgsLightColors,t);
  blend(b.mgsAmbient,a.mgsAmbient,t);
  b.mgsProjection[0]=a.mgsProjection[0]+(b.mgsProjection[0]-a.mgsProjection[0])*t;
  // Page origins, mask/texture state and effect/HUD commands belong to B.
  // Interpolating VRAM page flips would move the scene off the display.
}
}
void FrameInterpolator::reset() {
  previous_.clear(); camera_={}; presentationFrame_=epoch_=sourceFrame_=0;
}
std::optional<TemporalPair> FrameInterpolator::advance(
    const gpu::replay::VulkanReplayBakedBatch &batch,uint64_t frame) {
  std::map<Key,Uniforms> current;
  std::set<Key> duplicate;
  const Draw *first=nullptr;
  bool coherent=true;
  for (const auto &command:batch.commands) {
    const auto *draw=std::get_if<Draw>(&command.payload);
    if (!draw || !draw->sourceTopology || !draw->mgsTemporalIdentity[0] ||
        !draw->mgsTemporalIdentity[1] || !draw->segmentCount) continue;
    if (!first) first=draw;
    coherent &= first->mgsTemporalFrame==draw->mgsTemporalFrame &&
                first->mgsTemporalIdentity[0]==draw->mgsTemporalIdentity[0] &&
                first->mgsTemporalCamera==draw->mgsTemporalCamera;
    if (!current.emplace(draw->mgsTemporalIdentity,draw->segments[0].uniforms).second)
      duplicate.insert(draw->mgsTemporalIdentity);
  }
  if (!first) {
    // Pure latches preserve history; a drawn 2D scene (Codec/menu) ends it.
    if (std::any_of(batch.commands.begin(),batch.commands.end(),[](const auto &c){
          return std::holds_alternative<Draw>(c.payload); })) reset();
    return {};
  }
  if (!coherent) { reset(); return {}; }
  if (sourceFrame_==first->mgsTemporalFrame && epoch_==first->mgsTemporalIdentity[0])
    return {};
  for (const auto &key:duplicate) current.erase(key);
  const uint64_t delta=frame-presentationFrame_;
  const bool continuity=coherent && !previous_.empty() &&
      epoch_==first->mgsTemporalIdentity[0] && (delta==2 || delta==3) &&
      uint32_t(first->mgsTemporalFrame-sourceFrame_)==delta &&
      compatibleCamera(camera_,first->mgsTemporalCamera);
  TemporalPair pair;
  if (continuity) {
    pair.steps=uint32_t(delta);
    // The render target belongs to the DG draw area, not to the currently
    // displayed (usually opposite) page. Use its native origin, never blend it.
    pair.pageX=first->scissor.x/batch.internalScale;
    pair.pageY=first->scissor.y/batch.internalScale;
    for (const auto &command:batch.commands) {
      const auto *draw=std::get_if<Draw>(&command.payload);
      if (!draw) continue;
      const auto old=previous_.find(draw->mgsTemporalIdentity);
      const bool matched=draw->sourceTopology && current.contains(draw->mgsTemporalIdentity) &&
          old!=previous_.end() && compatibleFace(old->second,draw->segments[0].uniforms);
      if (matched) {
        ++pair.matchedFaces;
        pair.movingFaces += old->second.mgsViewRows!=draw->segments[0].uniforms.mgsViewRows;
      }
      for (unsigned s=0;s<draw->segmentCount;++s)
        for (unsigned phase=1;phase<pair.steps;++phase) {
          auto uniforms=draw->segments[s].uniforms;
          if (matched) blendFace(uniforms,old->second,float(phase)/pair.steps);
          pair.phases[phase-1].push_back(uniforms);
        }
    }
  } else metrics::count("mgs.interpolation.history_barrier");
  previous_=std::move(current); camera_=first->mgsTemporalCamera;
  sourceFrame_=first->mgsTemporalFrame; epoch_=first->mgsTemporalIdentity[0];
  presentationFrame_=frame;
  metrics::count("mgs.interpolation.endpoints");
  if (!pair.matchedFaces) return {};
  metrics::count("mgs.interpolation.matched_faces",pair.matchedFaces);
  metrics::count("mgs.interpolation.moving_faces",pair.movingFaces);
  return pair;
}
}
