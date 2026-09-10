// Tests for ps1Analyzer -- PsyQ Signature Matcher
// Validates database, name matching, prefix classification, and stub assignment

#include <gtest/gtest.h>
#include <ps1recomp/psyq_signatures.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <elfio/elfio.hpp>
#include <cstdio>

using namespace ps1recomp;

// Helper: Create ELF with PsyQ-like symbols

static std::string createPsyQElf(
    const std::string& path,
    const std::vector<std::pair<std::string, uint32_t>>& funcs) {

    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x80010000);

    // .text section
    auto* text_sec = writer.sections.add(".text");
    text_sec->set_type(ELFIO::SHT_PROGBITS);
    text_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text_sec->set_addr_align(4);
    text_sec->set_address(0x80010000);

    // Fill with NOPs (enough space for all functions)
    std::vector<uint8_t> text_data(0x100000, 0x00);
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()), text_data.size());

    // Add symbol table
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
        syma.add_symbol(stra, name.c_str(), addr, 64,
                        ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                        text_sec->get_index());
    }

    auto* segment = writer.segments.add();
    segment->set_type(ELFIO::PT_LOAD);
    segment->set_virtual_address(0x80010000);
    segment->set_physical_address(0x80010000);
    segment->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    segment->set_align(0x1000);
    segment->add_section_index(text_sec->get_index(), text_sec->get_addr_align());

    writer.save(path);
    return path;
}

static void cleanupFile(const std::string& path) {
    std::remove(path.c_str());
}

// Database Tests

TEST(PsyQMatcher, DatabaseNotEmpty) {
    PsyQMatcher matcher;
    EXPECT_GT(matcher.getDatabaseSize(), 70u);
}

TEST(PsyQMatcher, KnownFunctions) {
    PsyQMatcher matcher;
    EXPECT_TRUE(matcher.isKnown("GsInitGraph"));
    EXPECT_TRUE(matcher.isKnown("SpuInit"));
    EXPECT_TRUE(matcher.isKnown("CdInit"));
    EXPECT_TRUE(matcher.isKnown("PadInit"));
    EXPECT_TRUE(matcher.isKnown("VSync"));
    EXPECT_TRUE(matcher.isKnown("malloc"));
    EXPECT_TRUE(matcher.isKnown("memcpy"));
    EXPECT_TRUE(matcher.isKnown("printf"));
    EXPECT_TRUE(matcher.isKnown("RotTransPers"));
    EXPECT_TRUE(matcher.isKnown("rsin"));
    EXPECT_FALSE(matcher.isKnown("game_update"));
    EXPECT_FALSE(matcher.isKnown("player_move"));
}

// Subsystem Classification Tests

TEST(PsyQMatcher, ClassifyGraphics) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("GsInitGraph"), PsyQSubsystem::Graphics);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("DrawOTag"), PsyQSubsystem::Graphics);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("PutDrawEnv"), PsyQSubsystem::Graphics);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("FntPrint"), PsyQSubsystem::Graphics);
}

TEST(PsyQMatcher, ClassifySound) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("SpuInit"), PsyQSubsystem::Sound);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("SsInit"), PsyQSubsystem::Sound);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("SsVabOpenHead"), PsyQSubsystem::Sound);
}

TEST(PsyQMatcher, ClassifyCDROM) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("CdInit"), PsyQSubsystem::CDROM);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("CdRead"), PsyQSubsystem::CDROM);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("CdSearchFile"), PsyQSubsystem::CDROM);
}

TEST(PsyQMatcher, ClassifyController) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("PadInit"), PsyQSubsystem::Controller);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("PadRead"), PsyQSubsystem::Controller);
}

TEST(PsyQMatcher, ClassifyGTE) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("gte_ldv0"), PsyQSubsystem::GTE);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("RotTransPers"), PsyQSubsystem::GTE);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("TransMatrix"), PsyQSubsystem::GTE);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("NormalClip"), PsyQSubsystem::GTE);
}

TEST(PsyQMatcher, ClassifyVSync) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("VSync"), PsyQSubsystem::VSync);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("DrawSync"), PsyQSubsystem::VSync);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("ResetCallback"), PsyQSubsystem::VSync);
}

TEST(PsyQMatcher, ClassifyMemory) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("malloc"), PsyQSubsystem::Memory);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("free"), PsyQSubsystem::Memory);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("InitHeap"), PsyQSubsystem::Memory);
}

