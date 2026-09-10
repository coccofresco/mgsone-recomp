#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace ps1::gpu::replay::vulkan_abi {

// One fixed domain must be shared by every replay draw: scaling relative to a
// packet-local maximum would make D32 values from different packets
// incomparable.  The conservative 2^32 ceiling includes raw GTE W well beyond
// the normalized-one fallback (32768) without saturating those values.
inline constexpr uint64_t PROJECTIVE_W_DEPTH_DOMAIN_MAX = uint64_t{1} << 32;
inline constexpr float PROJECTIVE_W_TO_DEPTH_SCALE = 0x1p-32f;

[[nodiscard]] constexpr bool
isProjectiveWInDepthDomain(double projectiveW) noexcept {
  // These comparisons also reject NaN and infinities without relying on a
  // non-constexpr classification helper.
  return projectiveW > 0.0 &&
         projectiveW <= static_cast<double>(PROJECTIVE_W_DEPTH_DOMAIN_MAX);
}

[[nodiscard]] constexpr std::optional<float>
tryNormalizeProjectiveWDepth(double projectiveW) noexcept {
  if (!isProjectiveWInDepthDomain(projectiveW)) {
    return std::nullopt;
  }

  // A fixed linear transform preserves cross-packet ordering.  Rejection at
  // the domain boundary is intentional: callers must never silently clamp an
  // out-of-domain W onto a valid D32 value.
  return static_cast<float>(projectiveW *
                            static_cast<double>(PROJECTIVE_W_TO_DEPTH_SCALE));
}

// Must remain byte-for-byte identical to CrashReplayDrawState in
// replay_precise.vert/frag. The six-vector prefix remains compatible with
// existing fragment/Crash shaders; the appended fields are MGS vertex state.
struct alignas(16) DrawUniforms {
  std::array<float, 4> positionScaleBias{};
  std::array<float, 4> depthScaleBias{};
  std::array<float, 4> uvDomain{};
  std::array<uint32_t, 4> textureState{};
  std::array<uint32_t, 4> textureWindow{};
  std::array<uint32_t, 4> destinationState{};
  std::array<std::array<float, 4>, 12> mgsViewRows{};
  std::array<uint32_t, 4> mgsPointBytes{};
  std::array<float, 4> mgsProjection{};
};

// Binding offsets for replay_precise.vert:
//   0 = vec2 position, 1 = float W, 2 = vec2 UV, 3 = vec3 RGB255.
struct Vertex {
  std::array<float, 2> positionNative{};
  float projectiveW = 32768.0f;
  std::array<float, 2> uv{};
  std::array<float, 3> color255{};
};

// Host-scene geometry uses the same geometric payload as ordinary replay,
// but its material and presentation-depth state are intrinsic per triangle.
// Carrying that state on all three vertices lets an already ordered triangle
// stream cross Vulkan in one draw instead of issuing one dynamic-UBO draw per
// face. The shader declares these fields flat, so no material value is ever
// interpolated. Keeping this a separate ABI also leaves donor replay packets
// and their compact 32-byte vertex untouched.
struct SidecarVertex {
  Vertex geometry{};
  std::array<float, 4> uvDomain{};
  std::array<uint32_t, 4> textureState{};
  std::array<uint32_t, 4> textureWindow{};
  std::array<uint32_t, 4> presentationDepthBits{};
};

// Compact table entry for opaque GPU-driven sidecars. Geometry remains in the
// ordinary 32-byte vertex stream; one uint material index per triangle points
// at this 64-byte record. The vertex shader pulls the record from binding 1,
// avoiding three copies of flat state per triangle.
struct alignas(16) SidecarMaterial {
  std::array<float, 4> uvDomain{};
  std::array<uint32_t, 4> textureState{};
  std::array<uint32_t, 4> textureWindow{};
  std::array<uint32_t, 4> presentationDepthBits{};
};

