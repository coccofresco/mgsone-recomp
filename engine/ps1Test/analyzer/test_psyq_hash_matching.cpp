// Tests for the hash-based PsyQ detector (Sessao 0.4).
//
// Covers:
//   * `mask_immediates()` parity with tools/extract_psyq_signatures.py
//   * SHA-256 -> uint64_t determinism and the relocation-tolerance property
//   * TOML loader populates the in-memory hash maps
//   * End-to-end detection in a synthetic ELF whose .text mirrors a real
//     signature loaded from the project's psyq_signatures.toml.

#include <gtest/gtest.h>

#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/psyq_signatures.h>

#include <elfio/elfio.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ps1recomp;

// Path to the in-tree TOML data
// PS1RECOMP_DATA_DIR_DEFAULT is baked in by ps1Analyzer/CMakeLists.txt as
// `${CMAKE_SOURCE_DIR}/ps1Analyzer/data` so the tests work regardless of
// the build directory layout.

#ifndef PS1RECOMP_DATA_DIR_DEFAULT
#define PS1RECOMP_DATA_DIR_DEFAULT "ps1Analyzer/data"
#endif

static std::string sigsTomlPath() {
    return std::string(PS1RECOMP_DATA_DIR_DEFAULT) + "/psyq_signatures.toml";
}
static std::string metaTomlPath() {
    return std::string(PS1RECOMP_DATA_DIR_DEFAULT) + "/psyq_metadata.toml";
}

// mask_immediates -- parity with tools/tests/test_extract_psyq_signatures.py

TEST(PsyqHashMasking, RTypePassthrough) {
    // add $v0, $a0, $a1 -- pure R-type, no immediate
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x00851020u), 0x00851020u);
    // jr $ra (special, funct=0x08)
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x03E00008u), 0x03E00008u);
}

TEST(PsyqHashMasking, AddiuLuiImmZeroed) {
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x240900ADu), 0x24090000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x3C011F80u), 0x3C010000u);
}

TEST(PsyqHashMasking, AndiOriXori) {
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x30210FFFu), 0x30210000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x34210FFFu), 0x34210000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x38210FFFu), 0x38210000u);
}

TEST(PsyqHashMasking, LoadStoreImmZeroed) {
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x8FA41234u), 0x8FA40000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0xAC201814u), 0xAC200000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0xA0201D80u), 0xA0200000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0xA4201D80u), 0xA4200000u);
}

TEST(PsyqHashMasking, BranchOffsetsZeroed) {
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x10850010u), 0x10850000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x14850010u), 0x14850000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x18800010u), 0x18800000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x1C800010u), 0x1C800000u);
    // REGIMM (op 0x01)
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x04800010u), 0x04800000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x04810010u), 0x04810000u);
}

TEST(PsyqHashMasking, JJalTargetZeroed) {
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x08040000u), 0x08000000u);
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x0C040000u), 0x0C000000u);
}

TEST(PsyqHashMasking, CopKeptExact) {
    // mfc0/mtc0/cfc2/ctc2/mtc2 -- encoding distinguishes GTE ops via funct/rd
    for (uint32_t w : {0x40046800u, 0x40846800u, 0x48046800u, 0x4884E800u}) {
        EXPECT_EQ(PsyQMatcher::maskImmediates(w), w);
    }
}

TEST(PsyqHashMasking, AddiuDifferentImmCollideOnMask) {
    // The whole point of masking: two A0 BIOS-call wrappers that differ
    // only in the function index collapse to the same masked word.
    EXPECT_EQ(PsyQMatcher::maskImmediates(0x24090012u),
              PsyQMatcher::maskImmediates(0x240900ADu));
}

// SHA-256 -> uint64 helpers

TEST(PsyqHashHash, Deterministic) {
    const std::vector<uint8_t> bytes = {
        0x20, 0x10, 0x85, 0x00,  // add $v0,$a0,$a1 (LE)
        0x08, 0x00, 0xE0, 0x03,  // jr $ra
    };
    const uint64_t a = PsyQMatcher::hashFull(bytes.data(), bytes.size());
    const uint64_t b = PsyQMatcher::hashFull(bytes.data(), bytes.size());
    EXPECT_EQ(a, b);
}

