#include "runtime/games/mgs_scene.h"
#include "runtime/memory.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace ps1::games::mgs {
namespace {
using Matrix = std::array<double, 16>;
bool range(uint32_t address, uint64_t size) {
  const uint32_t physical = address & 0x1fffffffu;
  return physical < Memory::RAM_SIZE && size <= Memory::RAM_SIZE - physical;
}
struct Reader {
  const Memory &memory;
  uint16_t u16(uint32_t address) const {
    const auto *p = memory.ramPtr() + (address & 0x1fffffffu);
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
  }
  uint32_t u32(uint32_t address) const {
    return uint32_t(u16(address)) | uint32_t(u16(address + 2)) << 16;
  }
  int16_t angle(uint32_t address, unsigned axis) const {
    return int16_t(u16(address + axis * 2));
  }
};
Matrix identity() {
  Matrix m{};
  m[0] = m[5] = m[10] = m[15] = 1;
  return m;
}
Matrix multiply(const Matrix &a, const Matrix &b) {
  Matrix out{};
  for (unsigned c = 0; c < 4; ++c)
    for (unsigned r = 0; r < 4; ++r)
      for (unsigned k = 0; k < 4; ++k)
        out[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
  return out;
}
Matrix rotation(unsigned axis, int16_t angle) {
  const double radians = double(angle) * (2 * std::numbers::pi / 4096.0);
  const double c = std::cos(radians), s = std::sin(radians);
  Matrix m = identity();
  const unsigned i = (axis + 1) % 3, j = (axis + 2) % 3;
  m[i * 4 + i] = m[j * 4 + j] = c;
  m[i * 4 + j] = s;
  m[j * 4 + i] = -s;
  return m;
}
Matrix rotationZYX(const Reader &read, uint32_t address) {
  return multiply(multiply(rotation(2, read.angle(address, 2)),
                           rotation(1, read.angle(address, 1))),
                  rotation(0, read.angle(address, 0)));
}
void modelTranslation(Matrix &m, const Reader &read, uint32_t descriptor) {
  for (unsigned axis = 0; axis < 3; ++axis)
    m[12 + axis] = int32_t(read.u32(descriptor + 32 + axis * 4));
}
} // namespace

ScenePoseResult
reconstructScenePose(const Memory &memory, SceneInstance &instance,
                     uint32_t primaryModels,
                     const std::array<float, 16> *presentationRoot) {
  ScenePoseResult result;
  for (auto &model : instance.models)
    model.presentationWorld = model.world;
  const auto invalid = [&]() {
    result.invalidInput = true;
    return result;
  };
  if (!primaryModels || primaryModels > instance.models.size() ||
      !range(instance.guestAddress, 72))
    return invalid();
  const Reader read{memory};
  const uint32_t object = instance.guestAddress;
  const bool single = (instance.flags & 0x40u) != 0;
  const uint32_t rots = read.u32(object + 56);
  const uint32_t adjust = read.u32(object + 60);
  const uint32_t waist = read.u32(object + 64);
  const uint32_t movs = read.u32(object + 68);
  if (!single && !rots && !movs)
    return result; // DG_ScreenModels can consume precomposed scratchpad data.
  if (!single &&
      (primaryModels > 24 ||
       !range(rots ? rots : movs, uint64_t(primaryModels) * 8) ||
       (rots && adjust && !range(adjust, uint64_t(primaryModels) * 8)) ||
       (rots && waist && !range(waist, 8))))
    return invalid();
  Matrix world = identity();
  for (unsigned r = 0; r < 3; ++r) {
    for (unsigned c = 0; c < 3; ++c)
      world[c * 4 + r] = int16_t(read.u16(object + (r * 3 + c) * 2)) / 4096.0;
    world[12 + r] = int32_t(read.u32(object + 20 + r * 4));
  }
  if (presentationRoot)
    std::copy(presentationRoot->begin(), presentationRoot->end(),
              world.begin());
  std::vector<Matrix> pose(primaryModels, world);
  if (!single) {
    // DG_MDL descriptors must belong to this instance. Never follow a stale
    // descriptor or a forward/cyclic parent into another actor's pose.
    if (!range(instance.definition, 32ull + uint64_t(primaryModels) * 88))
      return invalid();
    for (uint32_t j = 0; j < primaryModels; ++j) {
      const uint32_t address = instance.models[j].guestAddress;
      if (!range(address, 68) ||
          read.u32(address + 64) != instance.definition + 32 + j * 88)
        return invalid();
      const int32_t parent =
          int32_t(read.u32(instance.definition + 76 + j * 88));
      if (parent < -1 || parent >= int32_t(j))
        return invalid();
    }
    // Source semantics: FoxdieTeam libdg/screen.c, DG_ApplyRots/Movs.
    // TEMP[-1] aliases WORLD. Joint 1 uses a separate waist/root basis;
    // adjustments rotate already-composed joints in Z, X, Y order, leaving
    // their translations intact. Keep those details, remove Q12 truncation.
    const Matrix base = rots && adjust ? identity() : world;
    Matrix root = identity();
    if (rots) {
      root = rotationZYX(read, waist ? waist : rots);
      modelTranslation(root, read, instance.definition + 32);
      root = multiply(base, root);
    }
    for (uint32_t j = 0; j < primaryModels; ++j) {
      const uint32_t descriptor = instance.definition + 32 + j * 88;
      const int32_t parent = int32_t(read.u32(descriptor + 44));
      const Matrix &parentMatrix = parent == -1 ? base : pose[parent];
      if (rots) {
        Matrix local = rotationZYX(read, rots + j * 8);
        modelTranslation(local, read, descriptor);
        pose[j] = multiply(j == 1 ? root : parentMatrix, local);
        if (adjust) {
          for (unsigned axis : {2u, 0u, 1u}) {
            const auto angle = read.angle(adjust + j * 8, axis);
            if (!angle)
              continue;
            Matrix rotated = multiply(rotation(axis, angle), pose[j]);
            for (unsigned r = 0; r < 3; ++r)
              rotated[12 + r] = pose[j][12 + r];
            pose[j] = rotated;
          }
        }
      } else {
        // Movs inherit the object's rotation, only the parent's translation.
        for (unsigned r = 0; r < 3; ++r) {
          pose[j][12 + r] = parentMatrix[12 + r];
          for (unsigned c = 0; c < 3; ++c)
            pose[j][12 + r] += world[c * 4 + r] * read.angle(movs + j * 8, c);
        }
      }
    }
    if (rots && adjust)
      for (auto &joint : pose)
        joint = multiply(world, joint);
  }
  // Publish atomically after validating the entire hierarchy. Extended pieces
  // retain native matrices here; replay explicitly uses their primary owner.
  for (uint32_t j = 0; j < primaryModels; ++j) {
    auto &model = instance.models[j];
    for (unsigned c = 0; c < 4; ++c)
      for (unsigned r = 0; r < 3; ++r) {
        const double error =
            std::abs(pose[j][c * 4 + r] - model.world[c * 4 + r]);
        auto &maximum =
            c == 3 ? result.maxTranslationError : result.maxRotationError;
        maximum = std::max(maximum, error);
      }
    std::copy(pose[j].begin(), pose[j].end(), model.presentationWorld.begin());
  }
  result.source =
      single ? ScenePoseSource::OnePiece
             : (rots ? ScenePoseSource::Rotations : ScenePoseSource::Movements);
  result.models = primaryModels;
  result.inheritedRoot = presentationRoot != nullptr;
  return result;
}

void reconstructScenePoses(const Memory &memory, SceneSnapshot &scene) {
  struct Owner {
    size_t instance;
    uint32_t model;
  };
  std::unordered_map<uint32_t, Owner> owners;
  const size_t count = scene.instances.size();
  std::vector<bool> done(count, false);
  for (size_t i = 0; i < count; ++i) {
    auto &instance = scene.instances[i];
    instance.pose = {};
    for (auto &model : instance.models)
      model.presentationWorld = model.world;
    for (uint32_t j = 0;
         j < instance.primaryModels && j < instance.models.size(); ++j) {
      auto [it, inserted] = owners.emplace(
          instance.models[j].guestAddress & 0x1fffffffu, Owner{i, j});
      if (!inserted)
        it->second.instance = count; // ambiguous identity cannot supply a root
    }
  }
  for (size_t pass = 0; pass < count; ++pass) {
    bool progress = false;
    for (size_t i = 0; i < count; ++i) {
      if (done[i])
        continue;
      auto &instance = scene.instances[i];
      if (instance.asset == ~0u) {
        done[i] = true;
        progress = true;
        continue;
      }
      const std::array<float, 16> *root = nullptr;
      const auto owner = owners.find(instance.rootAddress & 0x1fffffffu);
      if (instance.rootAddress && owner != owners.end()) {
        if (owner->second.instance == count) {
          instance.pose.invalidInput = true;
          done[i] = true;
          progress = true;
          continue;
        }
        const auto &parent = scene.instances[owner->second.instance];
        if (!done[owner->second.instance])
          continue;
        // ScreenChanl can copy a root before its owner was updated. Equality
        // of the native matrices proves the same sample, not just an address.
        const auto &model = parent.models[owner->second.model];
        bool sameSample =
            range(instance.guestAddress, 32) && range(model.guestAddress, 32);
        const Reader read{memory};
        if (sameSample) {
          for (unsigned k = 0; k < 9; ++k)
            sameSample &= read.u16(instance.guestAddress + k * 2) ==
                          read.u16(model.guestAddress + k * 2);
          for (unsigned k = 0; k < 3; ++k)
            sameSample &= read.u32(instance.guestAddress + 20 + k * 4) ==
                          read.u32(model.guestAddress + 20 + k * 4);
        }
        if (sameSample && (parent.pose.source == ScenePoseSource::Rotations ||
                           parent.pose.source == ScenePoseSource::Movements ||
                           parent.pose.inheritedRoot))
          root = &model.presentationWorld;
      }
      instance.pose =
          reconstructScenePose(memory, instance, instance.primaryModels, root);
      done[i] = true;
      progress = true;
    }
    if (!progress)
      break;
  }
  for (size_t i = 0; i < count; ++i)
    if (!done[i])
      scene.instances[i].pose.invalidInput = true; // cycle, retain native pose
}
} // namespace ps1::games::mgs
