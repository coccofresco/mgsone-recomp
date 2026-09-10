// Tests for ps1Analyzer -- Function Finder
// Validates function detection using crafted MIPS binaries

#include <gtest/gtest.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/elf_parser.h>
#include <elfio/elfio.hpp>
#include <cstdio>
#include <fstream>

using namespace ps1recomp;

// Helper: Encode MIPS instructions

static uint32_t makeNOP() { return 0x00000000; }

static uint32_t makeJAL(uint32_t pc, uint32_t target) {
    // JAL: opcode=3, target26 = (target >> 2) & 0x03FFFFFF
    (void)pc; // pc is needed for full calculation but target is absolute
    return (mips::OP_JAL << 26) | ((target >> 2) & 0x03FFFFFF);
}

static uint32_t makeJR_RA() {
    // JR $ra: SPECIAL | rs=31 | 0 | 0 | 0 | FUNC_JR
    return (mips::OP_SPECIAL << 26) | (mips::REG_RA << 21) | mips::FUNC_JR;
}

static uint32_t makeJR(uint32_t reg) {
    return (mips::OP_SPECIAL << 26) | (reg << 21) | mips::FUNC_JR;
}

static uint32_t makeADDIU_SP(int16_t imm) {
    // ADDIU $sp, $sp, imm
    return (mips::OP_ADDIU << 26) | (mips::REG_SP << 21) | (mips::REG_SP << 16)
         | (static_cast<uint16_t>(imm) & 0xFFFF);
}

static uint32_t makeADDIU(uint32_t rs, uint32_t rt, int16_t imm) {
    return (mips::OP_ADDIU << 26) | (rs << 21) | (rt << 16)
         | (static_cast<uint16_t>(imm) & 0xFFFF);
}

static uint32_t makeLUI(uint32_t rt, uint16_t imm) {
    return (0x0Fu << 26) | (rt << 16) | imm;
}

static uint32_t makeORI(uint32_t rs, uint32_t rt, uint16_t imm) {
    return (0x0Du << 26) | (rs << 21) | (rt << 16) | imm;
}

// Helper: write LE 32-bit word to buffer
static void writeLE32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(val & 0xFF);
    buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 24) & 0xFF);
}

// Helper: Create ELF with specific code

static std::string createElfWithCode(const std::string& path,
                                     const std::vector<uint32_t>& instructions,
                                     uint32_t baseAddr = 0x80010000,
                                     bool addSymbols = false,
                                     const std::vector<std::pair<std::string, uint32_t>>& funcs = {}) {
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(baseAddr);

    // .text section with given instructions
    auto* text_sec = writer.sections.add(".text");
    text_sec->set_type(ELFIO::SHT_PROGBITS);
    text_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text_sec->set_addr_align(4);
    text_sec->set_address(baseAddr);

    std::vector<uint8_t> text_data;
    for (auto instr : instructions) {
        writeLE32(text_data, instr);
    }
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()), text_data.size());

    // Add symbols if requested
    if (addSymbols && !funcs.empty()) {
        auto* strtab = writer.sections.add(".strtab");
        strtab->set_type(ELFIO::SHT_STRTAB);
        ELFIO::string_section_accessor stra(strtab);

        auto* symtab = writer.sections.add(".symtab");
        symtab->set_type(ELFIO::SHT_SYMTAB);
        symtab->set_info(2);
        symtab->set_addr_align(4);
        symtab->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));
        symtab->set_link(strtab->get_index());

        ELFIO::symbol_section_accessor syma(writer, symtab);
        for (const auto& [name, addr] : funcs) {
            syma.add_symbol(stra, name.c_str(), addr, 0,
                            ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                            text_sec->get_index());
        }
    }

    auto* segment = writer.segments.add();
    segment->set_type(ELFIO::PT_LOAD);
    segment->set_virtual_address(baseAddr);
    segment->set_physical_address(baseAddr);
    segment->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    segment->set_align(0x1000);
    segment->add_section_index(text_sec->get_index(), text_sec->get_addr_align());

    writer.save(path);
    return path;
}

static void cleanupFile(const std::string& path) {
    std::remove(path.c_str());
}

// MIPS Instruction Helper Tests

TEST(MipsHelpers, OpcodeExtraction) {
    uint32_t jal = makeJAL(0x80010000, 0x80010100);
    EXPECT_EQ(mips::getOpcode(jal), mips::OP_JAL);
    EXPECT_TRUE(mips::isJAL(jal));
}

TEST(MipsHelpers, JR_RA_Detection) {
    uint32_t jr_ra = makeJR_RA();
    EXPECT_TRUE(mips::isJR_RA(jr_ra));
    EXPECT_FALSE(mips::isJR_RA(makeNOP()));
}