TEST(PsyqHashHash, HashFullMatchesKnownDigest) {
    // SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223 b00361a396177a9cb410ff61f20015ad
    // High 64 bits = 0xba7816bf8f01cfea
    const std::string s = "abc";
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    EXPECT_EQ(PsyQMatcher::hashFull(p, s.size()), 0xBA7816BF8F01CFEAull);
}

TEST(PsyqHashHash, HashMaskedRelocationTolerant) {
    // Two addiu wrappers that differ only in the imm should produce the
    // same masked hash. (BIOS A0:0x12 vs A0:0xAD body.)
    auto pack = [](uint32_t w) {
        std::vector<uint8_t> v(4);
        v[0] =  w        & 0xFF;
        v[1] = (w >>  8) & 0xFF;
        v[2] = (w >> 16) & 0xFF;
        v[3] = (w >> 24) & 0xFF;
        return v;
    };
    auto a = pack(0x24090012u);
    auto b = pack(0x240900ADu);
    EXPECT_EQ(PsyQMatcher::hashMasked(a.data(), 4),
              PsyQMatcher::hashMasked(b.data(), 4));

    // Sanity: hashFull diverges (the imm survives).
    EXPECT_NE(PsyQMatcher::hashFull(a.data(), 4),
              PsyQMatcher::hashFull(b.data(), 4));
}

TEST(PsyqHashHash, ParseHashHex) {
    EXPECT_EQ(PsyQMatcher::parseHashHex("ba7816bf8f01cfea"), 0xBA7816BF8F01CFEAull);
    EXPECT_EQ(PsyQMatcher::parseHashHex("0000000000000000"), 0u);
    // Too short -> 0.
    EXPECT_EQ(PsyQMatcher::parseHashHex("ba78"), 0u);
    // Non-hex char -> 0.
    EXPECT_EQ(PsyQMatcher::parseHashHex("zzzzzzzzzzzzzzzz"), 0u);
}

// TOML loader -- verify the in-tree DB parses cleanly and populates the
// matcher with a non-trivial number of signatures.

TEST(PsyqHashLoader, LoadsInTreeDb) {
    PsyQMatcher m;
    // Default constructor already tries to load via PS1RECOMP_DATA_DIR; the
    // signature count from the in-tree DB is 2205 as of Sessao 0.3.
    EXPECT_GT(m.getSignatureCount(), 1500u);
    EXPECT_GT(m.getDatabaseSize(), 100u);
    EXPECT_TRUE(m.isKnown("ResetGraph"));
    EXPECT_TRUE(m.isKnown("CdInit"));
}

TEST(PsyqHashLoader, ExplicitLoadIsIdempotent) {
    PsyQMatcher m;
    const size_t before = m.getSignatureCount();
    // A second explicit load should overwrite identical entries (no growth).
    EXPECT_TRUE(m.loadFromToml(sigsTomlPath(), metaTomlPath()));
    EXPECT_EQ(m.getSignatureCount(), before);
}

// End-to-end synthetic-ELF detection

