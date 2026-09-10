// Test-only adapter: compile against a local SwanStation checkout. Nothing
// from the donor is linked into the runtime or distributed with this repo.
// Its actual colour instruction implementations run independently of ours.
#include "core/gte.cpp"

#include <array>
#include <bit>
#include <cstdio>

namespace CPU {
State g_state;
}

int main() {
  static_assert(std::endian::native == std::endian::little);
  // Fixed little-endian stream: instruction + 64 raw architectural register
  // words in, 64 register words out. No ROM, RAM, game identity or PGXP.
  std::array<uint32_t, 65> input{};
  for (;;) {
    const auto bytes = std::fread(input.data(), 1, sizeof(input), stdin);
    if (bytes == 0)
      return std::ferror(stdin) ? 2 : 0;
    if (bytes != sizeof(input))
      return 2;
    std::memcpy(CPU::g_state.gte_regs.r32, input.data() + 1, 256);
    const GTE::Instruction instruction{input[0]};
    switch (input[0] & 63u) {
    case 0x10:
      GTE::Execute_DPCS(instruction);
      break;
    case 0x11:
      GTE::Execute_INTPL(instruction);
      break;
    case 0x13:
      GTE::Execute_NCDS(instruction);
      break;
    case 0x14:
      GTE::Execute_CDP(instruction);
      break;
    case 0x16:
      GTE::Execute_NCDT(instruction);
      break;
    case 0x1b:
      GTE::Execute_NCCS(instruction);
      break;
    case 0x1c:
      GTE::Execute_CC(instruction);
      break;
    case 0x1e:
      GTE::Execute_NCS(instruction);
      break;
    case 0x20:
      GTE::Execute_NCT(instruction);
      break;
    case 0x29:
      GTE::Execute_DCPL(instruction);
      break;
    case 0x2a:
      GTE::Execute_DPCT(instruction);
      break;
    case 0x3f:
      GTE::Execute_NCCT(instruction);
      break;
    default:
      return 3; // Explicit coverage, never silently echo an instruction.
    }
    if (std::fwrite(CPU::g_state.gte_regs.r32, 1, 256, stdout) != 256)
      return 2;
  }
}