TEST(MipsHelpers, StackPrologue) {
    uint32_t prologue = makeADDIU_SP(-32);
    EXPECT_TRUE(mips::isStackPrologue(prologue));
    EXPECT_FALSE(mips::isStackEpilogue(prologue));

    // Regular ADDIU (not $sp) should not match
    uint32_t other = makeADDIU(4, 4, -16);
    EXPECT_FALSE(mips::isStackPrologue(other));
}

TEST(MipsHelpers, StackEpilogue) {
    uint32_t epilogue = makeADDIU_SP(32);
    EXPECT_TRUE(mips::isStackEpilogue(epilogue));
    EXPECT_FALSE(mips::isStackPrologue(epilogue));
}

TEST(MipsHelpers, JALTarget) {
    uint32_t pc = 0x80010000;
    uint32_t target = 0x80010100;
    uint32_t jal = makeJAL(pc, target);
    EXPECT_EQ(mips::jalTarget(pc, jal), target);
}

TEST(MipsHelpers, NOPDetection) {
    EXPECT_TRUE(mips::isNOP(0));
    EXPECT_FALSE(mips::isNOP(makeJR_RA()));
}

// Function Finder -- Entry Point Detection

TEST(FunctionFinder, DetectsEntryPoint) {
    const std::string path = "/tmp/ps1recomp_test_ff_entry.elf";
    std::vector<uint32_t> code = {makeNOP(), makeNOP(), makeJR_RA(), makeNOP()};
    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    EXPECT_GE(finder.getFunctionCount(), 1u);
    auto* func = finder.findByAddress(0x80010000);
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->name, "__start");
    EXPECT_EQ(func->source, FunctionSource::EntryPoint);

    cleanupFile(path);
}

// Function Finder -- JAL Target Detection

TEST(FunctionFinder, DetectsJALTargets) {
    const std::string path = "/tmp/ps1recomp_test_ff_jal.elf";

    // Code at 0x80010000:
    // func_A: JAL func_B (at +0x20), NOP, JR $ra, NOP
    // ...padding...
    // func_B (at +0x20): NOP, JR $ra, NOP
    std::vector<uint32_t> code(16, makeNOP());
    code[0] = makeJAL(0x80010000, 0x80010020); // JAL to offset 0x20
    code[2] = makeJR_RA();
    // func_B at offset 0x20 (index 8)
    code[9] = makeJR_RA(); // func_B: JR $ra

    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    // Should find entry point + JAL target
    EXPECT_GE(finder.getFunctionCount(), 2u);

    auto* funcB = finder.findByAddress(0x80010020);
    ASSERT_NE(funcB, nullptr);
    EXPECT_EQ(funcB->source, FunctionSource::JALTarget);

    // JAL targets should be tracked
    EXPECT_TRUE(finder.getJALTargets().count(0x80010020));

    cleanupFile(path);
}

// Function Finder -- Symbol Detection

TEST(FunctionFinder, DetectsSymbolFunctions) {
    const std::string path = "/tmp/ps1recomp_test_ff_syms.elf";

    std::vector<uint32_t> code(32, makeNOP());
    code[7]  = makeJR_RA(); // end of func at +0x00
    code[15] = makeJR_RA(); // end of func at +0x20

    std::vector<std::pair<std::string, uint32_t>> syms = {
        {"main", 0x80010000},
        {"update", 0x80010020}
    };

    createElfWithCode(path, code, 0x80010000, true, syms);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    auto* main_func = finder.findByAddress(0x80010000);
    ASSERT_NE(main_func, nullptr);
    // Entry point takes priority over symbol for the same address
    EXPECT_TRUE(main_func->source == FunctionSource::EntryPoint ||
                main_func->source == FunctionSource::Symbol);

    auto* update_func = finder.findByAddress(0x80010020);
    ASSERT_NE(update_func, nullptr);
    EXPECT_EQ(update_func->name, "update");
    EXPECT_EQ(update_func->source, FunctionSource::Symbol);

    cleanupFile(path);
}

// Function Finder -- Prologue Detection