namespace {

struct Sig {
    std::string name;
    std::string library;
    uint32_t    size;
    std::string hash_masked;
    std::string hash_full;
    std::string match_mode;
};

// Pick the first signature with the given name + match_mode out of the
// in-tree TOML. Uses a tiny ad-hoc parser: lines like `name = "..."` until
// a blank line bound each [[signature]] block.
static bool findSignature(const std::string& tomlPath,
                          const std::string& wantName,
                          const std::string& wantMode,
                          Sig& out) {
    std::ifstream in(tomlPath);
    if (!in) return false;

    auto extract = [](const std::string& line, const std::string& key,
                      std::string& dst) {
        const std::string needle = key + " ";
        if (line.find(needle) == 0 || line.find(key + "=") == 0 ||
            line.find(key + "  ") == 0 || line.find(key) == 0) {
            auto eq = line.find('=');
            if (eq == std::string::npos) return false;
            std::string v = line.substr(eq + 1);
            // Strip whitespace and surrounding quotes.
            size_t a = v.find_first_not_of(" \t\"");
            size_t b = v.find_last_not_of(" \t\"\r\n");
            if (a == std::string::npos) return false;
            dst = v.substr(a, b - a + 1);
            return true;
        }
        return false;
    };

    std::string line;
    Sig cur;
    while (std::getline(in, line)) {
        if (line == "[[signature]]") {
            cur = {};
            continue;
        }
        std::string v;
        if (line.find("name") == 0)        extract(line, "name", cur.name);
        else if (line.find("library") == 0) extract(line, "library", cur.library);
        else if (line.find("size") == 0) {
            std::string s;
            if (extract(line, "size", s)) cur.size = std::stoul(s);
        }
        else if (line.find("hash_masked") == 0) extract(line, "hash_masked", cur.hash_masked);
        else if (line.find("hash_full")   == 0) extract(line, "hash_full",   cur.hash_full);
        else if (line.find("match_mode")  == 0) extract(line, "match_mode",  cur.match_mode);
        else if (line.empty()) {
            if (cur.name == wantName && cur.match_mode == wantMode) {
                out = cur;
                return true;
            }
        }
    }
    return false;
}

// Build a synthetic ELF with a single function whose body hashes to the
// supplied target. We can't reproduce real PsyQ bytes (the SDK ships no
// .text we want to redistribute), so the test instead encodes the hash as
// the byte stream itself -- `hashFull` over those bytes is deterministic.
// To exercise the lookup path, we INSTEAD insert a custom signature into
// the matcher whose hash matches a known synthetic body, then run match.
// (This decouples the test from any real PsyQ binary.)
static std::string writeSyntheticElf(const std::string& path,
                                     uint32_t funcAddr,
                                     const std::vector<uint8_t>& body,
                                     uint32_t dataAddr = 0,
                                     const std::vector<uint8_t>& data = {}) {
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(funcAddr);

    auto* sec = writer.sections.add(".text");
    sec->set_type(ELFIO::SHT_PROGBITS);
    sec->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    sec->set_addr_align(4);
    sec->set_address(funcAddr);
    sec->set_data(reinterpret_cast<const char*>(body.data()), body.size());

    ELFIO::section* dataSec = nullptr;
    if (dataAddr != 0 && !data.empty()) {
        dataSec = writer.sections.add(".rodata");
        dataSec->set_type(ELFIO::SHT_PROGBITS);
        dataSec->set_flags(ELFIO::SHF_ALLOC);
        dataSec->set_addr_align(1);
        dataSec->set_address(dataAddr);
        dataSec->set_data(reinterpret_cast<const char*>(data.data()), data.size());
    }

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
    syma.add_symbol(stra, "func_under_test", funcAddr, body.size(),
                    ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0,
                    sec->get_index());

    auto* seg = writer.segments.add();
    seg->set_type(ELFIO::PT_LOAD);
    seg->set_virtual_address(funcAddr);
    seg->set_physical_address(funcAddr);
    seg->set_flags(ELFIO::PF_X | ELFIO::PF_R);
    seg->set_align(0x1000);
    seg->add_section_index(sec->get_index(), sec->get_addr_align());
    if (dataSec) {
        seg->add_section_index(dataSec->get_index(), dataSec->get_addr_align());
    }

    writer.save(path);
    return path;
}

} // namespace