// Previous endpoint for one compact projected vertex. The page origin is
// integral in native VRAM pixels, so packing its 11-bit X and 10-bit Y beside
// the validity bit keeps the record at one aligned vec4. The FP64 vertex
// variant reconstructs the same homogeneous midpoint previously built on the
// CPU; an invalid record makes the complete triangle retain its current pose.
inline constexpr uint32_t SIDECAR_PREVIOUS_ORIGIN_X_MASK = 0x7ffu;
inline constexpr uint32_t SIDECAR_PREVIOUS_ORIGIN_Y_MASK = 0x3ffu;
inline constexpr uint32_t SIDECAR_PREVIOUS_ORIGIN_Y_SHIFT = 11u;
inline constexpr uint32_t SIDECAR_PREVIOUS_VALID = UINT32_C(1) << 31u;
inline constexpr uint32_t SIDECAR_PREVIOUS_WORD_OFFSET_DISABLED = UINT32_MAX;

struct alignas(16) SidecarPreviousVertex {
  std::array<float, 3> positionW{};
  uint32_t packedOriginAndFlags = 0u;
};

// Presentation-only compute pass. Source and destination name non-overlapping
// ranges of the same frame upload buffer. The CPU supplies only endpoint-B
// vertices plus endpoint-A records; Vulkan copies the immutable payload and
// writes the homogeneous in-between X/Y/W directly into the destination.
// A dispatch always covers a triangle list so a numerically invalid corner
// makes the complete primitive snap to endpoint B.
struct SidecarMidpointPushConstants {
  uint32_t sourceVertexWordOffset = 0u;
  uint32_t destinationVertexWordOffset = 0u;
  uint32_t previousVertexWordOffset = 0u;
  uint32_t vertexCount = 0u;
  uint32_t vertexStrideWords = 0u;
  uint32_t currentOriginX = 0u;
  uint32_t currentOriginY = 0u;
  uint32_t reserved = 0u;
};

// Transitional raw/model-space vertex for the Crash 3 GPU scene path. A set
// high marker in positionOrModel[2] means word 0 contains signed model X/Y and
// the low half of word 2 contains signed model Z. Otherwise words 0..2 are the
// exact projected float X/Y/W oracle used for geometrically clipped vertices.
// UV and colour remain lossless floats while this path is certified; the next
// device-local topology stage can compact only fields proven to be integral.
inline constexpr uint32_t SIDECAR_MODEL_SPACE_VERTEX = UINT32_C(1) << 31u;

struct SidecarModelVertex {
  std::array<uint32_t, 3> positionOrModel{};
  std::array<float, 2> uv{};
  std::array<float, 3> color255{};
};

// std430 frame block read by crash3_sidecar_model_pulled.vert. Promoting the
// 3x3 GTE rotation to signed 32-bit lanes keeps its storage layout explicit;
// the shader widens every multiply/add to int64 before the FP64 perspective
// divide. presentationState.x is the exact horizontal presentation scale.
struct alignas(16) SidecarModelFrameState {
  std::array<std::array<int32_t, 4>, 3> rotation{};
  std::array<int32_t, 4> translationProjection{};
  std::array<float, 4> presentationState{};
};

struct SidecarModelPullPushConstants {
  uint32_t materialIndexWordOffset = 0;
  uint32_t materialWordOffset = 0;
  uint32_t modelVertexWordOffset = 0;
  uint32_t frameStateWordOffset = 0;
  uint32_t firstVertex = 0;
  uint32_t previousFrameStateWordOffset = UINT32_MAX;
  uint32_t previousModelVertexWordOffset = UINT32_MAX;
  uint32_t flags = 0;
};

// Persistent raw-scene ABI. Vertices and topology live in a device-local
// buffer; only selected-face/material state, resource placement and the
// current camera/deformation block cross the host-visible frame buffer.
inline constexpr uint32_t SIDECAR_RAW_VERTEX_COLOR_VALID =
    UINT32_C(1) << 31u;
inline constexpr uint32_t SIDECAR_RAW_FACE_PROJECTED_FALLBACK =
    UINT32_C(1) << 31u;
