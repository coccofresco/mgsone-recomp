#pragma once

#include "runtime/games/crash3_profile.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ps1::games::crash3 {

// Independent, game-specific reconstruction of the type-1 polygon stream
// which Crash has already admitted to its renderer list. It deliberately
// stops before the original per-face backface/screen/depth gates. The guest
// remains authoritative for simulation, animation, streaming and semantic
// object admission; Vulkan can consume this complete topology for the final
// presentation without exposing a wider scene to game logic.
enum class ObjectMeshDecodeStatus : uint8_t {
  Complete = 0,
  MissingRamSnapshot,
  UnsupportedModelType,
  UnsupportedVertexDecoder,
  MissingTransform,
  InvalidSourceManifest,
  InvalidCommandStream,
  InvalidVertexStream,
  InvalidColorTable,
  InvalidTextureTable,
  ResourceLimit,
};

struct ObjectPresentationMaterial {
  // Raw nine-bit selector encoded in the command. Animated selectors retain
  // bit 8 here; resolvedToken is the one-based 12-byte texture record reached
  // by Crash's node-local remap program for this exact captured frame.
  uint16_t sourceToken = 0;
  uint16_t resolvedToken = 0;
  bool decoded = false;
  bool textured = false;
  bool dynamicTexture = false;
  bool semiTransparent = false;
  uint16_t texturePage = 0;
  uint16_t clut = 0;
  std::array<std::array<float, 2>, 3> uv{};
  std::array<float, 4> uvDomain{};
};

struct ObjectPresentationVertex {
  // Exact signed GTE input reconstructed from Crash's packed two-frame vertex
  // interpolation in the func_8003F460/func_8003E260 renderer family. Its
  // node flag does not decide interpolation: that family always consumes
  // node+0x30 and node+0x34. The func_8003FDB4/func_8003D260 near-plane
  // family instead consumes only node+0x30 and expands its single stream by
  // 32 in the packed GTE lanes; node+0x34 remains executable code.
  std::array<double, 3> model{};
  // Camera-space coordinate after the node's captured GTE transform. This is
  // still in Crash's normalized per-object domain.
  std::array<double, 3> camera{};
  // Native 512x256 presentation coordinate before draw-page translation.
  std::array<double, 2> screen{};
  // False only when the source vertex lies exactly on the projection plane.
  // The camera-space value remains usable by the modern triangle clipper;
  // one such vertex must not invalidate the remainder of a near-plane mesh.
  bool directProjectionValid = false;
  // Camera Z with func_80040E38's normalization reversed. It is the common
  // positive depth domain used by the modern presenter.
  double projectiveW = 0.0;
  // Exact pre-DPCT RGB FIFO word selected by Crash's strip command stream.
  // Keeping it beside the decoded result makes colour certification capable
  // of distinguishing a command-FIFO error from a distance-cue error.
  uint32_t packedSourceColor = 0;
  std::array<float, 3> color255{};
  uint8_t stackIndex = 0;
};

struct ObjectPresentationFace {
  uint32_t objectAddress = 0;
  uint32_t rendererNodeAddress = 0;
  uint32_t ordinal = 0;
  uint32_t commandIndex = 0;
  uint32_t command = 0;
  // Exact commands which populated the three geometric FIFO vertices, plus
  // the strip-head command retained by Crash in scratch+268. Keeping both is
  // required to prove the renderer's per-triangle material selection rather
  // than guessing it from a summarized face token.
  std::array<uint32_t, 3> vertexCommands{};
  uint32_t stripHeadCommand = 0;
  bool originalBackfaceTest = false;
  // Exact sign accepted by func_8003E260's NCLIP branch. The node orientation
  // word is XORed with command bit 29 before comparison with MAC0. This is
  // model topology state, not the original 4:3 screen-frustum decision.
  bool acceptsNegativeWinding = false;
  // Some native constructors draw both sides with different lighting and OT
  // placement. This is their orientation branch, not a visibility rejection.
  bool constructorBackFacing = false;
  // 8003FC00 emits no packet when its three faded colour words are zero.
  // Preserve that explicit effect lifetime, independently of view culling.
  bool constructorEmitsFace = true;
  ObjectPresentationMaterial material{};
  std::array<ObjectPresentationVertex, 3> vertices{};
};