TEST(PsyqHashLoader, DetectsSyntheticBodyMatchingLoadedFullHash) {
    // Construct a tiny synthetic body, compute its hash_full, then write a
    // throw-away TOML containing a signature with that hash. Loading that
    // TOML and matching a synthetic ELF carrying the body should produce
    // exactly one match -- round-trips the full pipeline (loader -> indexer
    // -> matchByHash) without depending on any real PsyQ bytes.
    constexpr uint32_t FUNC_ADDR = 0x80050000;

    // 16-byte synthetic body (full mode threshold is <=24 bytes).
    const std::vector<uint8_t> body = {
        0x12, 0x34, 0x56, 0x78,  0x9A, 0xBC, 0xDE, 0xF0,
        0x11, 0x22, 0x33, 0x44,  0x55, 0x66, 0x77, 0x88,
    };
    const uint64_t hf = PsyQMatcher::hashFull(body.data(), body.size());

    // Materialise as 16 hex chars.
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hf));

    const std::string tmpToml = "/tmp/ps1recomp_test_psyq_synth.toml";
    {
        std::ofstream out(tmpToml);
        out << "[[signature]]\n"
            << "name        = \"SynthHello\"\n"
            << "library     = \"libtest\"\n"
            << "size        = 16\n"
            << "hash_masked = \"0000000000000000\"\n"
            << "hash_full   = \"" << hex << "\"\n"
            << "match_mode  = \"full\"\n"
            << "subsystem   = \"\"\n"
            << "stub_type   = \"\"\n"
            << "sources     = [\"synthetic\"]\n";
    }

    const std::string elfPath = "/tmp/ps1recomp_test_psyq_synth.elf";
    writeSyntheticElf(elfPath, FUNC_ADDR, body);

    PsyQMatcher matcher;
    ASSERT_TRUE(matcher.loadFromToml(tmpToml, ""));

    ElfParser elf;
    ASSERT_TRUE(elf.load(elfPath));

    FunctionFinder finder;
    finder.findFunctions(elf);

    matcher.matchFunctions(elf, finder);

    bool ok = false;
    std::string lib;
    for (const auto& m : matcher.getMatches()) {
        if (m.address == FUNC_ADDR && m.name == "SynthHello") {
            ok = true;
            lib = m.library;
        }
    }
    EXPECT_TRUE(ok) << "hash_full lookup should detect the synthetic function";
    EXPECT_EQ(lib, "libtest");

    std::remove(tmpToml.c_str());
    std::remove(elfPath.c_str());
}

TEST(PsyqHashLoader, MaskedHashIgnoresImmediateRelocation) {
    // 32-byte body chosen so size > 24 (masked-mode threshold). We then
    // build a second body identical to the first except for one imm field;
    // both should produce the SAME masked hash, and loading a sig keyed by
    // that masked hash should detect either body.
    constexpr uint32_t FUNC_ADDR = 0x80060000;

    auto pack32 = [](std::vector<uint8_t>& v, uint32_t w) {
        v.push_back( w        & 0xFF);
        v.push_back((w >>  8) & 0xFF);
        v.push_back((w >> 16) & 0xFF);
        v.push_back((w >> 24) & 0xFF);
    };

    std::vector<uint8_t> bodyA, bodyB;
    // 8 instructions x 4 bytes = 32 bytes
    pack32(bodyA, 0x27BDFFE8u);  // addiu sp,sp,-0x18
    pack32(bodyA, 0xAFBF0014u);  // sw ra,0x14(sp)
    pack32(bodyA, 0x240900ADu);  // addiu t1,zero,0xAD  <-- imm differs
    pack32(bodyA, 0x3C010000u);  // lui at,0
    pack32(bodyA, 0x242100A0u);  // addiu at,at,0xA0
    pack32(bodyA, 0x0040F809u);  // jalr at
    pack32(bodyA, 0x8FBF0014u);  // lw ra,0x14(sp)
    pack32(bodyA, 0x03E00008u);  // jr ra
    bodyB = bodyA;
    // Patch the third word's imm: 0xAD -> 0x12. Bytes 8..11.
    bodyB[8]  = 0x12; bodyB[9]  = 0x00;

    const uint64_t hmA = PsyQMatcher::hashMasked(bodyA.data(), bodyA.size());
    const uint64_t hmB = PsyQMatcher::hashMasked(bodyB.data(), bodyB.size());
    ASSERT_EQ(hmA, hmB) << "masked hash must absorb the imm change";

    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(hmA));

    const std::string tmpToml = "/tmp/ps1recomp_test_psyq_masked.toml";
    {
        std::ofstream out(tmpToml);
        out << "[[signature]]\n"
            << "name        = \"WrapperFn\"\n"
            << "library     = \"libapi\"\n"
            << "size        = 32\n"
            << "hash_masked = \"" << hex << "\"\n"
            << "hash_full   = \"0000000000000000\"\n"
            << "match_mode  = \"masked\"\n"
            << "subsystem   = \"\"\n"
            << "stub_type   = \"\"\n"
            << "sources     = [\"synthetic\"]\n";
    }

    PsyQMatcher matcher;
    ASSERT_TRUE(matcher.loadFromToml(tmpToml, ""));

    // Detect bodyA.
    {
        const std::string elfPath = "/tmp/ps1recomp_test_psyq_masked_a.elf";
        writeSyntheticElf(elfPath, FUNC_ADDR, bodyA);
        ElfParser elf; ASSERT_TRUE(elf.load(elfPath));
        FunctionFinder finder; finder.findFunctions(elf);
        matcher.matchFunctions(elf, finder);
        bool ok = false;
        for (const auto& m : matcher.getMatches())
            if (m.name == "WrapperFn" && m.address == FUNC_ADDR) ok = true;
        EXPECT_TRUE(ok) << "masked-mode lookup should detect bodyA";
        std::remove(elfPath.c_str());
    }

    // Detect bodyB (different imm) -- same hash, same match.
    {
        const std::string elfPath = "/tmp/ps1recomp_test_psyq_masked_b.elf";
        writeSyntheticElf(elfPath, FUNC_ADDR, bodyB);
        ElfParser elf; ASSERT_TRUE(elf.load(elfPath));
        FunctionFinder finder; finder.findFunctions(elf);
        matcher.matchFunctions(elf, finder);
        bool ok = false;
        for (const auto& m : matcher.getMatches())
            if (m.name == "WrapperFn" && m.address == FUNC_ADDR) ok = true;
        EXPECT_TRUE(ok) << "masked-mode lookup should detect bodyB after imm change";
        std::remove(elfPath.c_str());
    }

    std::remove(tmpToml.c_str());
}