TEST(FunctionFinder, DetectsProloguePatterns) {
    const std::string path = "/tmp/ps1recomp_test_ff_prologue.elf";

    // Two functions:
    // func_A (at +0): ADDIU $sp, $sp, -32 ... JR $ra, NOP
    // func_B (at +0x20): ADDIU $sp, $sp, -16 ... JR $ra, NOP
    std::vector<uint32_t> code(16, makeNOP());
    code[0]  = makeADDIU_SP(-32);   // func_A prologue
    code[6]  = makeJR_RA();          // func_A return
    // code[7] = NOP (delay slot)
    code[8]  = makeADDIU_SP(-16);   // func_B prologue (after JR+NOP)
    code[14] = makeJR_RA();          // func_B return

    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    // Should detect at least 2 functions (entry point + prologue at +0x20)
    EXPECT_GE(finder.getFunctionCount(), 2u);

    cleanupFile(path);
}

TEST(FunctionFinder, DetectsEntryBeforeDelayedStackPrologue) {
    const std::string path = "/tmp/ps1recomp_test_ff_delayed_prologue.elf";

    // The second function begins immediately after the first JR+delay pair,
    // but reads two globals before allocating its stack frame.  Its callable
    // entry is +0x18, not the ADDIU $sp at +0x20.
    std::vector<uint32_t> code(16, makeNOP());
    code[4] = makeJR_RA();
    code[6] = 0x3C028006;       // LUI v0, 0x8006
    code[7] = 0x8C420BF0;       // LW v0, 0x0BF0(v0)
    code[8] = makeADDIU_SP(-64);
    code[14] = makeJR_RA();

    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    const auto *callback = finder.findByAddress(0x80010018);
    ASSERT_NE(callback, nullptr);
    EXPECT_EQ(callback->source, FunctionSource::Prologue);
    EXPECT_EQ(finder.findByAddress(0x80010020), nullptr);

    cleanupFile(path);
}

TEST(FunctionFinder, DetectsLeafEntryStoredInData) {
    const std::string path = "/tmp/ps1recomp_test_ff_data_pointer.elf";

    // The callback at +0x18 has no prologue and is never a JAL target.  Its
    // address is stored in an embedded callback table at +0x30.  The prior
    // function's JR+delay pair makes +0x18 a conservative callable boundary.
    std::vector<uint32_t> code(16, makeNOP());
    code[4] = makeJR_RA();
    code[8] = makeJR_RA();
    code[12] = 0x80010018;

    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    const auto *callback = finder.findByAddress(0x80010018);
    ASSERT_NE(callback, nullptr);
    EXPECT_EQ(callback->source, FunctionSource::DataPointer);
    EXPECT_TRUE(callback->isLeaf);

    cleanupFile(path);
}

TEST(FunctionFinder, DetectsStoredCallbackAfterEarlierComputedJump) {
    const std::string path = "/tmp/ps1recomp_test_ff_data_pointer_after_jr.elf";

    // A computed JR earlier in a large hand-written renderer does not make
    // every later data pointer a local jump-table label.  The explicit return
    // immediately before +0x18 is the stronger boundary signal.  Crash 3 uses
    // this shape for the callable renderer entry at 0x8003FDB4.
    std::vector<uint32_t> code(16, makeNOP());
    code[0] = 0x8D280000; // lw t0, 0(t1)
    code[1] = makeJR(8);
    code[4] = makeJR_RA();
    code[8] = makeJR_RA();
    code[12] = 0x80010018;

    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    const auto *callback = finder.findByAddress(0x80010018);
    ASSERT_NE(callback, nullptr);
    EXPECT_EQ(callback->source, FunctionSource::DataPointer);

    cleanupFile(path);
}

TEST(FunctionFinder, DetectsCallbackAddressesComputedInCode) {
    const std::string path = "/tmp/ps1recomp_test_ff_computed_pointer.elf";

    // The entry routine passes two callback addresses indirectly. Neither
    // callback is a JAL target, has a stack prologue, or appears as a literal
    // 32-bit data word. Both begin after a prior JR+delay pair.
    std::vector<uint32_t> code(20, makeNOP());
    code[0] = makeLUI(5, 0x8001);
    code[1] = makeADDIU(5, 5, 0x20);
    code[2] = makeLUI(6, 0x8001);
    code[3] = makeORI(6, 6, 0x0030);
    code[6] = makeJR_RA();  // entry ends at +0x20
    code[10] = makeJR_RA(); // first callback ends at +0x30
    code[14] = makeJR_RA(); // second callback

    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    const auto *first = finder.findByAddress(0x80010020);
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->source, FunctionSource::ComputedPointer);
    EXPECT_EQ(first->size, 0x10u);

    const auto *second = finder.findByAddress(0x80010030);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->source, FunctionSource::ComputedPointer);

    cleanupFile(path);
}

// Function Finder -- Boundary Computation