// The high material-index bit is presentation-only temporal metadata. The raw
// shader masks it before addressing the material table and consults the
// previous frame block only for faces carrying this exact correspondence proof.
inline constexpr uint32_t SIDECAR_RAW_FACE_INTERPOLATE_PREVIOUS =
    UINT32_C(1) << 31u;
// Direct material records remove the per-frame SidecarMaterial table from the
// dominant raw opaque path. Two layouts share the same six payload words.
// Legacy-direct records retain one uint16 U/V pair per triangle corner and two
// uint16 domain pairs. GPU-UV records preserve all four source packet UVs as
// exact PSX bytes so Vulkan can evaluate the donor flipped-quad correction
// from the current projected geometry instead of freezing a CPU decision:
//   0 = texture page | CLUT << 16
//   1 = packet UV0 | packet UV1 << 16 (each UV is U8 | V8 << 8)
//   2 = packet UV2 | packet UV3 << 16
//   3 = minU | minV << 8 | maxU << 16 | maxV << 24
// Vulkan derives draw flags, texture state and the resident/background depth
// class from these bits plus the one common draw UBO.
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_MATERIAL =
    UINT32_C(1) << 30u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_TEXTURED = UINT32_C(1) << 0u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_BACKGROUND =
    UINT32_C(1) << 1u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_GPU_UV = UINT32_C(1) << 2u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_QUAD_HALF_ONE =
    UINT32_C(1) << 3u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_QUAD_HALF_TWO =
    UINT32_C(1) << 4u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_SEMI_TRANSPARENT = UINT32_C(1)
                                                                     << 5u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_ABR_SHIFT = 6u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_ABR_MASK =
    UINT32_C(3) << SIDECAR_RAW_FACE_DIRECT_ABR_SHIFT;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_FORCE_MASK = UINT32_C(1)
                                                               << 8u;
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_CHECK_MASK = UINT32_C(1)
                                                               << 9u;
// Projected-fallback vertices already carry exact floating-point UV. This
// layout uses payload words 1..4 for four bit-exact float domain components,
// avoiding a second expanded SidecarVertex stream for UVs outside U16.
inline constexpr uint32_t SIDECAR_RAW_FACE_DIRECT_FLOAT_DOMAIN = UINT32_C(1)
                                                                 << 10u;
// A raw pull normally addresses its selected-face records in the mapped
// per-frame SSBO. Once an exact plan revision has been committed beside the
// immutable scene, this bit redirects the same word offset to binding 2.
inline constexpr uint32_t SIDECAR_RAW_PULL_STATIC_FACE = UINT32_C(1) << 31u;
// In an all-direct persistent draw material_word_offset is otherwise unused.
// This tag instead points it at a compact per-midpoint interpolation bitset in
// the frame SSBO (one bit per selected face).
inline constexpr uint32_t SIDECAR_RAW_PULL_INTERPOLATION_BITS =
    UINT32_C(1) << 31u;

struct alignas(16) SidecarRawVertex {
  uint32_t rawXy = 0;
  uint32_t rawZAndFlags = 0;
  uint32_t packedColor = 0;
  uint32_t reserved = 0;
};

struct alignas(16) SidecarRawTopologyFace {
  std::array<uint32_t, 3> vertexIndices{};
  uint32_t resourceIndex = 0;
  // Canonical {a,b,c,d} source indices. Triangles repeat their final source
  // index. The raw shader uses the first three only for the exact above-native
  // flipped-quad predicate, leaving ordinary triangle projection unchanged.
  std::array<uint32_t, 4> completeVertexIndices{};
};

// One record per selected resident triangle. topologyOrFallback addresses the
// device-local topology table, or an exact projected three-vertex fallback
// for a CPU-clipped near-plane fan. UV remains float-exact for this migration
// stage; material state stays in the shared compact SidecarMaterial table.
struct alignas(16) SidecarRawFace {
  uint32_t topologyOrFallback = 0;
  uint32_t materialIndex = 0;
  // Legacy records store six float bit patterns. Direct-material records use
  // the packed integer layout above. Keeping one raw word array makes both
  // interpretations explicit and preserves the proven 32-byte ABI.
  std::array<uint32_t, 6> payload{};
};