struct ObjectPresentationMesh {
  uint32_t objectAddress = 0;
  uint32_t rendererNodeAddress = 0;
  // Stable logical model selected by the object plus the exact resource
  // installed in this renderer node. They normally agree; retaining both
  // makes a genuine model/LOD switch distinguishable from transient renderer
  // pool reuse without looking at projected size or distance.
  uint32_t logicalModelResource = 0;
  uint32_t rendererModelResource = 0;
  // Exact animation/renderer selection inputs which led to this command
  // stream.  They are diagnostic lineage, not part of temporal face identity:
  // animated models legitimately advance these values while remaining the
  // same logical object and model family.
  uint32_t animationSequence = 0;
  uint32_t animationFrame = 0;
  uint32_t renderSelector = 0;
  uint32_t currentFrameResource = 0;
  uint32_t nextFrameResource = 0;
  uint32_t commandTableResource = 0;
  // FNV-1a of the captured type-1 command stream. Vertex-frame pointers,
  // transforms, animation phase and projected coordinates are deliberately
  // absent, so this names topology/material program identity rather than a
  // pose.
  uint64_t rendererProgramFingerprint = 0;
  // FNV-1a of decoded strip connectivity and vertex-cache identities with
  // material selectors removed. Unlike face count, this distinguishes
  // equal-sized topologies and is therefore safe as a categorical temporal
  // barrier. It does not imply that a larger topology is a higher LOD.
  uint64_t rendererGeometryFingerprint = 0;
  uint32_t vertexRenderer = 0;
  uint32_t primitiveConstructor = 0;
  // The native node reached construction with owned source streams. Streams
  // may be prepared at its bounding preflight, before original 4:3 rejection;
  // this does not claim that the guest emitted a polygon for this node.
  bool sourceConstructionCaptured = false;
  int32_t scaleShift = -1;
  // Exact rotation/translation words used to project this node. Retaining
  // them in the decoded mesh lets diagnostics prove that a source-face miss
  // is geometric rather than a stale renderer-node association.
  std::array<uint32_t, 8> projectionTransformWords{};
  // Projection state captured with this exact renderer node. Keeping the GTE
  // fixed-point origin intact lets the host clip in camera space and then
  // reproduce Crash's projection without consulting a later global camera.
  uint16_t projectionPlane = 0;
  std::array<int32_t, 2> projectionOriginFixed{};
  bool orderingTableBasesCaptured = false;
  uint32_t orderingTableBasePositive = 0;
  uint32_t orderingTableBaseNonPositive = 0;
  int16_t orderingTableZsf3 = 0;
  int16_t orderingTableZsf4 = 0;
  // Exact identity of the 12-byte source-material table. Together with a
  // face's sourceToken this allows a material proved by an emitted packet to
  // authorize an otherwise culled face, including a sibling renderer node
  // sharing the same streamed model resource.
  uint32_t materialTableAddress = 0;
  uint32_t commandCount = 0;
  uint32_t decodedVertexCount = 0;
  uint32_t cachedVertexCount = 0;
  uint32_t colorCommandCount = 0;
  uint32_t unresolvedMaterialFaces = 0;
  uint32_t failureCommand = 0;
  uint32_t failureVertexIndex = 0;
  ObjectMeshDecodeStatus status = ObjectMeshDecodeStatus::InvalidSourceManifest;
  std::vector<ObjectPresentationFace> faces;
  std::vector<ObjectPresentationPacketBinding> packetBindings;
};

// Exact 8003DC00 normal-lighting/material program, fed by the controls
// installed by 8003D510 / 8003E1A4. It consumes a signed packed normal, not
// vertex RGB.
bool applyObjectNormalConstructor(const ObjectPresentationRendererNode &node,
                                  std::span<const uint8_t, 128> highlightTable,
                                  ObjectPresentationFace &face) noexcept;

bool hasIntrinsicObjectConstruction(
    const ObjectPresentationRendererNode &node) noexcept;