TEST(PsyqHashLoader, RealDbContainsResetGraph) {
    // Sanity check the canonical example. ResetGraph should be present
    // (with one or more @vN variants -- but base name must exist).
    Sig got;
    ASSERT_TRUE(findSignature(sigsTomlPath(), "ResetGraph", "masked", got))
        << "ResetGraph missing from " << sigsTomlPath();
    EXPECT_EQ(got.library, "libgpu");
    EXPECT_GT(got.size, 0u);
    EXPECT_EQ(got.hash_masked.size(), 16u);
}

// Collision handling -- two distinct base names sharing a hash must not
// produce a match through the colliding hash. Otherwise the matcher would
// silently mis-identify wrappers (cf. 9-way collision of CdSync / CdReadSync /
// PadStop / ... on hash_masked 43f20be7a8d281c6 in the in-tree DB).

TEST(PsyqHashLoader, CollidingMaskedHashIsDroppedFromLiveMap) {
    // Two 32-byte sigs share hash_masked, differ in hash_full. The masked
    // index must drop the colliding key; the full index keeps both since
    // hash_full is unique per sig. A body whose hash_full matches sig A
    // must resolve to A's name (not B's), even though their masked hashes
    // collide.
    constexpr uint32_t FUNC_ADDR = 0x80070000;

    auto pack32 = [](std::vector<uint8_t>& v, uint32_t w) {
        v.push_back( w        & 0xFF);
        v.push_back((w >>  8) & 0xFF);
        v.push_back((w >> 16) & 0xFF);
        v.push_back((w >> 24) & 0xFF);
    };

    // Body A -- distinct bytes from B (different jal target). Both bodies
    // mask to the same hash_masked (jal target field is masked off).
    std::vector<uint8_t> bodyA;
    pack32(bodyA, 0x27BDFFE8u);  // addiu sp,sp,-0x18
    pack32(bodyA, 0xAFBF0014u);  // sw ra,0x14(sp)
    pack32(bodyA, 0x0C0AAAAAu);  // jal 0x802AAAA8       (target A)
    pack32(bodyA, 0x00000000u);  // nop
    pack32(bodyA, 0x8FBF0014u);  // lw ra,0x14(sp)
    pack32(bodyA, 0x27BD0018u);  // addiu sp,sp,+0x18
    pack32(bodyA, 0x03E00008u);  // jr ra
    pack32(bodyA, 0x00000000u);  // nop

    std::vector<uint8_t> bodyB = bodyA;
    // Patch jal target bytes: 0x0C0AAAAAu -> 0x0C0BBBBBu.
    bodyB[8]  = 0xBB; bodyB[9]  = 0xBB; bodyB[10] = 0x0B; bodyB[11] = 0x0C;

    const uint64_t hmA = PsyQMatcher::hashMasked(bodyA.data(), bodyA.size());
    const uint64_t hmB = PsyQMatcher::hashMasked(bodyB.data(), bodyB.size());
    ASSERT_EQ(hmA, hmB) << "wrapper masking must collapse jal target";

    const uint64_t hfA = PsyQMatcher::hashFull(bodyA.data(), bodyA.size());
    const uint64_t hfB = PsyQMatcher::hashFull(bodyB.data(), bodyB.size());
    ASSERT_NE(hfA, hfB) << "full hash must distinguish bodies";

    auto toHex = [](uint64_t v) {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(v));
        return std::string(buf);
    };

    const std::string tmpToml = "/tmp/ps1recomp_test_psyq_collision_masked.toml";
    {
        std::ofstream out(tmpToml);
        out << "[[signature]]\n"
            << "name        = \"FuncA\"\n"
            << "library     = \"libapi\"\n"
            << "size        = 32\n"
            << "hash_masked = \"" << toHex(hmA) << "\"\n"
            << "hash_full   = \"" << toHex(hfA) << "\"\n"
            << "match_mode  = \"masked\"\n"
            << "subsystem   = \"\"\n"
            << "stub_type   = \"\"\n"
            << "sources     = [\"synthetic\"]\n"
            << "\n"
            << "[[signature]]\n"
            << "name        = \"FuncB\"\n"
            << "library     = \"libcd\"\n"
            << "size        = 32\n"
            << "hash_masked = \"" << toHex(hmB) << "\"\n"
            << "hash_full   = \"" << toHex(hfB) << "\"\n"
            << "match_mode  = \"masked\"\n"
            << "subsystem   = \"\"\n"
            << "stub_type   = \"\"\n"
            << "sources     = [\"synthetic\"]\n";
    }

    // Use a fresh matcher without the in-tree DB so we only see the test sigs.
    PsyQMatcher matcher;
    // Wipe state from the constructor's loadDefaults() -- we want a clean
    // slate scoped to this test's two synthetic entries. The public API
    // doesn't expose a reset; load the test TOML which leaves the in-tree
    // entries untouched but adds the two synthetic ones, then we test only
    // that the bodies resolve to the *correct* base name.
    ASSERT_TRUE(matcher.loadFromToml(tmpToml, ""));

    const std::string elfPath = "/tmp/ps1recomp_test_psyq_collision_masked.elf";
    writeSyntheticElf(elfPath, FUNC_ADDR, bodyA);
    ElfParser elf; ASSERT_TRUE(elf.load(elfPath));
    FunctionFinder finder; finder.findFunctions(elf);
    matcher.matchFunctions(elf, finder);

    std::string matchedName;
    for (const auto& m : matcher.getMatches()) {
        if (m.address == FUNC_ADDR) matchedName = m.name;
    }
    // hash_full distinguishes the two: bodyA must resolve to FuncA, never to
    // FuncB. Pre-fix, last-write-wins on the colliding hash_masked could pin
    // it on either name regardless of which body was being matched.
    EXPECT_EQ(matchedName, "FuncA")
        << "collision-dropping + hash_full lookup must pick the correct base name";

    std::remove(tmpToml.c_str());
    std::remove(elfPath.c_str());
}