struct alignas(16) SidecarRawResourceInstance {
  uint32_t translationXy = 0;
  int32_t translationZ = 0;
  uint32_t flags = 0;
  uint32_t reserved = 0;
};

// Explicit std430 layout consumed by crash3_sidecar_raw_pulled.vert.
// presentationState = {horizontal scale, page X, page Y, reserved};
// deformationState = {active|distance-cue flags, time, phaseShift,
// vectorShift}; deformationProgram = {bias, triangle handler, quad handler,
// reserved}. The final wave vector is padded after the exact 17 samples.
struct alignas(16) SidecarRawFrameState {
  std::array<std::array<int32_t, 4>, 3> rotation{};
  std::array<int32_t, 4> translationProjection{};
  std::array<float, 4> presentationState{};
  std::array<int32_t, 4> farColor{};
  std::array<uint32_t, 4> deformationState{};
  std::array<uint32_t, 4> deformationProgram{};
  std::array<std::array<int32_t, 4>, 5> waveSamples{};
};

struct SidecarRawPullPushConstants {
  uint32_t materialWordOffset = 0;
  uint32_t faceWordOffset = 0;
  uint32_t frameStateWordOffset = 0;
  uint32_t resourceWordOffset = 0;
  uint32_t fallbackModelVertexWordOffset = 0;
  uint32_t staticTopologyWordOffset = 0;
  uint32_t staticVertexWordOffset = 0;
  // UINT32_MAX selects the exact current endpoint. Otherwise this addresses a
  // previous SidecarRawFrameState immediately followed by its resource table.
  uint32_t previousFrameStateWordOffset = UINT32_MAX;
};

// Word offsets address the frame upload buffer exposed as a std430 uint array.
// firstVertex removes Vulkan's non-zero draw base before dividing by three.
// previousVertexWordOffset selects an optional four-word record per vertex;
// UINT32_MAX keeps the exact current endpoint path.
struct SidecarPullPushConstants {
  uint32_t materialIndexWordOffset = 0;
  uint32_t materialWordOffset = 0;
  uint32_t firstVertex = 0;
  uint32_t previousVertexWordOffset =
      SIDECAR_PREVIOUS_WORD_OFFSET_DISABLED;
};

// Must remain byte-for-byte identical to ReplayScratchCopyState in
// replay_copy_scratch.frag. Coordinates are scaled atlas pixels.
struct ScratchCopyPushConstants {
  // xy = logical source-scratch origin, zw = atlas destination origin.
  std::array<uint32_t, 4> sourceDestination{};
  // xy = region extent, z = force-mask, w = check-destination-mask.
  std::array<uint32_t, 4> extentFlags{};
};

// Vertex/fragment ABI for the opt-in visibility oracle.
// state = {oneBasedOwnerToken, passEvidenceBits, invalidateDepth,
//          floatBits(homogeneousWMultiplier)}.
// The diagnostic renderer submits one triangle per draw, so a token never
// depends on gl_PrimitiveID resets at segment boundaries.
struct VisibilityOwnerPushConstants {
  std::array<uint32_t, 4> state{};
};

// Independent occlusion-D32 and presented-surface-R32 affine remaps for
// replay_precise.vert. The shader keeps original clip X/Y/W (and therefore
// exact coverage and UV interpolation). Identity/zero is fail-closed.
struct PresentationDepthPushConstants {
  uint32_t occlusionMultiplierBits = 0x3f800000u;
  uint32_t occlusionBiasBits = 0u;
  uint32_t surfaceMultiplierBits = 0x3f800000u;
  uint32_t surfaceBiasBits = 0u;
};

// Storage-buffer ABI for the opt-in accepted-fragment stack. The descriptor
// begins at VisibilityStackHeader; records follow immediately in std430.
// Capacity is a memory bound only. The shader increments attemptedRecords for
// every accepted fragment and sets overflow if a record cannot be retained.
struct alignas(16) VisibilityStackHeader {
  uint32_t attemptedRecords = 0;
  uint32_t recordCapacity = 0;
  uint32_t overflow = 0;
  uint32_t schema = 1;
  std::array<uint32_t, 4> atlasRegion{};
};