TEST(FunctionFinder, ComputesSizes) {
    const std::string path = "/tmp/ps1recomp_test_ff_sizes.elf";

    // Three known functions via symbols
    std::vector<uint32_t> code(24, makeNOP());
    std::vector<std::pair<std::string, uint32_t>> syms = {
        {"func_a", 0x80010000},  // 32 bytes (8 instructions)
        {"func_b", 0x80010020},  // 16 bytes (4 instructions)
        {"func_c", 0x80010030}   // rest (32 bytes to end)
    };

    createElfWithCode(path, code, 0x80010000, true, syms);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    // func_a: 0x80010000 to 0x80010020 = 0x20 = 32 bytes
    auto* fa = finder.findByAddress(0x80010000);
    ASSERT_NE(fa, nullptr);
    EXPECT_EQ(fa->size, 0x20u);

    // func_b: 0x80010020 to 0x80010030 = 0x10 = 16 bytes
    auto* fb = finder.findByAddress(0x80010020);
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(fb->size, 0x10u);

    cleanupFile(path);
}

TEST(FunctionFinder, RecomputeBoundariesAssignsSizeToLateAddedFunction) {
    const std::string path = "/tmp/ps1recomp_test_ff_recompute.elf";

    // Three known functions via symbols at +0x00, +0x20, +0x50.
    // We will later inject a fourth at +0x40 via addFunction.
    std::vector<uint32_t> code(32, makeNOP());
    std::vector<std::pair<std::string, uint32_t>> syms = {
        {"func_a", 0x80010000},
        {"func_b", 0x80010020},
        {"func_c", 0x80010050}
    };

    createElfWithCode(path, code, 0x80010000, true, syms);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    // Inject an extra function inside the func_b range (post-detection).
    finder.addFunction(0x80010040, "func_added",
                       FunctionSource::Symbol);
    finder.recomputeBoundaries(elf);

    // The added function should get size = (next.address - addr) = 0x10.
    // Existing functions keep their pre-computed size (computeBoundaries
    // skips entries with size != 0).  Without `recomputeBoundaries` the
    // added function would have stayed at size=0 -- the bug this fixes.
    auto* added = finder.findByAddress(0x80010040);
    ASSERT_NE(added, nullptr);
    EXPECT_EQ(added->size, 0x10u);
    EXPECT_EQ(added->source, FunctionSource::Symbol);

    cleanupFile(path);
}

// Function Finder -- Leaf Detection

TEST(FunctionFinder, DetectsLeafFunctions) {
    const std::string path = "/tmp/ps1recomp_test_ff_leaf.elf";

    // func_a (leaf): no JAL, just NOP + JR $ra
    // func_b (non-leaf): has JAL + JR $ra
    std::vector<uint32_t> code(16, makeNOP());
    code[3]  = makeJR_RA();                         // func_a return (leaf)
    // func_b at +0x10
    code[4]  = makeADDIU_SP(-16);                   // func_b prologue
    code[5]  = makeJAL(0x80010014, 0x80010000);     // func_b calls func_a
    code[7]  = makeJR_RA();                         // func_b return

    std::vector<std::pair<std::string, uint32_t>> syms = {
        {"leaf_func", 0x80010000},
        {"caller_func", 0x80010010}
    };

    createElfWithCode(path, code, 0x80010000, true, syms);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    auto* leaf = finder.findByAddress(0x80010000);
    ASSERT_NE(leaf, nullptr);
    EXPECT_TRUE(leaf->isLeaf);

    auto* caller = finder.findByAddress(0x80010010);
    ASSERT_NE(caller, nullptr);
    EXPECT_FALSE(caller->isLeaf);

    cleanupFile(path);
}

// Function Finder -- findContaining

TEST(FunctionFinder, FindContaining) {
    const std::string path = "/tmp/ps1recomp_test_ff_containing.elf";

    std::vector<uint32_t> code(16, makeNOP());
    std::vector<std::pair<std::string, uint32_t>> syms = {
        {"func_a", 0x80010000},
        {"func_b", 0x80010020}
    };

    createElfWithCode(path, code, 0x80010000, true, syms);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    // Address inside func_a
    auto* f = finder.findContaining(0x80010010);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->address, 0x80010000u);

    // Exact start of func_b
    f = finder.findContaining(0x80010020);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->address, 0x80010020u);

    cleanupFile(path);
}

// Function Finder -- Empty Section

TEST(FunctionFinder, HandlesEmptyCode) {
    const std::string path = "/tmp/ps1recomp_test_ff_empty.elf";

    // Section with only NOPs (minimal)
    std::vector<uint32_t> code = {makeNOP()};
    createElfWithCode(path, code);

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    // Should at least have the entry point
    EXPECT_GE(finder.getFunctionCount(), 1u);

    cleanupFile(path);
}