enum class ObjectFaceProjectionStatus : uint8_t {
  Direct = 0,
  NearPlaneClipped,
  NearPlaneRejected,
  Invalid,
  ConstructorSuppressed,
};

struct ObjectProjectedVertex {
  std::array<double, 2> screen{};
  double projectiveW = 0.0;
  std::array<float, 2> uv{};
  std::array<float, 3> color255{};
};

struct ObjectProjectedTriangle {
  std::array<ObjectProjectedVertex, 3> vertices{};
};

struct ObjectFaceProjection {
  ObjectFaceProjectionStatus status = ObjectFaceProjectionStatus::Invalid;
  uint8_t triangleCount = 0;
  // Clipping one triangle against one plane can produce at most a quad.
  std::array<ObjectProjectedTriangle, 2> triangles{};
};

enum class ObjectPresentationProjectionMode : uint8_t {
  // Retains the first unsaturated PS1 GTE division depth. This is useful when
  // comparing reconstructed geometry with the packet stream.
  GteSaturation = 0,
  // Clips only geometry which is not in front of the camera. The full-scene
  // Vulkan sidecar uses this plane so PS1 division saturation cannot remove
  // otherwise valid 16:9 geometry.
  PositiveCamera,
  // Preserve the intentional apparent placement of a camera-enclosing
  // panorama by clamping its divide ratio, while retaining unsaturated
  // floating-point X/Y and a coherent positive W for Vulkan.
  GteClampedApparent,
};

struct ObjectPresentationDepthTransform {
  bool valid = false;
  float multiplier = 1.0f;
  float bias = 0.0f;
  uint32_t orderingTableBase = 0;
};

struct ObjectPresentationScene {
  uint32_t captureId = 0;
  uint64_t frameNumber = 0;
  uint32_t attemptedNodes = 0;
  uint32_t completeNodes = 0;
  uint32_t faceCount = 0;
  uint32_t unresolvedMaterialFaces = 0;
  std::vector<ObjectPresentationMesh> meshes;
};

// Pure and deterministic: reads only the immutable RAM image and renderer
// records carried by snapshot. It allocates no persistent state and mutates
// neither guest RAM nor the original GPU packet stream.
ObjectPresentationScene
decodeObjectPresentationScene(const ObjectPresentationSnapshot &snapshot);

// Projects one decoded face for presentation. Guest visibility and packet
// emission are not consulted. UV and vertex lighting are carried through
// geometric clipping so a near-plane crossing no longer discards the complete
// mesh. The default keeps the PS1 comparison plane; a full modern scene uses
// PositiveCamera instead. Camera-enclosing presentation shells use
// GteClampedApparent: this retains their authored apparent placement without
// importing the PS1 integer screen-coordinate limit. cameraScale is applied
// only by PositiveCamera and uniformly expands camera-space XYZ: angular
// projection is unchanged while homogeneous W moves a sky shell behind the
// rest of the modern scene.
ObjectFaceProjection projectObjectPresentationFace(
    const ObjectPresentationMesh &mesh, const ObjectPresentationFace &face,
    ObjectPresentationProjectionMode mode =
        ObjectPresentationProjectionMode::GteSaturation,
    double cameraScale = 1.0) noexcept;

// Reconstruct only Crash's authored one-sided-face decision against the
// precise host projection. Viewport/frustum rejection is deliberately absent:
// Vulkan applies that later against the complete 16:9 page.
bool objectPresentationTrianglePassesBackface(
    const ObjectPresentationFace &face,
    const ObjectProjectedTriangle &triangle) noexcept;

// Reconstruct Crash's intrinsic dynamic-object ordering coordinate. The game
// chooses scratch+0xf4/0xf8 from the signed primitive command, then inserts at
// clamp(base - (AVSZ MAC0 >> 17), 0, 2047). Undoing the renderer's shared
// power-of-two normalization gives one continuous affine depth for the whole
// node, including faces which never reached the guest OT. No screen-space or
// visibility heuristic participates.
ObjectPresentationDepthTransform
objectPresentationDepthTransform(const ObjectPresentationMesh &mesh,
                                 const ObjectPresentationFace &face,
                                 uint8_t sourceVertexCount = 3u) noexcept;

} // namespace ps1::games::crash3