struct alignas(16) VisibilityStackRecord {
  uint32_t localSample = 0;
  uint32_t owner = 0;
  uint32_t depthBits = 0;
  uint32_t operation = 0;
};

static_assert(std::is_standard_layout_v<DrawUniforms>);
static_assert(std::is_trivially_copyable_v<DrawUniforms>);
static_assert(alignof(DrawUniforms) == 16);
static_assert(sizeof(DrawUniforms) == 320);
static_assert(offsetof(DrawUniforms, mgsViewRows) == 96);
static_assert(offsetof(DrawUniforms, mgsPointBytes) == 288);
static_assert(offsetof(DrawUniforms, mgsProjection) == 304);
static_assert(offsetof(DrawUniforms, positionScaleBias) == 0);
static_assert(offsetof(DrawUniforms, depthScaleBias) == 16);
static_assert(offsetof(DrawUniforms, uvDomain) == 32);
static_assert(offsetof(DrawUniforms, textureState) == 48);
static_assert(offsetof(DrawUniforms, textureWindow) == 64);
static_assert(offsetof(DrawUniforms, destinationState) == 80);

static_assert(std::is_standard_layout_v<Vertex>);
static_assert(std::is_trivially_copyable_v<Vertex>);
static_assert(sizeof(Vertex) == 32);
static_assert(offsetof(Vertex, positionNative) == 0);
static_assert(offsetof(Vertex, projectiveW) == 8);
static_assert(offsetof(Vertex, uv) == 12);
static_assert(offsetof(Vertex, color255) == 20);

static_assert(std::is_standard_layout_v<SidecarVertex>);
static_assert(std::is_trivially_copyable_v<SidecarVertex>);
static_assert(sizeof(SidecarVertex) == 96);
static_assert(offsetof(SidecarVertex, geometry) == 0);
static_assert(offsetof(SidecarVertex, uvDomain) == 32);
static_assert(offsetof(SidecarVertex, textureState) == 48);
static_assert(offsetof(SidecarVertex, textureWindow) == 64);
static_assert(offsetof(SidecarVertex, presentationDepthBits) == 80);

static_assert(std::is_standard_layout_v<SidecarMaterial>);
static_assert(std::is_trivially_copyable_v<SidecarMaterial>);
static_assert(alignof(SidecarMaterial) == 16);
static_assert(sizeof(SidecarMaterial) == 64);
static_assert(offsetof(SidecarMaterial, uvDomain) == 0);
static_assert(offsetof(SidecarMaterial, textureState) == 16);
static_assert(offsetof(SidecarMaterial, textureWindow) == 32);
static_assert(offsetof(SidecarMaterial, presentationDepthBits) == 48);

static_assert(std::is_standard_layout_v<SidecarPreviousVertex>);
static_assert(std::is_trivially_copyable_v<SidecarPreviousVertex>);
static_assert(alignof(SidecarPreviousVertex) == 16);
static_assert(sizeof(SidecarPreviousVertex) == 16);
static_assert(offsetof(SidecarPreviousVertex, positionW) == 0);
static_assert(offsetof(SidecarPreviousVertex, packedOriginAndFlags) == 12);

static_assert(std::is_standard_layout_v<SidecarMidpointPushConstants>);
static_assert(std::is_trivially_copyable_v<SidecarMidpointPushConstants>);
static_assert(sizeof(SidecarMidpointPushConstants) == 32);
static_assert(offsetof(SidecarMidpointPushConstants, sourceVertexWordOffset) ==
              0);
static_assert(offsetof(SidecarMidpointPushConstants, vertexStrideWords) == 16);
static_assert(offsetof(SidecarMidpointPushConstants, reserved) == 28);

static_assert(std::is_standard_layout_v<SidecarModelVertex>);
static_assert(std::is_trivially_copyable_v<SidecarModelVertex>);
static_assert(sizeof(SidecarModelVertex) == 32);
static_assert(offsetof(SidecarModelVertex, positionOrModel) == 0);
static_assert(offsetof(SidecarModelVertex, uv) == 12);
static_assert(offsetof(SidecarModelVertex, color255) == 20);

