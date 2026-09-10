// ps1Analyzer -- PsyQ Signature Matcher Implementation
//
// Identifies PsyQ SDK functions by SHA-256 hash of their .text bytes.
// Two hash modes mirror the generator in tools/extract_psyq_signatures.py:
//   * `hash_full`   -- bytes verbatim. Used for tiny BIOS A0/B0/C0 wrappers
//                     (size <= 24) where the function index is the only
//                     distinguishing byte and would be lost to masking.
//   * `hash_masked` -- each 32-bit word run through `maskImmediates()` first.
//                     Tolerant to relocations (the link-time imm fields).
//
// Both inputs are run through SHA-256 and truncated to the high 64 bits;
// the same truncation the Python generator writes to the TOML.

#include "ps1recomp/psyq_signatures.h"

#include "ps1recomp/elf_parser.h"
#include "ps1recomp/function_finder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include <fmt/format.h>
#include <toml.hpp>

namespace ps1recomp {

namespace {

// SHA-256 (public-domain reference, compact)
//
// Operates on a single buffer in one shot -- adequate for the small (<= 64KB)
// function bodies we hash. Output is the 32-byte digest in big-endian order;
// the matcher only ever reads the high 8 bytes.

constexpr uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t ror32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

void sha256_compress(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16)
             | (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19)  ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

// One-shot SHA-256. Writes the full 32-byte digest to `out`.
void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t state[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };

    size_t full_blocks = len / 64;
    for (size_t i = 0; i < full_blocks; ++i) {
        sha256_compress(state, data + i * 64);
    }

