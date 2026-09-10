// Tests for ps1Analyzer -- Config Generator
// Validates TOML generation from analyzer results

#include <gtest/gtest.h>
#include <ps1recomp/config_generator.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/psyq_signatures.h>
#include <elfio/elfio.hpp>
#include <toml.hpp>
#include <cstdio>
#include <sstream>

using namespace ps1recomp;

// Helper: Create test ELF with known content

static std::string createTestElf(
    const std::string& path,
    const std::vector<std::pair<std::string, uint32_t>>& funcs,
    const std::vector<uint32_t>& code = {}) {

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

    // Use provided code or fill with NOPs
    std::vector<uint8_t> text_data;
    if (!code.empty()) {
        text_data.resize(code.size() * 4);
        for (size_t i = 0; i < code.size(); ++i) {
            text_data[i * 4 + 0] = (code[i] >>  0) & 0xFF;
            text_data[i * 4 + 1] = (code[i] >>  8) & 0xFF;
            text_data[i * 4 + 2] = (code[i] >> 16) & 0xFF;
            text_data[i * 4 + 3] = (code[i] >> 24) & 0xFF;
        }
    } else {
        text_data.resize(0x1000, 0x00); // 4KB of NOPs
    }
    text_sec->set_data(reinterpret_cast<const char*>(text_data.data()), text_data.size());

    // .data section
    auto* data_sec = writer.sections.add(".data");
    data_sec->set_type(ELFIO::SHT_PROGBITS);
    data_sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_WRITE);
    data_sec->set_addr_align(4);
    data_sec->set_address(0x80020000);
    std::vector<uint8_t> data_data(256, 0xAA);
    data_sec->set_data(reinterpret_cast<const char*>(data_data.data()), data_data.size());

    // Add symbol table
    if (!funcs.empty()) {
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

// Basic Generation Tests

TEST(ConfigGenerator, GeneratesValidTOML) {
    const std::string path = "/tmp/ps1recomp_test_config_basic.elf";

    createTestElf(path, {
        {"main",       0x80010000},
        {"game_loop",  0x80010100}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    // Should not be empty
    EXPECT_FALSE(tomlStr.empty());

    // Should parse as valid TOML
    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    // Check [binary] section
    EXPECT_TRUE(parsed.contains("binary"));
    auto& binary = parsed.at("binary");
    EXPECT_EQ(toml::find<std::string>(binary, "entry_point"), "0x80010000");

    cleanupFile(path);
}

TEST(ConfigGenerator, ContainsBinarySection) {
    const std::string path = "/tmp/ps1recomp_test_config_binary.elf";

    createTestElf(path, {{"main", 0x80010000}});

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    auto& binary = parsed.at("binary");
    EXPECT_EQ(toml::find<std::string>(binary, "path"), path);
    EXPECT_EQ(toml::find<std::string>(binary, "entry_point"), "0x80010000");
    EXPECT_GT(toml::find<int64_t>(binary, "total_code_size"), 0);

    cleanupFile(path);
}

TEST(ConfigGenerator, ContainsStatsSection) {
    const std::string path = "/tmp/ps1recomp_test_config_stats.elf";

    createTestElf(path, {
        {"main",        0x80010000},
        {"GsInitGraph", 0x800B0000}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    EXPECT_TRUE(parsed.contains("stats"));
    auto& stats = parsed.at("stats");
    EXPECT_GE(toml::find<int64_t>(stats, "total_functions"), 1);
    EXPECT_GE(toml::find<int64_t>(stats, "psyq_functions"), 1);

    cleanupFile(path);
}

// Function List Tests

TEST(ConfigGenerator, ListsGameFunctions) {
    const std::string path = "/tmp/ps1recomp_test_config_funcs.elf";

    createTestElf(path, {
        {"main",         0x80010000},
        {"game_update",  0x80010100}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    EXPECT_TRUE(parsed.contains("functions"));
    auto& funcs = parsed.at("functions");
    EXPECT_GE(funcs.as_array().size(), 1u);

    // Check first function has required fields
    auto& first = funcs.as_array().at(0);
    EXPECT_TRUE(first.contains("name"));
    EXPECT_TRUE(first.contains("address"));
    EXPECT_TRUE(first.contains("size"));
    EXPECT_TRUE(first.contains("source"));
    EXPECT_TRUE(first.contains("leaf"));

    cleanupFile(path);
}

// PsyQ Classification Tests

TEST(ConfigGenerator, SeparatesPsyQFunctions) {
    const std::string path = "/tmp/ps1recomp_test_config_psyq.elf";

    createTestElf(path, {
        {"main",         0x80010000},
        {"game_update",  0x80010100},
        {"GsInitGraph",  0x800B0000},  // stub
        {"printf",       0x800D0000},  // skip
        {"memcpy",       0x800D0100}   // passthrough
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);

    // Stubs should contain GsInitGraph
    EXPECT_TRUE(parsed.contains("stubs"));
    bool hasGsInit = false;
    for (const auto& s : parsed.at("stubs").as_array()) {
        if (toml::find<std::string>(s, "name") == "GsInitGraph") {
            hasGsInit = true;
            EXPECT_TRUE(s.contains("subsystem"));
        }
    }
    EXPECT_TRUE(hasGsInit);

    // Skips should contain printf
    EXPECT_TRUE(parsed.contains("skips"));
    bool hasPrintf = false;
    for (const auto& s : parsed.at("skips").as_array()) {
        if (toml::find<std::string>(s, "name") == "printf") {
            hasPrintf = true;
        }
    }
    EXPECT_TRUE(hasPrintf);

    // Passthroughs should contain memcpy
    EXPECT_TRUE(parsed.contains("passthroughs"));
    bool hasMemcpy = false;
    for (const auto& s : parsed.at("passthroughs").as_array()) {
        if (toml::find<std::string>(s, "name") == "memcpy") {
            hasMemcpy = true;
        }
    }
    EXPECT_TRUE(hasMemcpy);

    // Game functions should NOT contain PsyQ functions
    for (const auto& f : parsed.at("functions").as_array()) {
        auto name = toml::find<std::string>(f, "name");
        EXPECT_NE(name, "GsInitGraph") << "PsyQ stub should not be in functions";
        EXPECT_NE(name, "printf") << "PsyQ skip should not be in functions";
        EXPECT_NE(name, "memcpy") << "PsyQ passthrough should not be in functions";
    }

    cleanupFile(path);
}

TEST(ConfigGenerator, KeepsRecompileMatchesInFunctionList) {
    const std::string path = "/tmp/ps1recomp_test_config_recompile.elf";

    createTestElf(path, {
        {"main",          0x80010000},
        {"SetGraphDebug", 0x80010100}
    });

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::istringstream is(gen.generateString(elf, finder, matcher, path));
    auto parsed = toml::parse(is);

    bool hasFunctionBody = false;
    for (const auto& f : parsed.at("functions").as_array()) {
        if (toml::find<std::string>(f, "name") == "SetGraphDebug") {
            hasFunctionBody = true;
            break;
        }
    }
    EXPECT_TRUE(hasFunctionBody);

    cleanupFile(path);
}

// File Write Test

TEST(ConfigGenerator, WritesToFile) {
    const std::string elfPath = "/tmp/ps1recomp_test_config_file.elf";
    const std::string outPath = "/tmp/ps1recomp_test_config_output.toml";

    createTestElf(elfPath, {{"main", 0x80010000}});

    ElfParser elf;
    ASSERT_TRUE(elf.load(elfPath));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    EXPECT_TRUE(gen.generate(elf, finder, matcher, elfPath, outPath));

    // Read back and parse
    auto parsed = toml::parse(outPath);
    EXPECT_TRUE(parsed.contains("binary"));
    EXPECT_TRUE(parsed.contains("stats"));
    EXPECT_TRUE(parsed.contains("functions"));

    cleanupFile(elfPath);
    cleanupFile(outPath);
}

TEST(ConfigGenerator, ReportsFileError) {
    const std::string elfPath = "/tmp/ps1recomp_test_config_err.elf";

    createTestElf(elfPath, {{"main", 0x80010000}});

    ElfParser elf;
    ASSERT_TRUE(elf.load(elfPath));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    // Invalid path should fail
    EXPECT_FALSE(gen.generate(elf, finder, matcher, elfPath,
                              "/nonexistent/dir/config.toml"));
    EXPECT_FALSE(gen.getError().empty());

    cleanupFile(elfPath);
}

// Empty Input Test

TEST(ConfigGenerator, HandleEmptyAnalysis) {
    const std::string path = "/tmp/ps1recomp_test_config_empty.elf";

    createTestElf(path, {}); // No symbols

    ElfParser elf;
    ASSERT_TRUE(elf.load(path));

    FunctionFinder finder;
    finder.findFunctions(elf);

    PsyQMatcher matcher;
    matcher.matchFunctions(elf, finder);

    ConfigGenerator gen;
    std::string tomlStr = gen.generateString(elf, finder, matcher, path);

    // Should still produce valid TOML
    std::istringstream is(tomlStr);
    auto parsed = toml::parse(is);
    EXPECT_TRUE(parsed.contains("binary"));
    EXPECT_TRUE(parsed.contains("stats"));

    cleanupFile(path);
}