static_assert(std::is_standard_layout_v<SidecarModelFrameState>);
static_assert(std::is_trivially_copyable_v<SidecarModelFrameState>);
static_assert(alignof(SidecarModelFrameState) == 16);
static_assert(sizeof(SidecarModelFrameState) == 80);
static_assert(offsetof(SidecarModelFrameState, rotation) == 0);
static_assert(offsetof(SidecarModelFrameState, translationProjection) == 48);
static_assert(offsetof(SidecarModelFrameState, presentationState) == 64);

static_assert(std::is_standard_layout_v<SidecarModelPullPushConstants>);
static_assert(std::is_trivially_copyable_v<SidecarModelPullPushConstants>);
static_assert(sizeof(SidecarModelPullPushConstants) == 32);
static_assert(offsetof(SidecarModelPullPushConstants,
                       materialIndexWordOffset) == 0);
static_assert(offsetof(SidecarModelPullPushConstants, modelVertexWordOffset) ==
              8);
static_assert(offsetof(SidecarModelPullPushConstants, frameStateWordOffset) ==
              12);
static_assert(offsetof(SidecarModelPullPushConstants, flags) == 28);

static_assert(std::is_standard_layout_v<SidecarRawVertex>);
static_assert(std::is_trivially_copyable_v<SidecarRawVertex>);
static_assert(alignof(SidecarRawVertex) == 16);
static_assert(sizeof(SidecarRawVertex) == 16);
static_assert(offsetof(SidecarRawVertex, rawXy) == 0);
static_assert(offsetof(SidecarRawVertex, rawZAndFlags) == 4);
static_assert(offsetof(SidecarRawVertex, packedColor) == 8);

static_assert(std::is_standard_layout_v<SidecarRawTopologyFace>);
static_assert(std::is_trivially_copyable_v<SidecarRawTopologyFace>);
static_assert(alignof(SidecarRawTopologyFace) == 16);
static_assert(sizeof(SidecarRawTopologyFace) == 32);
static_assert(offsetof(SidecarRawTopologyFace, vertexIndices) == 0);
static_assert(offsetof(SidecarRawTopologyFace, resourceIndex) == 12);
static_assert(offsetof(SidecarRawTopologyFace, completeVertexIndices) == 16);

static_assert(std::is_standard_layout_v<SidecarRawFace>);
static_assert(std::is_trivially_copyable_v<SidecarRawFace>);
static_assert(alignof(SidecarRawFace) == 16);
static_assert(sizeof(SidecarRawFace) == 32);
static_assert(offsetof(SidecarRawFace, topologyOrFallback) == 0);
static_assert(offsetof(SidecarRawFace, materialIndex) == 4);
static_assert(offsetof(SidecarRawFace, payload) == 8);

static_assert(std::is_standard_layout_v<SidecarRawResourceInstance>);
static_assert(std::is_trivially_copyable_v<SidecarRawResourceInstance>);
static_assert(alignof(SidecarRawResourceInstance) == 16);
static_assert(sizeof(SidecarRawResourceInstance) == 16);
static_assert(offsetof(SidecarRawResourceInstance, translationXy) == 0);
static_assert(offsetof(SidecarRawResourceInstance, translationZ) == 4);
static_assert(offsetof(SidecarRawResourceInstance, flags) == 8);

static_assert(std::is_standard_layout_v<SidecarRawFrameState>);
static_assert(std::is_trivially_copyable_v<SidecarRawFrameState>);
static_assert(alignof(SidecarRawFrameState) == 16);
static_assert(sizeof(SidecarRawFrameState) == 208);
static_assert(offsetof(SidecarRawFrameState, rotation) == 0);
static_assert(offsetof(SidecarRawFrameState, translationProjection) == 48);
static_assert(offsetof(SidecarRawFrameState, presentationState) == 64);
static_assert(offsetof(SidecarRawFrameState, farColor) == 80);
static_assert(offsetof(SidecarRawFrameState, deformationState) == 96);
static_assert(offsetof(SidecarRawFrameState, deformationProgram) == 112);
static_assert(offsetof(SidecarRawFrameState, waveSamples) == 128);