    // Tail block(s): copy remainder, append 0x80, pad with zeroes, append
    // length in bits as a 64-bit big-endian word at the end.
    uint8_t tail[128] = {};
    size_t rem = len - full_blocks * 64;
    if (rem > 0) std::memcpy(tail, data + full_blocks * 64, rem);
    tail[rem] = 0x80;
    size_t pad_target = (rem < 56) ? 64 : 128;
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) {
        tail[pad_target - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    }
    sha256_compress(state, tail);
    if (pad_target == 128) sha256_compress(state, tail + 64);

    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(state[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
}

// High 64 bits of SHA-256(data) as host uint64_t. Matches the Python
// generator, which formats `hashlib.sha256(...).hexdigest()[:16]`: those
// are the 8 high-order bytes in big-endian order.
uint64_t sha256_high64(const uint8_t* data, size_t len) {
    uint8_t digest[32];
    sha256(data, len, digest);
    uint64_t h = 0;
    for (int i = 0; i < 8; ++i) h = (h << 8) | digest[i];
    return h;
}

// String helpers

PsyQSubsystem subsystemFromString(const std::string& s) {
    if (s == "graphics")   return PsyQSubsystem::Graphics;
    if (s == "sound")      return PsyQSubsystem::Sound;
    if (s == "cdrom")      return PsyQSubsystem::CDROM;
    if (s == "controller") return PsyQSubsystem::Controller;
    if (s == "gte")        return PsyQSubsystem::GTE;
    if (s == "vsync")      return PsyQSubsystem::VSync;
    if (s == "memory")     return PsyQSubsystem::Memory;
    if (s == "libc")       return PsyQSubsystem::LibC;
    if (s == "math")       return PsyQSubsystem::Math;
    return PsyQSubsystem::Other;
}

StubType stubTypeFromString(const std::string& s) {
    if (s == "stub")        return StubType::Stub;
    if (s == "skip")        return StubType::Skip;
    if (s == "passthrough") return StubType::Passthrough;
    return StubType::Recompile;
}

std::string stripVariantSuffix(const std::string& name) {
    auto pos = name.find("@v");
    return pos == std::string::npos ? name : name.substr(0, pos);
}

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    std::ifstream f(path);
    return f.good();
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

bool stringAtAddressEquals(const ElfParser& elf, uint32_t address,
                           const std::string& expected) {
    const Section* sec = elf.findSectionByAddress(address);
    if (!sec || !sec->data) return false;

    const size_t offset = static_cast<size_t>(address - sec->vaddr);
    if (offset + expected.size() >= sec->size) return false;
    return std::memcmp(sec->data + offset, expected.data(), expected.size()) == 0
        && sec->data[offset + expected.size()] == 0;
}

// Some PsyQ wrappers are byte-identical in the .OBJ archive because their
// only differences are relocations. Once linked, however, they load a literal
// diagnostic name before calling a validator (LoadImage/StoreImage are the
// important pair). Track simple LUI + ADDIU/ORI constants and accept an
// ambiguous candidate only when the function itself references an exact
// NUL-terminated candidate name in the loaded image.
template <typename Candidates>
auto resolveByReferencedName(
    const ElfParser& elf, const uint8_t* bytes, size_t size,
    const Candidates& candidates) {
    std::array<uint32_t, 32> values{};
    std::array<bool, 32> known{};
    known[0] = true;

    auto resolved = candidates.empty() ? nullptr : &candidates.front();
    resolved = nullptr;
    std::string resolvedName;

    auto consider = [&](uint32_t address) {
        for (const auto& candidate : candidates) {
            if (candidate.size != size ||
                !stringAtAddressEquals(elf, address, candidate.name)) {
                continue;
            }
            if (!resolvedName.empty() && resolvedName != candidate.name) {
                resolved = nullptr;
                resolvedName = "<ambiguous>";
                return;
            }
            if (resolvedName.empty()) {
                resolved = &candidate;
                resolvedName = candidate.name;
            }
        }
    };

    for (size_t offset = 0; offset + 4 <= size; offset += 4) {
        const uint32_t word = readLe32(bytes + offset);
        const uint32_t op = mips::getOpcode(word);
        const uint32_t rs = mips::getRs(word);
        const uint32_t rt = mips::getRt(word);

        bool handledWrite = false;
        if (op == 0x0Fu) { // LUI
            values[rt] = (word & 0xFFFFu) << 16;
            known[rt] = true;
            handledWrite = true;
        } else if (op == 0x09u && known[rs]) { // ADDIU
            values[rt] = values[rs] + static_cast<uint32_t>(
                static_cast<int32_t>(static_cast<int16_t>(word & 0xFFFFu)));
            known[rt] = true;
            handledWrite = true;
        } else if (op == 0x0Du && known[rs]) { // ORI
            values[rt] = values[rs] | (word & 0xFFFFu);
            known[rt] = true;
            handledWrite = true;
        }

        if (handledWrite) {
            consider(values[rt]);
        } else {
            for (uint32_t reg = 1; reg < 32; ++reg) {
                if (mips::writesRegister(word, reg)) known[reg] = false;
            }
        }
    }

    return resolvedName == "<ambiguous>" ? nullptr : resolved;
}

// Returns the first existing path in the candidate list, or empty string.
std::string findDataFile(const std::string& filename) {
    const char* env = std::getenv("PS1RECOMP_DATA_DIR");
    std::vector<std::string> candidates;
    if (env && *env) {
        candidates.push_back(std::string(env) + "/" + filename);
    }
#ifdef PS1RECOMP_DATA_DIR_DEFAULT
    candidates.push_back(std::string(PS1RECOMP_DATA_DIR_DEFAULT) + "/" + filename);
#endif
    candidates.push_back("ps1Analyzer/data/" + filename);
    candidates.push_back("../ps1Analyzer/data/" + filename);
    candidates.push_back("../../ps1Analyzer/data/" + filename);

    for (const auto& p : candidates) {
        if (fileExists(p)) return p;
    }
    return {};
}

} // namespace

// Constructor

PsyQMatcher::PsyQMatcher() {
    loadDefaults();
}

void PsyQMatcher::loadDefaults() {
    std::string sigs = findDataFile("psyq_signatures.toml");
    std::string meta = findDataFile("psyq_metadata.toml");
    if (sigs.empty() && meta.empty()) {
        fmt::print(stderr,
                   "[PSYQ] warning: no psyq_signatures.toml or psyq_metadata.toml "
                   "found; matcher will be empty (set PS1RECOMP_DATA_DIR or run "
                   "from the project root)\n");
        return;
    }
    loadFromToml(sigs, meta);
}

// Loader: explicit

bool PsyQMatcher::loadFromToml(const std::string& sigsPath,
                               const std::string& metadataPath) {
    bool any = false;
    if (!metadataPath.empty()) any |= loadMetadata(metadataPath);
    if (!sigsPath.empty())     any |= loadSignatures(sigsPath);
    return any;
}

bool PsyQMatcher::loadMetadata(const std::string& path) {
    if (!fileExists(path)) {
        fmt::print(stderr, "[PSYQ] metadata file not found: {}\n", path);
        return false;
    }
    try {
        const auto root = toml::parse(path);
        if (!root.contains("function")) return true;
        const auto& funcs = toml::find<std::vector<toml::value>>(root, "function");
        for (const auto& f : funcs) {
            const auto name = toml::find<std::string>(f, "name");
            const auto sub  = f.contains("subsystem")
                              ? toml::find<std::string>(f, "subsystem") : "";
            const auto stub = f.contains("stub_type")
                              ? toml::find<std::string>(f, "stub_type") : "";
            PsyQFunction entry;
            entry.name      = name;
            entry.subsystem = subsystemFromString(sub);
            entry.stubType  = stubTypeFromString(stub);
            m_database[name] = std::move(entry);
        }
        return true;
    } catch (const std::exception& e) {
        fmt::print(stderr, "[PSYQ] failed to parse metadata {}: {}\n", path, e.what());
        return false;
    }
}

bool PsyQMatcher::loadSignatures(const std::string& path) {
    if (!fileExists(path)) {
        fmt::print(stderr, "[PSYQ] signature file not found: {}\n", path);
        return false;
    }
    try {
        const auto root = toml::parse(path);
        if (!root.contains("signature")) return true;
        const auto& sigs = toml::find<std::vector<toml::value>>(root, "signature");

        // Stage every (hash -> LoadedSig) into temporary multimap-style buckets.
        // The PsyQ .OBJ archive zeroes jal/branch targets pending relocation,
        // so multiple unrelated 8-instruction wrappers (CdSync, CdReadSync,
        // PadStop, GsSetProjection, ...) collapse to identical bytes -- both
        // hash_masked AND hash_full collide across distinct base names. The
        // promote step below drops any hash key claimed by more than one base
        // name, so the live maps only ever return unambiguous identifications.
        std::unordered_map<uint64_t, std::vector<LoadedSig>> stagedMasked;
        std::unordered_map<uint64_t, std::vector<LoadedSig>> stagedFull;

        for (const auto& s : sigs) {
            LoadedSig ls;
            const auto rawName = toml::find<std::string>(s, "name");
            ls.name    = stripVariantSuffix(rawName);
            ls.library = s.contains("library")
                         ? toml::find<std::string>(s, "library") : "";
            ls.size    = static_cast<uint32_t>(toml::find<int64_t>(s, "size"));
            const auto sub  = s.contains("subsystem")
                              ? toml::find<std::string>(s, "subsystem") : "";
            const auto stub = s.contains("stub_type")
                              ? toml::find<std::string>(s, "stub_type") : "";
            ls.subsystem = sub.empty()
                           ? classifySubsystem(ls.name)
                           : subsystemFromString(sub);
            ls.stubType  = stub.empty() ? StubType::Recompile : stubTypeFromString(stub);

            const auto mode = s.contains("match_mode")
                              ? toml::find<std::string>(s, "match_mode") : "masked";
            ls.fullMode = (mode == "full");

            const auto hashMasked = s.contains("hash_masked")
                              ? toml::find<std::string>(s, "hash_masked") : "";
            const auto hashFull   = s.contains("hash_full")
                              ? toml::find<std::string>(s, "hash_full") : "";

            // Stage both hashes regardless of declared match_mode. A masked-
            // mode sig may still be matchable by its hash_full if that one is
            // unique across the DB; conversely, a full-mode sig still owns
            // its masked hash and we want to refuse a competing claim on it.
            if (!hashMasked.empty()) {
                const uint64_t hk = parseHashHex(hashMasked);
                if (hk != 0) stagedMasked[hk].push_back(ls);
            }
            if (!hashFull.empty()) {
                const uint64_t hk = parseHashHex(hashFull);
                if (hk != 0) stagedFull[hk].push_back(ls);
            }

            // Seed the name database so isKnown() / matchByName work for any
            // signature, even without a metadata entry.
            if (m_database.find(ls.name) == m_database.end()) {
                m_database[ls.name] = PsyQFunction{
                    ls.name, ls.subsystem, ls.stubType, ""
                };
            }
        }

        // Promote only hashes claimed by a single base name into the live
        // maps. Same-base-name collisions (CdReadSync, CdReadSync@v2, ...)
        // collapse to one entry without loss; cross-name collisions get
        // dropped entirely because we cannot tell which symbol is right.
        auto promote = [](auto& staging, auto& live, auto& ambiguous) -> size_t {
            size_t dropped = 0;
            for (auto& kv : staging) {
                auto& list = kv.second;
                std::unordered_set<std::string> distinct;
                for (auto& s : list) distinct.insert(s.name);
                if (distinct.size() == 1) {
                    live[kv.first] = list.front();
                    ambiguous.erase(kv.first);
                } else {
                    live.erase(kv.first);
                    ambiguous[kv.first] = list;
                    ++dropped;
                }
            }
            return dropped;
        };

        const size_t droppedMasked = promote(stagedMasked, m_byMasked,
                                             m_ambiguousMasked);
        const size_t droppedFull   = promote(stagedFull, m_byFull,
                                             m_ambiguousFull);

        if (droppedMasked || droppedFull) {
            fmt::print(stderr,
                       "[PSYQ] dropped {} masked + {} full hash collisions "
                       "across distinct PsyQ functions\n",
                       droppedMasked, droppedFull);
        }
        return true;
    } catch (const std::exception& e) {
        fmt::print(stderr, "[PSYQ] failed to parse signatures {}: {}\n", path, e.what());
        return false;
    }
}

// Hash primitives

uint32_t PsyQMatcher::maskImmediates(uint32_t word) {
    const uint32_t op = (word >> 26) & 0x3Fu;

    // SPECIAL (R-type): no immediate field.
    if (op == 0x00u) return word;

    // J / JAL: 26-bit target lives in low bits.
    if (op == 0x02u || op == 0x03u) return word & 0xFC000000u;

    // REGIMM (BLTZ, BGEZ, ...) and direct branches.
    if (op == 0x01u || (op >= 0x04u && op <= 0x07u) ||
        (op >= 0x14u && op <= 0x17u)) {
        return word & 0xFFFF0000u;
    }

    // ALU-imm group (ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI).
    if (op >= 0x08u && op <= 0x0Fu) return word & 0xFFFF0000u;

    // Loads / stores (opcode >= 0x20 -- LB, LW, SW, SWC2, ...).
    if (op >= 0x20u) return word & 0xFFFF0000u;

    // COP0/1/2/3 (0x10..0x13) and anything unrecognised: keep exact.
    return word;
}

uint64_t PsyQMatcher::hashFull(const uint8_t* data, size_t size) {
    return sha256_high64(data, size);
}

uint64_t PsyQMatcher::hashMasked(const uint8_t* data, size_t size) {
    if (size == 0 || (size % 4) != 0) return 0;
    std::vector<uint8_t> masked(size);
    for (size_t off = 0; off < size; off += 4) {
        uint32_t w = static_cast<uint32_t>(data[off])
                   | (static_cast<uint32_t>(data[off + 1]) << 8)
                   | (static_cast<uint32_t>(data[off + 2]) << 16)
                   | (static_cast<uint32_t>(data[off + 3]) << 24);
        uint32_t m = maskImmediates(w);
        masked[off + 0] = static_cast<uint8_t>(m);
        masked[off + 1] = static_cast<uint8_t>(m >> 8);
        masked[off + 2] = static_cast<uint8_t>(m >> 16);
        masked[off + 3] = static_cast<uint8_t>(m >> 24);
    }
    return sha256_high64(masked.data(), masked.size());
}

uint64_t PsyQMatcher::parseHashHex(const std::string& hex) {
    if (hex.size() < 16) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < 16; ++i) {
        const char c = hex[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return v;
}

// Match driver

void PsyQMatcher::matchFunctions(const ElfParser& elf, const FunctionFinder& finder) {
    m_matches.clear();

    // Pass 1: exact name (ELF symbols)
    matchByName(elf);

    // Pass 2: prefix heuristic (e.g., `Cd*` at PsyQ-range addresses)
    matchByPrefix(elf, finder);

    // Pass 3: hash-based detection against the loaded TOML DB
    matchByHash(elf, finder);
}

// Pass 1: Name Matching

void PsyQMatcher::matchByName(const ElfParser& elf) {
    for (const auto& sym : elf.getSymbols()) {
        if (!sym.isFunction() || sym.name.empty()) continue;

        auto it = m_database.find(sym.name);
        if (it != m_database.end()) {
            PsyQMatch m;
            m.address    = sym.address;
            m.name       = sym.name;
            m.subsystem  = it->second.subsystem;
            m.stubType   = it->second.stubType;
            m.exactMatch = true;
            m_matches.push_back(std::move(m));
        }
    }
}

// Pass 2: Prefix Matching

void PsyQMatcher::matchByPrefix(const ElfParser& elf, const FunctionFinder& finder) {
    for (const auto& func : finder.getFunctions()) {
        bool already = false;
        for (const auto& m : m_matches) {
            if (m.address == func.address) { already = true; break; }
        }
        if (already) continue;
        if (!ElfParser::isLikelyPsyQ(func.address)) continue;
        if (func.name.empty() || func.name.substr(0, 5) == "func_") continue;

        PsyQSubsystem sub = classifySubsystem(func.name);
        if (sub != PsyQSubsystem::Other) {
            PsyQMatch m;
            m.address    = func.address;
            m.name       = func.name;
            m.subsystem  = sub;
            m.stubType   = StubType::Stub;
            m.exactMatch = false;
            m_matches.push_back(std::move(m));
        }
    }
}

// Pass 3: Hash-based detection

namespace {
constexpr uint32_t FULL_MATCH_SIZE_LIMIT = 24; // mirrors the Python generator
}

void PsyQMatcher::matchByHash(const ElfParser& elf, const FunctionFinder& finder) {
    if (m_byMasked.empty() && m_byFull.empty()) return;

    for (const auto& func : finder.getFunctions()) {
        bool already = false;
        for (const auto& m : m_matches) {
            if (m.address == func.address) { already = true; break; }
        }
        if (already) continue;

        const Section* sec = elf.findSectionByAddress(func.address);
        if (!sec || sec->type != SectionType::Text || !sec->data) continue;

        const size_t funcOffset = func.address - sec->vaddr;
        if (funcOffset >= sec->size) continue;

        size_t funcSize = func.size > 0 ? func.size : sec->size - funcOffset;
        const size_t avail = sec->size - funcOffset;
        if (funcSize > avail) funcSize = avail;
        if (funcSize < 4 || (funcSize % 4) != 0) continue;

        // Hashes for 2-instruction (8-byte) PsyQ stubs collide pervasively
        // with unrelated game code (e.g. libgs trampolines `j tgt; nop` match
        // every short prologue). Skip them -- they're not reliable fingerprints.
        if (funcSize <= 8) continue;

        const uint8_t* bytes = sec->data + funcOffset;

        // Always try hash_full first -- for tiny BIOS wrappers the masked
        // hash collides, so `full` is the only reliable signal.
        const uint64_t hf = hashFull(bytes, funcSize);
        auto fIt = m_byFull.find(hf);
        const LoadedSig* hit = nullptr;
        bool fullHit = false;
        if (fIt != m_byFull.end() && fIt->second.size == funcSize) {
            hit = &fIt->second;
            fullHit = true;
        }
        if (!hit) {
            auto aIt = m_ambiguousFull.find(hf);
            if (aIt != m_ambiguousFull.end()) {
                hit = resolveByReferencedName(elf, bytes, funcSize, aIt->second);
                fullHit = (hit != nullptr);
            }
        }

        // Otherwise (only if size > 24 -- masked hashes for tinier funcs are
        // not reliable per the Python generator's classification) try masked.
        if (!hit && funcSize > FULL_MATCH_SIZE_LIMIT) {
            const uint64_t hm = hashMasked(bytes, funcSize);
            auto mIt = m_byMasked.find(hm);
            if (mIt != m_byMasked.end() && mIt->second.size == funcSize) {
                hit = &mIt->second;
                fullHit = false;
            }
            if (!hit) {
                auto aIt = m_ambiguousMasked.find(hm);
                if (aIt != m_ambiguousMasked.end()) {
                    hit = resolveByReferencedName(elf, bytes, funcSize,
                                                  aIt->second);
                    fullHit = false;
                }
            }
        }

        if (!hit) continue;

        PsyQMatch m;
        m.address    = func.address;
        m.name       = hit->name;
        m.library    = hit->library;
        m.subsystem  = hit->subsystem;
        m.stubType   = hit->stubType;
        m.exactMatch = true;
        m_matches.push_back(std::move(m));
        (void)fullHit; // currently unused; getLibraryCounts walks matches
    }
}

// Queries

std::vector<const PsyQMatch*> PsyQMatcher::getStubs() const {
    std::vector<const PsyQMatch*> out;
    for (const auto& m : m_matches)
        if (m.stubType == StubType::Stub) out.push_back(&m);
    return out;
}

std::vector<const PsyQMatch*> PsyQMatcher::getSkips() const {
    std::vector<const PsyQMatch*> out;
    for (const auto& m : m_matches)
        if (m.stubType == StubType::Skip) out.push_back(&m);
    return out;
}

std::vector<const PsyQMatch*> PsyQMatcher::getPassthroughs() const {
    std::vector<const PsyQMatch*> out;
    for (const auto& m : m_matches)
        if (m.stubType == StubType::Passthrough) out.push_back(&m);
    return out;
}

bool PsyQMatcher::isKnown(const std::string& name) const {
    return m_database.count(name) > 0;
}

PsyQMatcher::LibraryCounts PsyQMatcher::getLibraryCounts() const {
    LibraryCounts c;
    for (const auto& m : m_matches) {
        if      (m.library == "libgpu") ++c.libgpu;
        else if (m.library == "libetc") ++c.libetc;
        else if (m.library == "libapi") ++c.libapi;
        else if (m.library == "libcd")  ++c.libcd;
        else if (m.library == "libgte") ++c.libgte;
        else if (m.library == "libgs")  ++c.libgs;
        else                            ++c.other;
    }
    return c;
}

// Subsystem Classification

PsyQSubsystem PsyQMatcher::classifySubsystem(const std::string& name) {
    if (name.empty()) return PsyQSubsystem::Other;

    if (name == "VSync" || name == "VSyncCallback" ||
        name == "DrawSync" || name == "ResetCallback" ||
        name == "StopCallback" || name == "RestartCallback" ||
        name == "ChangeClearPAD") {
        return PsyQSubsystem::VSync;
    }

    if (name == "malloc" || name == "free" || name == "calloc" ||
        name == "realloc" || name == "malloc3" || name == "InitHeap") {
        return PsyQSubsystem::Memory;
    }

    if (name == "printf" || name == "sprintf" ||
        name.substr(0, 3) == "mem" || name.substr(0, 3) == "str" ||
        name == "rand" || name == "srand" || name == "abs" ||
        name == "labs" || name == "atoi" || name == "atol") {
        return PsyQSubsystem::LibC;
    }

    if (name.substr(0, 4) == "rsin" || name.substr(0, 4) == "rcos" ||
        name.substr(0, 5) == "ratan" || name == "SquareRoot0" ||
        name == "SquareRoot12") {
        return PsyQSubsystem::Math;
    }

    if (name.substr(0, 3) == "Spu" || name.substr(0, 4) == "SPU_" ||
        name.substr(0, 2) == "Ss" || name.substr(0, 4) == "SND_") {
        return PsyQSubsystem::Sound;
    }

    if (name.substr(0, 2) == "Cd") return PsyQSubsystem::CDROM;

    if (name.substr(0, 3) == "Pad" || name.substr(0, 3) == "Gun")
        return PsyQSubsystem::Controller;

    if (name.substr(0, 2) == "Gs" || name.substr(0, 4) == "GPU_" ||
        name.substr(0, 3) == "Fnt" || name.substr(0, 3) == "Put" ||
        name == "ResetGraph" ||
        name == "DrawPrim" || name == "DrawOTag" || name == "DrawOTagEnv" ||
        name == "ClearOTag" || name == "ClearOTagR" ||
        name == "LoadImage" || name == "StoreImage" || name == "MoveImage" ||
        name == "LoadTPage" || name == "LoadClut" ||
        name == "GetTPage" || name == "GetClut" ||
        name == "SetDispMask" || name == "SetDefDrawEnv" || name == "SetDefDispEnv") {
        return PsyQSubsystem::Graphics;
    }

    if (name.substr(0, 4) == "gte_" || name.substr(0, 3) == "Rot" ||
        name.substr(0, 5) == "Trans" || name.substr(0, 3) == "Mul" ||
        name.substr(0, 4) == "Comp" || name.substr(0, 5) == "Apply" ||
        name == "InitGeom" || name == "NormalClip" ||
        name.substr(0, 7) == "Average" || name.substr(0, 5) == "Outer" ||
        name == "SetGeomOffset" || name == "SetGeomScreen" ||
        name == "SetRotMatrix" || name == "SetTransMatrix" ||
        name == "SetLightMatrix" || name == "SetColorMatrix" ||
        name == "SetBackColor" || name == "SetFarColor") {
        return PsyQSubsystem::GTE;
    }

    if (name.find("Sync") != std::string::npos ||
        name.find("Callback") != std::string::npos) {
        return PsyQSubsystem::VSync;
    }

    return PsyQSubsystem::Other;
}

// String Helpers

const char* PsyQMatcher::subsystemName(PsyQSubsystem sub) {
    switch (sub) {
        case PsyQSubsystem::Graphics:   return "Graphics";
        case PsyQSubsystem::Sound:      return "Sound";
        case PsyQSubsystem::CDROM:      return "CD-ROM";
        case PsyQSubsystem::Controller: return "Controller";
        case PsyQSubsystem::GTE:        return "GTE";
        case PsyQSubsystem::VSync:      return "VSync";
        case PsyQSubsystem::Memory:     return "Memory";
        case PsyQSubsystem::LibC:       return "LibC";
        case PsyQSubsystem::Math:       return "Math";
        case PsyQSubsystem::Other:      return "Other";
    }
    return "Unknown";
}

const char* PsyQMatcher::stubTypeName(StubType type) {
    switch (type) {
        case StubType::Stub:        return "stub";
        case StubType::Skip:        return "skip";
        case StubType::Passthrough: return "passthrough";
        case StubType::Recompile:   return "recompile";
    }
    return "unknown";
}

} // namespace ps1recomp
