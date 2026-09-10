// Tests for function-end refinement.
//
// The rule this replaces was "a function ends where the next detected one
// begins". That is wrong whenever detection produces a false entry point
// inside a real function: the body gets cut, and the branch spanning the cut
// is emitted as a call to an address the recompiler never emits. Measured on
// Crash Bandicoot (SCUS-94900): 0x8003A264 is a label inside func_8003A144,
// reached by two forward branches; we cut there and dispatched to nothing.

#include "ps1recomp/function_finder.h"
#include <gtest/gtest.h>
#include <vector>

using namespace ps1recomp;

namespace {

// Instruction builders -- just enough to express the shapes under test.
constexpr uint32_t kNop = 0x00000000u;
constexpr uint32_t kJrRa = 0x03E00008u; // jr $ra

// jr $reg
uint32_t jr(uint32_t reg) { return (reg << 21) | 0x08u; }

// lw $rt, imm($rs) -- opcode 0x23
uint32_t lw(uint32_t rt, uint32_t rs, int16_t imm) {
  return (0x23u << 26) | (rs << 21) | (rt << 16) |
         static_cast<uint16_t>(imm);
}

// addu $rd, $rs, $rt -- SPECIAL fn 0x21
uint32_t addu(uint32_t rd, uint32_t rs, uint32_t rt) {
  return (rs << 21) | (rt << 16) | (rd << 11) | 0x21u;
}

// move $rd, $ra  ==  addu $rd, $ra, $zero
uint32_t moveFromRa(uint32_t rd) { return addu(rd, 31, 0); }

// beq $rs, $rt, offset(instructions forward from the delay slot)
uint32_t beq(uint32_t rs, uint32_t rt, int16_t words) {
  return (0x04u << 26) | (rs << 21) | (rt << 16) |
         static_cast<uint16_t>(words);
}

constexpr uint32_t kBase = 0x80010000u;

TEST(RefineFunctionEnd, EndsAtPlainReturn) {
  // index 0: nop, 1: jr $ra, 2: nop(delay) -> body is [base, base+12)
  const std::vector<uint32_t> w{kNop, kJrRa, kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 20), kBase + 12);
}

TEST(RefineFunctionEnd, ForwardBranchOverTheReturnKeepsTheBodyTogether) {
  // This is the shape that broke Crash. A branch jumps past an early `jr $ra`;
  // the function continues to the *second* return, and everything in between
  // belongs to one body.
  //
  //  0: beq $0,$0,+4   -> targets index 6
  //  1: nop (delay)
  //  2: jr $ra          <- must NOT end the function: index 6 is still ahead
  //  3: nop
  //  4: nop
  //  5: nop
  //  6: jr $ra          <- the real end
  //  7: nop (delay)
  const std::vector<uint32_t> w{beq(0, 0, 4), kNop, kJrRa, kNop,
                                kNop,         kNop, kJrRa, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 32), kBase + 32)
      << "an early return before a live forward branch must not cut the body";
}

TEST(RefineFunctionEnd, ReturnAtOrPastReachDoesEndTheFunction) {
  // Same shape, but the branch target lands exactly on the terminator, so the
  // terminator is legitimately the end.
  //
  //  0: beq $0,$0,+2 -> index 4
  //  1: nop (delay)
  //  2: nop
  //  3: nop
  //  4: jr $ra
  //  5: nop (delay)
  //  6..7: another function's padding
  const std::vector<uint32_t> w{beq(0, 0, 2), kNop, kNop, kNop,
                                kJrRa,        kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 32), kBase + 24);
}

TEST(RefineFunctionEnd, JumpTableJrDoesNotEndTheFunction) {
  // `jr $t0` where $t0 came out of memory is a jump table or a call through a
  // pointer -- the targets are inside this function, so it continues.
  //
  //  0: lw $t0, 0($t1)
  //  1: jr $t0        <- not a return
  //  2: nop (delay)
  //  3: nop
  //  4: jr $ra        <- the real end
  //  5: nop (delay)
  const std::vector<uint32_t> w{lw(8, 9, 0), jr(8), kNop, kNop, kJrRa, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 24), kBase + 24);
}

TEST(RefineFunctionEnd, JrThroughAnRaAliasIsAReturn) {
  // `move $t0, $ra; ...; jr $t0` is a return with the address parked in another
  // register -- it must end the function, unlike the jump-table case above.
  const std::vector<uint32_t> w{moveFromRa(8), kNop, jr(8), kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 24), kBase + 16);
}

TEST(RefineFunctionEnd, NeverRunsPastTheHardBound) {
  // No terminator at all: the caller's bound wins, and nothing beyond it is
  // ever claimed.
  const std::vector<uint32_t> w{kNop, kNop, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 8), kBase + 8);
}

TEST(RefineFunctionEnd, TerminatorNearTheBoundIsClampedNotOverrun) {
  // `jr $ra` in the last slot would put end at +8 past it; the bound clamps it.
  const std::vector<uint32_t> w{kNop, kJrRa};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 8), kBase + 8);
}

TEST(RefineFunctionEnd, BackwardBranchDoesNotExtendTheBody) {
  // A loop branching backwards must not push `reach` forward and swallow the
  // following function.
  //
  //  0: nop
  //  1: beq $0,$0,-2  -> back to index 0
  //  2: nop (delay)
  //  3: jr $ra
  //  4: nop (delay)
  const std::vector<uint32_t> w{kNop, beq(0, 0, -2), kNop, kJrRa, kNop, kNop};
  EXPECT_EQ(refineFunctionEnd(w, kBase, kBase + 24), kBase + 20);
}