static_assert(std::is_standard_layout_v<SidecarRawPullPushConstants>);
static_assert(std::is_trivially_copyable_v<SidecarRawPullPushConstants>);
static_assert(sizeof(SidecarRawPullPushConstants) == 32);
static_assert(offsetof(SidecarRawPullPushConstants, materialWordOffset) == 0);
static_assert(offsetof(SidecarRawPullPushConstants, faceWordOffset) == 4);
static_assert(offsetof(SidecarRawPullPushConstants,
                       fallbackModelVertexWordOffset) == 16);
static_assert(offsetof(SidecarRawPullPushConstants,
                       previousFrameStateWordOffset) == 28);

static_assert(std::is_standard_layout_v<SidecarPullPushConstants>);
static_assert(std::is_trivially_copyable_v<SidecarPullPushConstants>);
static_assert(sizeof(SidecarPullPushConstants) == 16);
static_assert(offsetof(SidecarPullPushConstants, materialIndexWordOffset) == 0);
static_assert(offsetof(SidecarPullPushConstants, materialWordOffset) == 4);
static_assert(offsetof(SidecarPullPushConstants, firstVertex) == 8);
static_assert(offsetof(SidecarPullPushConstants, previousVertexWordOffset) ==
              12);

static_assert(std::is_standard_layout_v<ScratchCopyPushConstants>);
static_assert(std::is_trivially_copyable_v<ScratchCopyPushConstants>);
static_assert(sizeof(ScratchCopyPushConstants) == 32);
static_assert(offsetof(ScratchCopyPushConstants, sourceDestination) == 0);
static_assert(offsetof(ScratchCopyPushConstants, extentFlags) == 16);

static_assert(std::is_standard_layout_v<VisibilityOwnerPushConstants>);
static_assert(std::is_trivially_copyable_v<VisibilityOwnerPushConstants>);
static_assert(sizeof(VisibilityOwnerPushConstants) == 16);
static_assert(offsetof(VisibilityOwnerPushConstants, state) == 0);

static_assert(std::is_standard_layout_v<PresentationDepthPushConstants>);
static_assert(std::is_trivially_copyable_v<PresentationDepthPushConstants>);
static_assert(sizeof(PresentationDepthPushConstants) == 16);
static_assert(offsetof(PresentationDepthPushConstants,
                       occlusionMultiplierBits) == 0);
static_assert(offsetof(PresentationDepthPushConstants, occlusionBiasBits) ==
              4);
static_assert(offsetof(PresentationDepthPushConstants, surfaceMultiplierBits) ==
              8);
static_assert(offsetof(PresentationDepthPushConstants, surfaceBiasBits) == 12);

static_assert(std::is_standard_layout_v<VisibilityStackHeader>);
static_assert(std::is_trivially_copyable_v<VisibilityStackHeader>);
static_assert(sizeof(VisibilityStackHeader) == 32);
static_assert(offsetof(VisibilityStackHeader, attemptedRecords) == 0);
static_assert(offsetof(VisibilityStackHeader, recordCapacity) == 4);
static_assert(offsetof(VisibilityStackHeader, overflow) == 8);
static_assert(offsetof(VisibilityStackHeader, schema) == 12);
static_assert(offsetof(VisibilityStackHeader, atlasRegion) == 16);

static_assert(std::is_standard_layout_v<VisibilityStackRecord>);
static_assert(std::is_trivially_copyable_v<VisibilityStackRecord>);
static_assert(sizeof(VisibilityStackRecord) == 16);
static_assert(offsetof(VisibilityStackRecord, localSample) == 0);
static_assert(offsetof(VisibilityStackRecord, owner) == 4);
static_assert(offsetof(VisibilityStackRecord, depthBits) == 8);
static_assert(offsetof(VisibilityStackRecord, operation) == 12);

} // namespace ps1::gpu::replay::vulkan_abi