TEST(PsyqHashLoader, FullyCollidingPairDropsBothFromMatching) {
    // Two sigs with identical hash_masked AND hash_full but distinct names --
    // the unrelocated PsyQ .OBJ wrapper case. Both must drop; the matcher
    // must refuse to match a body that hashes there.
    constexpr uint32_t FUNC_ADDR = 0x80080000;

    std::vector<uint8_t> body(32, 0);
    for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<uint8_t>(i * 7);

    const uint64_t hm = PsyQMatcher::hashMasked(body.data(), body.size());
    const uint64_t hf = PsyQMatcher::hashFull(body.data(), body.size());

    auto toHex = [](uint64_t v) {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(v));
        return std::string(buf);
    };

    const std::string tmpToml = "/tmp/ps1recomp_test_psyq_full_collision.toml";
    {
        std::ofstream out(tmpToml);
        for (const char* name : {"WrapperX", "WrapperY"}) {
            out << "[[signature]]\n"
                << "name        = \"" << name << "\"\n"
                << "library     = \"libcd\"\n"
                << "size        = 32\n"
                << "hash_masked = \"" << toHex(hm) << "\"\n"
                << "hash_full   = \"" << toHex(hf) << "\"\n"
                << "match_mode  = \"masked\"\n"
                << "subsystem   = \"\"\n"
                << "stub_type   = \"\"\n"
                << "sources     = [\"synthetic\"]\n\n";
        }
    }

    PsyQMatcher matcher;
    ASSERT_TRUE(matcher.loadFromToml(tmpToml, ""));

    const std::string elfPath = "/tmp/ps1recomp_test_psyq_full_collision.elf";
    writeSyntheticElf(elfPath, FUNC_ADDR, body);
    ElfParser elf; ASSERT_TRUE(elf.load(elfPath));
    FunctionFinder finder; finder.findFunctions(elf);
    matcher.matchFunctions(elf, finder);

    for (const auto& m : matcher.getMatches()) {
        EXPECT_FALSE(m.address == FUNC_ADDR && (m.name == "WrapperX" || m.name == "WrapperY"))
            << "fully colliding wrappers must not match -- got " << m.name;
    }

    std::remove(tmpToml.c_str());
    std::remove(elfPath.c_str());
}