TEST(WritesRegister, ClassifiesTheFormsBoundaryDetectionRelieson) {
  EXPECT_TRUE(mips::writesRegister(lw(8, 9, 0), 8));
  EXPECT_FALSE(mips::writesRegister(lw(8, 9, 0), 9)) << "rs is the base, not a destination";
  EXPECT_TRUE(mips::writesRegister(addu(8, 9, 10), 8));
  EXPECT_FALSE(mips::writesRegister(jr(8), 8)) << "jr writes nothing";
  EXPECT_FALSE(mips::writesRegister(addu(0, 9, 10), 0)) << "$zero is never written";
}

} // namespace

// Sobreposicao de funcoes.
//
// Tamanhos vem de fontes que nao se revisitam: simbolo do ELF, distancia ate a
// proxima entrada, e `--add-func` depois de tudo. Adicionar uma entrada dentro
// de uma funcao ja dimensionada deixava a que engloba com o tamanho antigo, e
// os mesmos bytes saiam duas vezes no arquivo gerado, sob dois nomes.
//
// Os sete casos abaixo sao os medidos em configs/crash_recomp.toml -- um para
// cada `--add-func` que tools/regen_crash.sh acumulou para contornar a
// deteccao fraca. Cada workaround criou uma sobreposicao.

namespace {

FunctionInfo fn(uint32_t addr, uint32_t size, const char *name) {
  FunctionInfo f;
  f.address = addr;
  f.size = size;
  f.name = name;
  f.source = FunctionSource::JALTarget;
  f.isLeaf = false;
  return f;
}

} // namespace

TEST(ClampOverlappingSizes, ShrinksTheContainerToTheNextEntry) {
  // O par medido: func_8004636C (1716) engloba func_added_800466A0 (896);
  // as duas terminavam em 0x80046A20.
  std::vector<FunctionInfo> funcs{
      fn(0x8004636Cu, 1716, "func_8004636C"),
      fn(0x800466A0u, 896, "func_added_800466A0"),
  };
  clampOverlappingSizes(funcs);
  EXPECT_EQ(funcs[0].size, 0x800466A0u - 0x8004636Cu)
      << "a funcao que engloba tem de parar onde a proxima comeca";
  EXPECT_EQ(funcs[1].size, 896u) << "a ultima nao tem o que sobrepor";
}

TEST(ClampOverlappingSizes, FixesEverySevenMeasuredPairs) {
  // Os sete pares reais de configs/crash_recomp.toml.
  std::vector<FunctionInfo> funcs{
      fn(0x80016A6Cu, 680, "func_80016A6C"),  fn(0x80016C18u, 4, "add1"),
      fn(0x8001AAD8u, 452, "func_8001AAD8"),  fn(0x8001AC60u, 4, "add2"),
      fn(0x800253A0u, 912, "func_800253A0"),  fn(0x80025628u, 4, "add3"),
      fn(0x8002D384u, 792, "func_8002D384"),  fn(0x8002D638u, 4, "add4"),
      fn(0x8002E3F8u, 1236, "func_8002E3F8"), fn(0x8002E8A4u, 4, "add5"),
      fn(0x800342D8u, 588, "func_800342D8"),  fn(0x80034504u, 4, "add6"),
      fn(0x8004636Cu, 1716, "func_8004636C"), fn(0x800466A0u, 4, "add7"),
  };
  clampOverlappingSizes(funcs);
  for (size_t i = 0; i + 1 < funcs.size(); ++i) {
    EXPECT_LE(funcs[i].address + funcs[i].size, funcs[i + 1].address)
        << funcs[i].name << " ainda alcanca " << funcs[i + 1].name;
  }
}

TEST(ClampOverlappingSizes, LeavesNonOverlappingSizesAlone) {
  std::vector<FunctionInfo> funcs{
      fn(0x80010000u, 16, "a"), // termina exatamente onde b comeca
      fn(0x80010010u, 8, "b"),  // deixa uma folga antes de c
      fn(0x80010100u, 32, "c"),
  };
  const auto before = funcs;
  clampOverlappingSizes(funcs);
  for (size_t i = 0; i < funcs.size(); ++i)
    EXPECT_EQ(funcs[i].size, before[i].size) << funcs[i].name;
}

TEST(ClampOverlappingSizes, SortsWhenTheInputIsOutOfOrder) {
  std::vector<FunctionInfo> funcs{
      fn(0x800466A0u, 896, "depois"),
      fn(0x8004636Cu, 1716, "antes"),
  };
  clampOverlappingSizes(funcs);
  ASSERT_EQ(funcs[0].name, "antes");
  EXPECT_EQ(funcs[0].size, 0x800466A0u - 0x8004636Cu);
}

TEST(ClampOverlappingSizes, ToleratesDuplicateAddresses) {
  std::vector<FunctionInfo> funcs{
      fn(0x80010000u, 64, "a"),
      fn(0x80010000u, 64, "duplicata"),
      fn(0x80010080u, 16, "b"),
  };
  clampOverlappingSizes(funcs); // nao pode dividir por zero nem zerar tudo
  EXPECT_EQ(funcs[2].size, 16u);
}
