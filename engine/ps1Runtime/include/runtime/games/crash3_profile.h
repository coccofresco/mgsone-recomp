#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

struct recomp_context;

namespace ps1 {
struct CPUContext;
struct PGXPValue;
class Memory;
} // namespace ps1

namespace ps1::games::crash3 {

inline constexpr std::string_view PROFILE_NAME = "crash3_sces_01420";

// Deliberately game-owned policy. Nothing in this structure is inferred or
// enabled for another executable; the generic PS1 runtime remains unchanged
// when profile is false.
struct ProfileOptions {
  bool enabled = false;
  bool widescreen16x9 = false;
  bool presentationDepthCorrections = false;
  uint32_t sceneryCullGuardPixels = 0;
};

void configure(const ProfileOptions &options) noexcept;
const ProfileOptions &options() noexcept;
bool active() noexcept;
bool widescreen16x9() noexcept;
bool presentationDepthCorrections() noexcept;
uint32_t sceneryCullGuardPixels() noexcept;

// Presentation-only horizontal projection scale used by Crash 3. 3/4 converts
// its 4:3 vertical-FOV projection to 16:9 while leaving the architectural GTE
// SXY FIFO, NCLIP results, camera state and gameplay coordinates untouched.
double horizontalProjectionScale() noexcept;

// Convert a packed architectural GTE SXY only for Crash's local bounding-box
// visibility test. The actual GTE FIFO, NCLIP, packet coordinates and gameplay
// state remain untouched. At 16:9 this maps the wider native horizontal range
// through the same exact 3/4 presentation transform before the original
// 512x256 test; otherwise it is an identity operation.
uint32_t viewportCullPresentationSxy(uint32_t packedSxy) noexcept;

// Crash's native-world, scratchpad polygon, procedural four-corner surface and
// point-sprite renderers consume projected SXY values in code-proven viewport
// rejects; some also write them to final packets. Keep the architectural GTE
// FIFO native for NCLIP/gameplay, but expose the 16:9 coordinate at those exact
// renderer sites only for the main 512-wide projection. Non-SXY registers,
// off-screen passes and every other guest PC are returned bit-identically.
uint32_t rendererPresentationSxy(uint32_t pc, uint8_t dataReg,
                                 uint32_t packedSxy,
                                 int32_t projectionOriginXQ16) noexcept;

// SCES-01420's func_80045638 projects one 3D centre with RTPS, then expands
// it into a textured quad in integer CPU code. The exact SXY2 read captures
// that construction provenance without changing the generated translation;
// the submission lookup later derives a sidecar only for those four proven
// packet addresses. This keeps packet bytes, RAM, OT linkage, GTE registers
// and game logic bit-identical while avoiding another monolithic recompile.
void observeProjectedBillboardCentre(const CPUContext &ctx, uint8_t dataReg,
                                     uint32_t packedSxy,
                                     const PGXPValue &precision) noexcept;
bool lookupProjectedBillboardPrecision(uint32_t address, uint32_t packedSxy,
                                       PGXPValue &precision) noexcept;

// True only when rendererPresentationSxy returns a translated or compressed,
// branch-local integer witness instead of the real final coordinate. Its PGXP
// side channel still carries the exact final X/Y and homogeneous projection.
bool rendererPresentationSxyIsCullOnlyGuard(
    uint32_t pc, uint8_t dataReg, int32_t projectionOriginXQ16) noexcept;

// Expand only Crash's packed-SXY scenery visibility witnesses. The hook runs
// at exact AVSZ3/AVSZ4 command PCs after packet-bound coordinates have already
// been preserved. It either translates the temporary comparison X values and
// their bound, or reconstructs a complete procedural-surface quad in the
// compressed guard domain. It never changes the GTE FIFO, packet coordinates,
// camera or Vulkan viewport.
void prepareSceneryCullGuardForCommand(CPUContext &ctx) noexcept;

// Crash 3 builds a compact SLST containing only the scenery polygons selected
// for the original view. These two hooks virtualise the decoder's count and
// entry loads without replacing any of its transformation, backface, viewport
// or packet-emission code. PS1_CRASH3_SLST_MODE accepts:
//   audit  - retain the original list and trace resident-vs-selected coverage;
//   adjacent - retain a bounded window of consecutively decoded SLSTs;
//   symmetric - decode the source path and retain states on both sides of the
//               current camera-path sample;
//   geometric - decode the complete validated source path as a candidate
//               superset, preserve the current list, and retain added faces
//               only when their real world vertices intersect the current
//               16:9 camera frustum;
//   geometric-resident - start from the current checked guest SLST and recover
//               resident quad halves/faces only where their real geometry
//               intersects the widened side band. It needs no host history,
//               so it resumes deterministically from a savestate;
//   bypass - enumerate all resident faces only when the decoded-list capacity
//            can hold them; otherwise fail closed to the original list.
// PS1_CRASH3_SLST_ADJACENT_DELTAS selects 1..64 preceding decoded states for
// adjacent mode, or the radius on either side for symmetric mode (default 1).
// Both windows are presentation-only and never write the guest's
// double-buffered SLSTs. Symmetric mode validates its independently decoded
// final state against the game's raw endpoint and otherwise fails closed.
// Geometric mode additionally validates Crash's resident/scratchpad world
// layout and fails closed rather than trusting stale or malformed geometry.
// PS1_TRACE_CRASH3_SCENERY_ORIENTATION=1 adds determinant-sign counts for
// diagnosis only; no sign is used as a visibility rule.
// The unset/default path is bit-identical to the guest's original loads.
void observeSceneryDelta(recomp_context *ctx) noexcept;
int32_t scenerySelectionCount(recomp_context *ctx,
                              uint32_t listAddress) noexcept;
uint32_t scenerySelectionEntry(recomp_context *ctx,
                               uint32_t cursorAddress) noexcept;

// Host-only resident scenery sidecar. Crash continues to consume its original
// SLST verbatim; this immutable, frame-keyed copy exists solely so Vulkan can
// admit and present geometry outside the guest's original workload window.
// While this mode is active, the older guest-side viewport/guard experiments
// are forced to identity as well: architectural culling sees exactly the
// native 4:3 inputs and only PGXP/Vulkan owns the wider presentation.
// PS1_CRASH3_VULKAN_RESIDENT_MODE accepts:
//   audit - construct and trace the sidecar without drawing it;
//   debug - additionally submit the decoded resident presentation.
// Off/unset performs no resident scan. The debug material is deliberately not
// implicit: source texture/color is the fail-closed default, while the bright
// coverage material requires its separate explicit material-mode switch.
// PS1_TRACE_CRASH3_RESIDENT_PROJECTION=1 exports the immutable projected
// triangle catalog for frames selected by the optional inclusive
// PS1_TRACE_CRASH3_RESIDENT_FRAME_FROM/TO bounds.  It is diagnostic-only and
// lets offline coverage tools distinguish absent topology from a later Vulkan
// rejection without changing either guest or presentation state.
// PS1_CRASH3_RESIDENT_COHORT_PRESENT=0 keeps the transition journal and trace
// active while bypassing transactional presentation, providing a same-binary
// A/B baseline for section-load investigations.
enum class ResidentPresentationMode : uint8_t {
  Off = 0,
  Audit,
  Debug,
};

struct ResidentPresentationVertex {
  double x = 0.0;
  double y = 0.0;
  double projectiveW = 0.0;
};

// Material payload decoded from the same remap program and 12-byte record
// consumed by func_8004103C. sourceToken retains the packed scenery selector;
// resolvedToken is the one-based texture record selected for this exact
// animation state and distance band.
struct ResidentPresentationMaterial {
  uint16_t sourceToken = 0;
  uint16_t resolvedToken = 0;
  bool decoded = false;
  bool textured = false;
  bool dynamicTexture = false;
  // Crash copies its 32-byte distance selector table to renderer scratch and
  // indexes the resulting scratch function without a second 32-byte bound.
  // This distinguishes exact reads from the adjacent scratch state when host
  // completion reaches the same farther bands as the original renderer.
  bool selectorFromScratchTail = false;
  bool semiTransparent = false;
  uint16_t texturePage = 0;
  uint16_t clut = 0;
  // Raw UVs retain the complete source GP0 primitive, including the fourth
  // corner shared by both host triangles of a quad.  The modern high-scale
  // path uses the same donor-proven flipped-sprite predicate as ordinary GP0
  // recording; the resulting one-texel offsets are presentation metadata and
  // are applied only when Vulkan renders above native resolution.
  std::array<std::array<float, 2>, 4> packetUv{};
  uint8_t packetVertexCount = 0;
  uint8_t modernUvOffsetU = 0;
  uint8_t modernUvOffsetV = 0;
  std::array<std::array<float, 2>, 3> uv{};
  std::array<float, 4> uvDomain{};
};

struct ResidentPresentationFace {
  // Stable resource identity from Crash's EID hash pair. Unlike `world`, it
  // survives packed-world reordering at zone boundaries and therefore scopes
  // host caches and face identity across a transactional section handoff.
  uint32_t resourceId = 0;
  uint64_t resourceGeometryFingerprint = 0;
  uint32_t resourceFaceOrdinal = 0;
  // Intrinsic placement class from the resource info record. Crash uses
  // flags=0 for world/camera-relative scenery and flags=1 for geometry whose
  // translation is exactly origin/4 (the camera-anchored background class).
  // Keeping this beside the immutable resource identity lets Vulkan choose a
  // presentation-depth class without consulting an EID, level or frame.
  uint32_t resourceFlags = 0;
  // True only for a resource which has left the guest's current resident
  // table but remains geometrically relevant to the wider host view. It is
  // never treated as evidence that the original renderer emitted a packet.
  bool retainedResource = false;
  uint16_t packed = 0;
  uint8_t world = 0;
  uint8_t half = 0;
  // Exact state found in the current source SLST: 0 means absent, 1/2 are
  // half-quads, 3 is a complete quad and 4 is a triangle.  Host completion
  // uses this intrinsic witness to reproduce AVSZ3 versus AVSZ4 and the
  // resulting distance-selected material without changing guest selection.
  uint8_t sourceSelectionState = 0;
  // Ascending world/triangle/quad construction ordinal. This is the stable
  // order of the complete resident topology, independent of the camera's
  // sparse and potentially reordered SLST.
  uint32_t topologyOrdinal = 0;
  // Exact ordinal in func_8004103C's traversal of the current source SLST.
  // Both halves of one source quad share an ordinal because they are emitted
  // as one primitive. AddPrim prepends that primitive to its local OT bucket,
  // so final equal-bucket command order reverses this value.
  uint32_t sourceTraversalOrdinal = 0;
  bool sourceTraversalOrdinalValid = false;
  // Exact bucket selected by the renderer's AVSZ3/AVSZ4 MAC0 path. This is
  // presentation metadata only; the guest ordering table remains untouched.
  uint16_t orderingTableBucket = 0;
  bool orderingTableBucketValid = false;
  // Membership in Crash's decoded SLST. This is only a guest selection
  // witness: later renderer geometry can still reject the face before GP0.
  bool sourceSelected = false;
  bool nearPlaneClipped = false;
  // The exact projected position is intentionally absent. This face is valid
  // only through the complete raw Vulkan scene named by gpuScene; consumers
  // without that capability must reject the host replacement atomically.
  bool gpuRawOnly = false;
  ResidentPresentationMaterial material{};
  // Crash embeds a 10-bit index in the otherwise-discarded low nibbles of
  // each scenery vertex. It addresses the world's already-lit RGB table, so
  // this is the exact guest-authored Gouraud modulation before an optional
  // distance cue, not a host lighting estimate.
  bool colorDecoded = false;
  std::array<std::array<float, 3>, 3> color255{};
  // Exact signed VXY/VZ values loaded into the GTE for this source triangle.
  // They are retained as a categorical projection-input proof, independently
  // of the later high-precision camera transform.
  std::array<std::array<int16_t, 3>, 3> modelPosition{};
  std::array<uint32_t, 3> sourceVertexIndex{};
  std::array<ResidentPresentationVertex, 3> vertices{};
};

struct ResidentPresentationResource {
  // EID is the immutable key; entryAddress is the current paging result and
  // is deliberately excluded from resource/cohort identity.
  uint32_t resourceId = 0;
  uint32_t entryAddress = 0;
  uint32_t infoAddress = 0;
  uint64_t geometryFingerprint = 0;
  std::array<int32_t, 3> origin{};
  std::array<int32_t, 3> translation{};
  uint32_t flags = 0;
  uint32_t vertexCount = 0;
  uint32_t triangleCount = 0;
  uint32_t quadCount = 0;
  bool identityValid = false;
  bool placementValidated = false;
  // Per-resource admission is deliberately distinct from the verdict for the
  // whole resident table. During a streamed rewrite, already coherent rows
  // can enter the scene cache while unrelated rows are still transient.
  bool individuallyValidated = false;
  bool individualPayloadComplete = false;
  bool individualSourceWitness = false;
  bool individualPlacementWitness = false;
  uint32_t individualObservations = 0;
  bool sceneCacheMember = false;
};

// Immutable source payload for GPU-resident scenery. Geometry identity hashes
// the exact packed vertices and face descriptors, while the separate colour
// fingerprint makes a changed lighting table a new upload without pretending
// that the mesh itself changed. The low vertex nibbles are intentionally kept:
// Crash uses them as the index into its already-lit colour table and as flags
// for the optional wave/distance-cue program.
struct ResidentGpuRawVertex {
  uint32_t rawXy = 0;
  uint16_t rawZ = 0;
  bool colorDecoded = false;
  std::array<uint8_t, 3> color255{};
};

struct ResidentGpuTopologyFace {
  uint32_t resourceFaceOrdinal = 0;
  uint32_t topologyOrdinal = 0;
  uint16_t packed = 0;
  uint8_t half = 0;
  std::array<uint32_t, 3> sourceVertexIndex{};
  std::array<uint32_t, 4> completeSourceVertexIndex{};
  // Original packed polygon words. They carry both immutable indices and the
  // source material token, so a later GPU presentation-plan stage can resolve
  // materials without consulting the expanded CPU-projected face cache.
  uint32_t descriptorA = 0;
  uint32_t descriptorB = 0;
};

// Camera-independent material/topology record for the dominant scenery lane.
// Values are kept in their native PSX byte/halfword domains; the Vulkan raw
// shader derives high-resolution quad UV correction, projection, lighting and
// clipping from current frame state. Dynamic remap tokens and semitransparent
// faces deliberately remain in ResidentGpuPresentationFace.
struct ResidentGpuStaticOpaqueFace {
  uint32_t resourceFaceOrdinal = 0;
  uint16_t packed = 0;
  uint16_t sourceToken = 0;
  uint16_t resolvedToken = 0;
  uint16_t texturePage = 0;
  uint16_t clut = 0;
  uint8_t half = 0;
  bool textured = false;
  std::array<uint16_t, 4> packetUv{};
  std::array<uint8_t, 4> uvDomain{};
};

struct ResidentGpuResourcePayload {
  uint32_t schema = 1;
  uint32_t resourceId = 0;
  uint64_t geometryFingerprint = 0;
  uint64_t colorTableFingerprint = 0;
  uint64_t staticMaterialFingerprint = 0;
  std::vector<ResidentGpuRawVertex> vertices;
  std::vector<ResidentGpuTopologyFace> faces;
  std::vector<ResidentGpuStaticOpaqueFace> staticOpaqueFaces;
};

struct ResidentGpuResourceInstance {
  // `world` is only the slot used by this capture. Stable reuse is keyed by
  // the immutable payload identity above, never by the slot or RAM address.
  uint32_t world = 0;
  uint32_t flags = 0;
  uint32_t translationXy = 0;
  int32_t translationZ = 0;
  std::shared_ptr<const ResidentGpuResourcePayload> payload;
};

struct ResidentGpuDeformationState {
  bool active = false;
  bool distanceCued = false;
  uint32_t trianglePostprocessor = 0;
  uint32_t quadPostprocessor = 0;
  uint32_t time = 0;
  uint32_t phaseShift = 0;
  uint32_t vectorShift = 0;
  uint32_t bias = 0;
  // Every address used by Crash's wave program is base + N*128 for N=0..16.
  // Capturing those exact signed samples avoids uploading an unrelated 6 KiB
  // ROM span every frame and is sufficient to execute the program on Vulkan.
  std::array<int16_t, 17> waveSamples{};
};

// Compact same-frame decision record for one immutable topology face. It
// deliberately excludes projected/model vertices: Vulkan can derive those
// from the raw payload and frame state, while the CPU remains authoritative
// for Crash's material program, SLST witness and local ordering decision.
struct ResidentGpuPresentationFace {
  uint32_t resourceIndex = 0;
  uint32_t resourceFaceOrdinal = 0;
  uint32_t topologyOrdinal = 0;
  uint16_t packed = 0;
  uint8_t half = 0;
  uint8_t sourceSelectionState = 0;
  uint32_t sourceTraversalOrdinal = 0;
  uint16_t orderingTableBucket = 0;
  bool sourceTraversalOrdinalValid = false;
  bool orderingTableBucketValid = false;
  bool sourceSelected = false;
  bool requiresProjectedFallback = false;
  bool colorDecoded = false;
  bool gpuRawOnly = false;
  ResidentPresentationMaterial material{};
};

struct ResidentGpuSceneCapture {
  uint32_t schema = 1;
  uint32_t captureId = 0;
  uint64_t topologyFingerprint = 0;
  uint64_t resourceSetFingerprint = 0;
  std::array<std::array<int16_t, 3>, 3> rotation{};
  std::array<int32_t, 3> translation{};
  int64_t projectionPlane = 0;
  std::array<int32_t, 3> farColor{};
  ResidentGpuDeformationState deformation{};
  std::vector<ResidentGpuResourceInstance> resources;
  std::vector<ResidentGpuPresentationFace> presentationFaces;
  // When active, staticOpaqueFaces across every resource and the residual
  // presentationFaces form one exact, non-overlapping partition of raw faces.
  bool staticOpaquePlanActive = false;
  uint32_t staticOpaqueFaceCount = 0;
  uint32_t rawVertexCount = 0;
  uint32_t rawFaceCount = 0;
  uint32_t gpuRawOnlyFaceCount = 0;
  uint32_t reusedPayloadCount = 0;
  uint32_t rebuiltPayloadCount = 0;
  // The current guest table can temporarily stop carrying a coherent
  // placement for a resource which is still part of the last certified
  // resident cohort. In that case the raw scene is rebuilt from immutable
  // cached payloads and the camera offset witnessed by the remaining valid
  // rows. This flag lets the renderer accept retained CPU oracle faces as
  // exact raw-topology attachments without weakening the ordinary join.
  bool certifiedCohortRecovery = false;
  // True only when these instances describe the complete final presentation
  // cohort. A transactional retained-resource frame keeps the CPU oracle until
  // the same immutable payloads have also been attached to this capture.
  bool completeForPresentation = false;
};

struct ResidentPresentationSnapshot {
  // Host-only construction identity copied into every original scenery
  // projection emitted from this selection. Vulkan joins on this value, not
  // on frame arithmetic or a coincident screen position. It never enters
  // guest RAM/GTE state and is cleared with the ordinary PGXP side channel.
  uint32_t captureId = 0;
  // Fingerprint of the resident geometry/material pointer set. This scopes
  // renderer-side learned material state across level/topology changes.
  uint64_t topologyFingerprint = 0;
  // Order-independent identity of the stable EID + immutable-mesh set. This
  // is the key consumed by the generic candidate -> validated -> active
  // transition journal; no level number, frame or section coordinate enters
  // it.
  uint64_t resourceSetFingerprint = 0;
  uint64_t activeCohortFingerprint = 0;
  uint32_t activeCohortGeneration = 0;
  uint32_t candidateObservations = 0;
  // Exact resource-set intersection with the previously active cohort.  It
  // classifies a continuous streaming handoff independently of the order in
  // which Crash rewrites its resident table.  Zero shared resources is kept
  // distinct because it is a hard scene boundary and must not retain the old
  // scene over the new one.
  uint32_t sharedActiveResourceCount = 0;
  bool resourceSetValidated = false;
  bool placementValidated = false;
  bool resourceIdentityConflict = false;
  bool cohortCandidate = false;
  bool cohortDisjoint = false;
  bool cohortCommitted = false;
  // Transactional presentation state.  Invalid partial tables reproject the
  // last certified active cohort; a valid overlapping candidate is presented
  // immediately and supplemented with only its missing outgoing resources.
  // These flags make the policy auditable at every analogous section load.
  bool activeCohortReprojected = false;
  bool candidateCohortPresented = false;
  bool cohortPresentationValid = false;
  bool cohortPresentationApplied = false;
  // scenerySelectionCount runs while a later GPU mutation batch is being
  // constructed. The frame is diagnostic metadata only; Crash's own tagged
  // packet cohort and double-buffer flip remain the presentation clock.
  uint64_t frameNumber = 0;
  uint32_t listAddress = 0;
  uint32_t sourceEntryCount = 0;
  uint32_t sourceFaceCount = 0;
  uint32_t residentFaceCount = 0;
  uint32_t presentationFrustumFaces = 0;
  uint32_t outsidePresentationFrustumFaces = 0;
  uint32_t nearPlaneClippedFaces = 0;
  uint32_t nearPlaneGeneratedFaces = 0;
  uint32_t nearPlaneRejectedFaces = 0;
  uint32_t decodedMaterialFaces = 0;
  uint32_t unresolvedMaterialFaces = 0;
  uint32_t decodedTexturedFaces = 0;
  uint32_t decodedDynamicMaterialFaces = 0;
  uint32_t decodedScratchTailDynamicMaterialFaces = 0;
  uint32_t decodedSemiTransparentFaces = 0;
  uint32_t decodedOrderingTableFaces = 0;
  uint32_t unresolvedOrderingTableFaces = 0;
  uint32_t decodedColorFaces = 0;
  uint32_t unresolvedColorFaces = 0;
  // Retained for fail-closed accounting. A successfully clipped input is not
  // deferred; this remains non-zero only if a future unsupported case is
  // deliberately omitted.
  uint32_t nearPlaneDeferredFaces = 0;
  uint32_t retainedResourceCount = 0;
  uint32_t retainedResourceFaces = 0;
  // The transactional scene cache contains the active cohort, independently
  // certified incoming rows and the immediately outgoing overlap. It follows
  // resource validity, rather than a zone/frame identifier.
  uint32_t sceneCacheResourceCount = 0;
  uint32_t sceneCacheFaces = 0;
  uint32_t individuallyValidatedResources = 0;
  uint32_t individuallyAdmittedResources = 0;
  uint32_t individuallyRevokedResources = 0;
  uint32_t reprojectedActiveResourceCount = 0;
  uint32_t reprojectedActiveResourceFaces = 0;
  // Same-frame raw source for the Vulkan migration. It is deliberately kept
  // beside the projected oracle during certification; consumers may select it
  // only through exact captureId/resource fingerprints and must fall back if
  // completeForPresentation is false.
  std::optional<ResidentGpuSceneCapture> gpuScene;
  std::vector<ResidentPresentationResource> resources;
  std::vector<ResidentPresentationFace> faces;
};

ResidentPresentationMode residentPresentationMode() noexcept;
// The list/count hook runs before func_8004103C installs the projection
// transform used by its polygon loop.  Finish a pending resident capture at
// the first exact scenery RTPT boundary so source reconstruction and the
// original packet renderer consume the same GTE controls.
void captureResidentPresentationAtProjection(const CPUContext &ctx) noexcept;
void observeResidentPresentationProjectionInputs(
    const CPUContext &ctx) noexcept;
std::optional<ResidentPresentationSnapshot>
residentPresentationSnapshot(uint64_t frameNumber) noexcept;
// Immutable copies of the bounded recent queue, oldest first. The renderer
// uses their tagged source faces to correlate a snapshot with the actual
// replay cohort; frame numbers remain diagnostic metadata, never the sole
// proof of temporal identity.
std::vector<ResidentPresentationSnapshot>
residentPresentationSnapshots() noexcept;
// Zero-copy renderer handoff for the exact capture identity already carried
// by scenery PGXP provenance. Published snapshots are immutable; retaining a
// handle remains safe while the bounded producer journal advances.
std::shared_ptr<const ResidentPresentationSnapshot>
residentPresentationSnapshotByCapture(uint32_t captureId) noexcept;

// Small, mutex-consistent scene signal for unattended checkpoint collection.
// Unlike residentPresentationSnapshots(), this never copies geometry.  The
// host may poll it once per VBlank to detect a stable scenery episode after a
// title/loading gap without naming a level, camera position or resource EID.
struct ResidentPresentationCheckpointSignal {
  uint64_t frameNumber = 0;
  uint64_t resourceSetFingerprint = 0;
  uint64_t activeCohortFingerprint = 0;
  uint32_t captureId = 0;
  uint32_t activeCohortGeneration = 0;
  bool stable = false;
  bool committed = false;
  bool disjoint = false;
};

ResidentPresentationCheckpointSignal
residentPresentationCheckpointSignal() noexcept;

// SCES-01420 prints `%d p l%c` from its resource loader whenever it switches
// level identity (`Y` is the title). The generic libc forwards only the raw
// format address/register arguments here; this profile validates the exact
// executable-owned format address before publishing a host-only marker. It
// changes neither guest RAM nor printf output and is used to checkpoint short
// demo levels which can finish before a resident scenery cohort commits.
struct DemoLevelLoadSignal {
  uint64_t generation = 0;
  uint32_t loadOrdinal = 0;
  uint8_t levelCode = 0;
  bool title = false;
};

void observeDebugPrintf(const CPUContext &ctx, uint32_t formatAddress) noexcept;
DemoLevelLoadSignal demoLevelLoadSignal() noexcept;

// Independent proof that the resident decoder reproduces func_8004103C's
// local OT bucket. The observer receives MAC0>>15 directly from the original
// AVSZ3/AVSZ4 instruction and joins it to the current exact SLST/capture tag;
// it is deliberately distinct from the later outer-DMA OT anchor.
struct ResidentOrderingTableProof {
  uint64_t topologyFingerprint = 0;
  uint64_t matches = 0;
  uint64_t mismatches = 0;
  uint64_t unavailable = 0;
};

void observeSceneryOrderingTableAverage(const CPUContext &ctx,
                                        uint32_t averageDepth,
                                        uint8_t vertexCount) noexcept;
// Passing a capture ID returns the immutable proof state associated with that
// exact scenery packet cohort. Zero retains the live/global diagnostic view.
// This prevents a later resource-table construction from replacing the proof
// while Vulkan is still consuming the preceding double-buffered cohort.
ResidentOrderingTableProof
residentOrderingTableProof(uint32_t captureId = 0u) noexcept;

// Crash 3 dynamic-object presentation sidecar. The hook runs at the exact
// entry of func_8001D72C, before that function applies its display-category
// gates and before func_8001B858 builds the renderer list. Guest memory,
// object status and the original render list remain authoritative for game
// logic. The host copy is the input to the independent Vulkan presentation
// path and therefore includes objects that the original presentation later
// omits.
//
// PS1_CRASH3_VULKAN_OBJECT_MODE accepts:
//   audit - capture objects and their exact renderer-node bindings only;
//   debug - retain the same catalog for incremental host reconstruction.
// Off/unset is allocation-free and bit-identical to the prior runtime.
enum class ObjectPresentationMode : uint8_t {
  Off = 0,
  Audit,
  Debug,
};

struct ObjectPresentationPacketBinding {
  uint32_t packetAddress = 0;
  uint32_t commandIndex = 0;
  uint32_t drawCommand = 0;
};

struct ObjectPresentationRendererNode {
  uint32_t address = 0;
  uint32_t byteCount = 0;
  // listDiscovered proves that the node came from the complete list rooted at
  // renderer state +8336, before the common renderer inspects node+0x1c.
  // transformInstalled is narrower: func_80040E38 reached its TRX/TRY/TRZ
  // writes. Type-2 sprites can legitimately have the former without the
  // latter, so the two states must never be conflated with visibility.
  bool listDiscovered = false;
  // Some renderer passes begin transforming nodes before every logical
  // object has been linked into the global list.  In that case the list scan
  // correctly fails closed, but func_80040E38 still exposes an intrinsic
  // node+0x10 -> object relation at the exact transform boundary.  This flag
  // admits only that directly validated pair; it is neither a screen-space
  // visibility inference nor permission to reuse a node across captures.
  bool directBindingDiscovered = false;
  bool transformInstalled = false;
  // Exact power-of-two normalization selected by func_80040E38 for this
  // node. The guest shifts model coordinates and translation together before
  // GTE projection; the host reverses it only for cross-object presentation
  // depth, never for gameplay or the architectural GTE.
  int32_t scaleShift = -1;
  // Largest SCES-01420 render record is the 0x70-byte type-1 object node.
  // Keeping its exact populated words makes source/material decoding
  // independent from later reuse of Crash's transient renderer pool.
  std::array<uint32_t, 0x70u / sizeof(uint32_t)> words{};
  // func_8003F73C copies the colour table into shared renderer scratch and
  // then consumes the command stream. Several objects can reuse and rewrite
  // those source ranges inside one guest frame, after the frame-wide RAM
  // image was taken. Capture both bounded streams at this node's exact
  // func_80040E38 transform boundary so an asynchronously decoded sidecar can
  // never observe a neighbouring object's table. commandWords excludes the
  // leading colour count and includes the UINT32_MAX terminator.
  bool type1StreamsCaptured = false;
  uint32_t type1ColorWordCount = 0;
  std::vector<uint32_t> type1CommandWords;
  std::vector<uint32_t> type1ColorWords;
  // Exact copied prefix at scratch+320 before any decoded vertex overwrites
  // its aliased VXY/Z words. The source table itself can be rewritten after
  // this copy, so it cannot stand in for the initial scratch contents.
  std::vector<uint32_t> type1ScratchColorWords;
  // Exact triangle constructor's first SXY store: r23 is the DMA node and
  // r18 is the command cursor AFTER the completed face. Observe emission,
  // including the specialized zero-cue path that skips common RGB setup.
  // Captured with this immutable node, never looked up in later live RAM.
  std::vector<ObjectPresentationPacketBinding> packetBindings;
  // func_8003FC1C uses a separate per-vertex SZ fade (3072..5120), not
  // node-wide DPCT. Its exact executed constructor authenticates the mode.
  bool perVertexFadeCaptured = false;
  bool perVertexFadeConsistent = true;
  std::array<int32_t, 3> perVertexFadeFarColor{};
  // Exact object transform/projection controls at renderer installation.
  // TRX/TRY/TRZ are patched with the values about to be written because the
  // observer intentionally runs before architectural GTE mutation.
  std::array<uint32_t, 32> gteControlWords{};
  // Crash selects one of these two intrinsic OT origins from the primitive
  // command before subtracting AVSZ3/AVSZ4 depth. They are scratch+0xf4 and
  // scratch+0xf8 at the exact func_80040E38 transform boundary; retaining
  // them lets the host reproduce the same apparent depth for faces the guest
  // later culls, without consulting their observed screen visibility.
  bool orderingTableBasesCaptured = false;
  uint32_t orderingTableBasePositive = 0;
  uint32_t orderingTableBaseNonPositive = 0;
  // The type-1 primitive renderer can rewrite node+0x1c/node+0x20 after list
  // discovery and after transform installation.  DPCT is the first point at
  // which the effective IR0 and far-colour controls are architecturally
  // observable together.  Capture that exact tuple against this node rather
  // than decoding a stale pre-render copy of the mutable words.
  bool distanceCueCaptured = false;
  bool distanceCueConsistent = true;
  int32_t distanceCueIr0 = 0;
  std::array<int32_t, 3> distanceCueFarColor{};
  uint32_t distanceCueProofs = 0;
};

struct ObjectPresentationObject {
  uint32_t address = 0;
  uint32_t modelResource = 0;
  uint32_t globalResource = 0;
  uint32_t externalResource = 0;
  uint32_t zoneResource = 0;
  // Exact eight-word payload passed as obj+0x20 to Crash's renderer. It is
  // retained raw until each model family authenticates its lighting schema.
  std::array<uint32_t, 8> lightingWords{};
  std::array<int32_t, 3> translation{};
  std::array<int32_t, 3> rotation{};
  std::array<int32_t, 3> scale{};
  uint32_t statusA = 0;
  uint32_t statusB = 0;
  uint32_t statusC = 0;
  uint32_t subtype = 0;
  uint32_t id = 0;
  uint32_t animationSequence = 0;
  uint32_t animationFrame = 0;
  uint32_t routineFlags = 0;
  uint32_t displayMode = 0;
  uint32_t renderSelector = 0;
  uint8_t modelType = 0;
  bool supportedModelType = false;
  // Exact linked-list nodes written by func_8001B858 for this object. Empty
  // means the logical object reached the pre-presentation hook but emitted no
  // node; it does not mean the object was absent from the game simulation.
  std::vector<ObjectPresentationRendererNode> rendererNodes;
};

struct ObjectPresentationSnapshot {
  // Host-only temporal identity. It is also packed beside every matching
  // object's PGXP callsite, so a delayed Vulkan cohort never joins a reused
  // renderer-node address to the wrong frame.
  uint32_t captureId = 0;
  uint64_t frameNumber = 0;
  bool sealed = false;
  bool rendererListScanAttempted = false;
  bool rendererListComplete = false;
  uint32_t rendererListHead = 0;
  uint32_t rendererListNodes = 0;
  uint32_t rejectedObjects = 0;
  uint32_t rejectedRendererNodes = 0;
  // Exact node->object relations in the complete normal render list.
  uint32_t rendererNodeBindings = 0;
  // Subset which reached func_80040E38's common GTE transform installation.
  uint32_t rendererTransformBindings = 0;
  std::vector<ObjectPresentationObject> objects;
  // Debug-mode-only immutable copy taken after the complete renderer list has
  // been validated. Crash streams and reuses model storage; retaining the
  // exact 2 MiB image makes later Vulkan decoding independent of both guest
  // pool reuse and the renderer thread's timing. Audit/off never allocate it.
  // This coarse WIP capture is replaced by sparse certified resource blocks
  // once every object format's pointer graph is known.
  std::shared_ptr<const std::vector<uint8_t>> guestRam;
};

ObjectPresentationMode objectPresentationMode() noexcept;
void observeObjectPrePresentation(recomp_context *ctx) noexcept;
// Stable Crash-owned hook immediately after func_8003F73C has copied the
// type-1 colour prefix to renderer scratch and before command consumption.
void observeObjectRendererStreams(recomp_context *ctx) noexcept;
// Bounded copies, oldest first. The final entry can be the current unsealed
// catalog so Vulkan may consume it in the same presentation interval.
std::vector<ObjectPresentationSnapshot> objectPresentationSnapshots() noexcept;

// Preserve the aspect of direct 2D packets when the 512-pixel page is finally
// presented as 16:9. The adjustment affects only CrashReplay presentation;
// the guest's original GPU coordinates and gameplay logic are untouched.
int32_t screen2DPresentationX(int32_t x, int32_t drawAreaLeft,
                              int32_t drawAreaRight,
                              int32_t packetAnchorX) noexcept;

// The native renderer no longer executes the generic per-instruction model
// lineage tracker. Preserve one explicitly Crash-owned, value-independent
// construction tag in the otherwise-unused model-source carrier. The marker
// keeps it disjoint from real PS1 address tokens and the validity flag remains
// clear, so universal provenance cannot mistake it for a direct model load.
void attachProjectionTag(const CPUContext &ctx, uint8_t vectorIndex,
                         PGXPValue &projection) noexcept;
std::optional<uint64_t> decodeProjectionTag(uint64_t carrier) noexcept;
// Decode the stable pre-cull vertex identity carried by Crash's procedural
// projectors. The value is categorical construction provenance, not a
// screen-position or motion heuristic. The narrower ordinal decoder is kept
// only for reading historical captures; ordinal tags cannot admit interpolation.
std::optional<uint64_t>
proceduralProjectionIdentity(uint64_t gameProfileTag) noexcept;
// Strong presentation admission additionally binds the structured payload to
// the exact Crash renderer which owns that schema. Generic construction tags
// are hashes and can otherwise share the reserved payload prefix by chance.
std::optional<uint64_t>
proceduralProjectionIdentity(uint64_t gameProfileCallsite,
                             uint64_t gameProfileTag) noexcept;
std::optional<uint64_t>
proceduralProjectionOrdinal(uint64_t gameProfileTag) noexcept;

// Direct hooks for Crash's coordinate-normalizing renderer. The game computes
// its shift at 0x80040E38, writes the shifted GTE translation, then installs
// the correspondingly normalized rotation at fixed PCs consumed here.
// Timeline restore reloads the same value from scratchpad. The three-argument
// form receives the exact value before the architectural control write; the
// two-argument form remains a deterministic test/legacy observer.
void observeGteControlWrite(const CPUContext &ctx, uint8_t reg) noexcept;
void observeGteControlWrite(const CPUContext &ctx, uint8_t reg,
                            uint32_t writtenValue) noexcept;
// Called immediately before architectural DPCT execution.  It is a no-op
// outside Crash 3's proven type-1 primitive callsite.
void observeObjectDistanceCue(const CPUContext &ctx) noexcept;
void observeObjectPacketBinding(const CPUContext &ctx) noexcept;
void restoreTransientState(const Memory &memory) noexcept;

// Decode the stable Crash-owned callsite carried beside PGXP projection data.
// A zero callsite means the profile did not attach one; scale shift -1 means
// the projection was outside the game's scratchpad-backed polygon renderer.
uint32_t projectionCallsiteGtePc(uint64_t callsite) noexcept;
int32_t projectionCallsiteScaleShift(uint64_t callsite) noexcept;
uint8_t projectionCallsiteVectorIndex(uint64_t callsite) noexcept;
uint32_t projectionCallsiteObjectSource(uint64_t callsite) noexcept;
uint32_t projectionCallsiteObjectCapture(uint64_t callsite) noexcept;
// Exact sidecar-only join from a process-local GTE projection event to Crash's
// type-1 vertex command. The bounded journal is lock-free and populated only
// while an authenticated object capture is active; it changes neither PGXP's
// shared layout nor the guest/replay projection context.
std::optional<uint32_t> objectProjectionCommand(uint64_t projectionId) noexcept;
// Exact logical object joined at func_80040E38's validated node->object
// boundary. Renderer-node addresses are transient in several levels; this
// process-local journal lets replay use the persistent game object without
// putting a frame/capture ordinal into temporal identity.
std::optional<uint32_t>
objectProjectionLogicalObject(uint64_t projectionId) noexcept;

} // namespace ps1::games::crash3