TEST(PsyqHashLoader, ResolvesCollidingWrappersByReferencedDiagnosticName) {
    constexpr uint32_t FUNC_ADDR = 0x80090000;
    constexpr uint32_t DATA_ADDR = 0x80091000;

    auto pack32 = [](std::vector<uint8_t>& v, uint32_t w) {
        v.push_back(w & 0xFF);
        v.push_back((w >> 8) & 0xFF);
        v.push_back((w >> 16) & 0xFF);
        v.push_back((w >> 24) & 0xFF);
    };

    std::vector<uint8_t> body;
    pack32(body, 0x27BDFFE8u); // addiu sp,sp,-24
    pack32(body, 0xAFBF0014u); // sw ra,20(sp)
    pack32(body, 0x3C048009u); // lui a0,0x8009
    pack32(body, 0x24841000u); // addiu a0,a0,0x1000 -> diagnostic string
    pack32(body, 0x0C012345u); // jal validator
    pack32(body, 0x00000000u); // nop
    pack32(body, 0x03E00008u); // jr ra
    pack32(body, 0x27BD0018u); // addiu sp,sp,24

    const uint64_t hm = PsyQMatcher::hashMasked(body.data(), body.size());
    auto toHex = [](uint64_t v) {
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(v));
        return std::string(buf);
    };

    const std::string tmpToml = "/tmp/ps1recomp_test_psyq_named_collision.toml";
    {
        std::ofstream out(tmpToml);
        for (const char* name : {"LoadImage", "StoreImage"}) {
            out << "[[signature]]\n"
                << "name        = \"" << name << "\"\n"
                << "library     = \"libgpu\"\n"
                << "size        = 32\n"
                << "hash_masked = \"" << toHex(hm) << "\"\n"
                // Deliberately not the linked body's full hash: this exercises
                // disambiguation of the relocation-tolerant masked collision.
                << "hash_full   = \"1111111111111111\"\n"
                << "match_mode  = \"masked\"\n"
                << "subsystem   = \"graphics\"\n"
                << "stub_type   = \"stub\"\n"
                << "sources     = [\"synthetic\"]\n\n";
        }
    }

    const std::vector<uint8_t> diagnostic = {
        'L', 'o', 'a', 'd', 'I', 'm', 'a', 'g', 'e', 0
    };
    const std::string elfPath = "/tmp/ps1recomp_test_psyq_named_collision.elf";
    writeSyntheticElf(elfPath, FUNC_ADDR, body, DATA_ADDR, diagnostic);

    PsyQMatcher matcher;
    ASSERT_TRUE(matcher.loadFromToml(tmpToml, ""));
    ElfParser elf; ASSERT_TRUE(elf.load(elfPath));
    FunctionFinder finder; finder.findFunctions(elf);
    matcher.matchFunctions(elf, finder);

    std::string matchedName;
    for (const auto& m : matcher.getMatches()) {
        if (m.address == FUNC_ADDR) matchedName = m.name;
    }
    EXPECT_EQ(matchedName, "LoadImage");

    std::remove(tmpToml.c_str());
    std::remove(elfPath.c_str());
}