TEST(PsyQMatcher, ClassifyLibC) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("printf"), PsyQSubsystem::LibC);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("memcpy"), PsyQSubsystem::LibC);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("strlen"), PsyQSubsystem::LibC);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("rand"), PsyQSubsystem::LibC);
}

TEST(PsyQMatcher, ClassifyMath) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("rsin"), PsyQSubsystem::Math);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("rcos"), PsyQSubsystem::Math);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("ratan2"), PsyQSubsystem::Math);
}

TEST(PsyQMatcher, ClassifyOther) {
    EXPECT_EQ(PsyQMatcher::classifySubsystem("game_loop"), PsyQSubsystem::Other);
    EXPECT_EQ(PsyQMatcher::classifySubsystem("update_physics"), PsyQSubsystem::Other);
    EXPECT_EQ(PsyQMatcher::classifySubsystem(""), PsyQSubsystem::Other);
}

// Name Matching Tests

TEST(PsyQMatcher, MatchGraphicsFunctions) {
    const std::string path = "/tmp/ps1recomp_test_psyq_gfx.elf";

    createPsyQElf(path, {
        {"main",         0x80010000},
        {"GsInitGraph",  0x800B0000},
        {"GsDrawOt",     0x800B0100},
        {"DrawSync",     0x800B0200},
        {"game_update",  0x80020000}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    // Should match GsInitGraph, GsDrawOt, DrawSync
    EXPECT_GE(matcher.getMatchCount(), 3u);

    // Stubs should include GPU functions
    auto stubs = matcher.getStubs();
    EXPECT_GE(stubs.size(), 2u); // GsInitGraph, GsDrawOt are stubs

    cleanupFile(path);
}

TEST(PsyQMatcher, MatchSoundFunctions) {
    const std::string path = "/tmp/ps1recomp_test_psyq_snd.elf";

    createPsyQElf(path, {
        {"main",     0x80010000},
        {"SpuInit",  0x800C0000},
        {"SpuSetKey",0x800C0100}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    EXPECT_GE(matcher.getMatchCount(), 2u);

    // Both are stubs
    auto stubs = matcher.getStubs();
    bool found_spu_init = false;
    for (const auto* s : stubs) {
        if (s->name == "SpuInit") found_spu_init = true;
    }
    EXPECT_TRUE(found_spu_init);

    cleanupFile(path);
}

TEST(PsyQMatcher, MarksImplementedLibcdFunctionsAsRuntimeStubs) {
    const std::string path = "/tmp/ps1recomp_test_psyq_libcd.elf";

    createPsyQElf(path, {
        {"main",        0x80010000},
        {"CdControlF",  0x800B0000},
        {"CdSync",      0x800B0100},
        {"CD_datasync", 0x800B0200}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    bool foundControlF = false;
    bool foundSync = false;
    bool foundDataSync = false;
    for (const auto* stub : matcher.getStubs()) {
        if (stub->name == "CdControlF" && stub->address == 0x800B0000) {
            foundControlF = true;
        }
        if (stub->name == "CdSync" && stub->address == 0x800B0100) {
            foundSync = true;
        }
        if (stub->name == "CD_datasync" && stub->address == 0x800B0200) {
            foundDataSync = true;
        }
    }
    EXPECT_TRUE(foundControlF);
    EXPECT_TRUE(foundSync);
    EXPECT_TRUE(foundDataSync);

    cleanupFile(path);
}

TEST(PsyQMatcher, MatchPassthroughs) {
    const std::string path = "/tmp/ps1recomp_test_psyq_pass.elf";

    createPsyQElf(path, {
        {"main",   0x80010000},
        {"memcpy", 0x800D0000},
        {"malloc", 0x800D0100},
        {"printf", 0x800D0200}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    auto passthroughs = matcher.getPassthroughs();
    EXPECT_GE(passthroughs.size(), 2u); // memcpy, malloc

    auto skips = matcher.getSkips();
    EXPECT_GE(skips.size(), 1u); // printf

    cleanupFile(path);
}

TEST(PsyQMatcher, NoMatchForGameFunctions) {
    const std::string path = "/tmp/ps1recomp_test_psyq_game.elf";

    createPsyQElf(path, {
        {"main",           0x80010000},
        {"game_init",      0x80020000},
        {"player_update",  0x80030000},
        {"render_scene",   0x80040000}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    // None of these should match PsyQ
    EXPECT_EQ(matcher.getMatchCount(), 0u);

    cleanupFile(path);
}

// Pass 3: Byte Signature Detection Tests

// Helper: write a 32-bit LE word into a byte buffer at position
static void writeLE32(std::vector<uint8_t>& buf, size_t offset, uint32_t val) {
    buf[offset + 0] = (val >>  0) & 0xFF;
    buf[offset + 1] = (val >>  8) & 0xFF;
    buf[offset + 2] = (val >> 16) & 0xFF;
    buf[offset + 3] = (val >> 24) & 0xFF;
}

// Create an ELF whose .text section contains synthetic instruction bytes at
// specific offsets relative to 0x80010000.
static std::string createSyntheticELF(
        const std::string& path,
        // (name, address) pairs -- these are func_ style (no symbols)
        const std::vector<std::pair<std::string,uint32_t>>& funcs,
        // (address, instruction_word) pairs to inject
        const std::vector<std::pair<uint32_t,uint32_t>>& instrs) {

    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x80010000);

    constexpr uint32_t TEXT_BASE = 0x80010000;
    constexpr size_t   TEXT_SIZE = 0x100000; // 1MB

    auto* text_sec = writer.sections.add(".text");
    text_sec->set_type(ELFIO::SHT_PROGBITS);
    text_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text_sec->set_addr_align(4);
    text_sec->set_address(TEXT_BASE);

    std::vector<uint8_t> text_data(TEXT_SIZE, 0x00);
    for (const auto& [addr, word] : instrs) {
        if (addr >= TEXT_BASE && addr < TEXT_BASE + TEXT_SIZE) {
            writeLE32(text_data, addr - TEXT_BASE, word);
        }
    }
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()),
                       text_data.size());

    // Add function symbols (STT_FUNC) so FunctionFinder can find them
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
        syma.add_symbol(stra, name.c_str(), addr, 128,
                        ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                        text_sec->get_index());
    }

    auto* seg = writer.segments.add();
    seg->set_type(ELFIO::PT_LOAD);
    seg->set_virtual_address(TEXT_BASE);
    seg->set_physical_address(TEXT_BASE);
    seg->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    seg->set_align(0x1000);
    seg->add_section_index(text_sec->get_index(), text_sec->get_addr_align());

    writer.save(path);
    return path;
}

// Note: the prior Pass3 byte-pattern detection tests
// (Pass3DetectsSpuInitBySPURegWrite, Pass3DetectsCdInitByBIOSCallIndex)
// were removed in Sessao 0.4 -- the InstrPattern/ByteSig matcher is gone.
// Equivalent coverage now lives in test_psyq_hash_matching.cpp, which uses
// real SHA-256 hashes from psyq_signatures.toml.

TEST(PsyQMatcher, Pass3DoesNotMatchUnrelatedFunction) {
    // A function with only NOPs -- should NOT match any signature
    constexpr uint32_t FUNC_ADDR = 0x80070000;

    createSyntheticELF("/tmp/ps1recomp_test_psyq_p3_nop.elf",
        {{"func_80070000", FUNC_ADDR}},
        {} /* no injected instructions -- all NOPs */);

    ElfParser elf;
    ASSERT_TRUE(elf.load("/tmp/ps1recomp_test_psyq_p3_nop.elf"));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    // NOP-only function should not trigger any signature
    EXPECT_EQ(matcher.getMatchCount(), 0u)
        << "NOP-only function should not match any PsyQ signature";

    std::remove("/tmp/ps1recomp_test_psyq_p3_nop.elf");
}

// String Helper Tests

TEST(PsyQMatcher, SubsystemNames) {
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Graphics), "Graphics");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Sound), "Sound");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::CDROM), "CD-ROM");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Controller), "Controller");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::GTE), "GTE");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::VSync), "VSync");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Memory), "Memory");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::LibC), "LibC");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Math), "Math");
    EXPECT_STREQ(PsyQMatcher::subsystemName(PsyQSubsystem::Other), "Other");
}

TEST(PsyQMatcher, StubTypeNames) {
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Stub), "stub");
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Skip), "skip");
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Passthrough), "passthrough");
    EXPECT_STREQ(PsyQMatcher::stubTypeName(StubType::Recompile), "recompile");
}
