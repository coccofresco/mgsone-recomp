#pragma once

// ps1Analyzer -- PsyQ Signature Matcher
// Identifies PsyQ SDK functions by SHA-256 hash of their MIPS opcodes,
// loaded from ps1Analyzer/data/psyq_signatures.toml.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps1recomp {

// Forward declarations
class ElfParser;
class FunctionFinder;
struct FunctionInfo;

// PsyQ Subsystems

enum class PsyQSubsystem {
    Graphics,    // Gs*, GPU_*, Draw*, Set*, Clear*
    Sound,       // Sp*, SPU_*, Spu*
    CDROM,       // Cd*, CdInit, CdRead
    Controller,  // Pad*, Gun*
    GTE,         // gte_*, Rot*, Trans*
    VSync,       // VSync, DrawSync, *Sync
    Memory,      // malloc, free, InitHeap
    LibC,        // printf, memcpy, strlen, etc.
    Math,        // rsin, rcos, ratan, SquareRoot0
    Other        // Unclassified PsyQ functions
};

// Stub Types

enum class StubType {
    Stub,         // Needs runtime implementation (GPU, SPU, etc.)
    Skip,         // Can be safely skipped (debug prints, profiling)
    Passthrough,  // Use host equivalent (memcpy, malloc, etc.)
    Recompile     // Should be recompiled normally (not a known PsyQ func)
};

// PsyQ Function Definition

struct PsyQFunction {
    std::string    name;
    PsyQSubsystem  subsystem;
    StubType       stubType;
    std::string    description;
};

// PsyQ Match Result

struct PsyQMatch {
    uint32_t       address;     // Function address in binary
    std::string    name;        // Base function name (no @vN suffix)
    std::string    library;     // libgpu / libcd / libgte / libapi / libetc
    PsyQSubsystem  subsystem;
    StubType       stubType;
    bool           exactMatch;  // true = symbol/hash, false = prefix heuristic
};

// PsyQ Matcher

class PsyQMatcher {
public:
    PsyQMatcher();

    /// Load a signature DB and metadata file.
    /// Empty `sigsPath` skips signature loading; empty `metadataPath` skips
    /// the subsystem/stub_type join. Returns true if at least one of the two
    /// was loaded successfully.
    bool loadFromToml(const std::string& sigsPath,
                      const std::string& metadataPath);

    /// Run matching against parsed ELF symbols and detected functions.
    void matchFunctions(const ElfParser& elf, const FunctionFinder& finder);

    const std::vector<PsyQMatch>& getMatches() const { return m_matches; }
    std::vector<const PsyQMatch*> getStubs() const;
    std::vector<const PsyQMatch*> getSkips() const;
    std::vector<const PsyQMatch*> getPassthroughs() const;

    size_t getMatchCount() const { return m_matches.size(); }
    size_t getDatabaseSize() const { return m_database.size(); }
    size_t getSignatureCount() const { return m_byMasked.size() + m_byFull.size(); }

    bool isKnown(const std::string& name) const;

    /// Number of detected matches per library (for diagnostics).
    struct LibraryCounts {
        size_t libgpu = 0;
        size_t libetc = 0;
        size_t libapi = 0;
        size_t libcd  = 0;
        size_t libgte = 0;
        size_t libgs  = 0;
        size_t other  = 0;
        size_t fullMode = 0;
        size_t maskedMode = 0;
    };
    LibraryCounts getLibraryCounts() const;

    /// Classify subsystem from function name (for prefix-fallback only).
    static PsyQSubsystem classifySubsystem(const std::string& name);

    static const char* subsystemName(PsyQSubsystem sub);
    static const char* stubTypeName(StubType type);

    // Hash primitives (public for testing)

    /// Zero immediate fields in a 32-bit MIPS instruction. Mirrors the
    /// Python reference implementation in tools/extract_psyq_signatures.py.
    /// COP0/1/2/3 instructions are kept exact (GTE encoding is critical).
    static uint32_t maskImmediates(uint32_t word);

    /// SHA-256 of `data[0..size)`, truncated to the first 16 hex chars
    /// (the high 64 bits of the digest), returned as a uint64_t.
    static uint64_t hashFull(const uint8_t* data, size_t size);

    /// Like `hashFull`, but each 32-bit word is passed through
    /// `maskImmediates` before hashing. `size` must be a multiple of 4.
    static uint64_t hashMasked(const uint8_t* data, size_t size);

    /// Parse the first 16 hex chars of a SHA-256 string into a uint64_t.
    /// Returns 0 on malformed input.
    static uint64_t parseHashHex(const std::string& hex);

private:
    // Loaded signature row. `subsystem`/`stubType` are joined from the
    // metadata file when available; otherwise filled with Other/Recompile.
    struct LoadedSig {
        std::string    name;     // Base name, no @vN
        std::string    library;  // libgpu / libcd / ...
        uint32_t       size;
        PsyQSubsystem  subsystem;
        StubType       stubType;
        bool           fullMode; // true = matched via hash_full
    };

    std::vector<PsyQMatch> m_matches;
    std::unordered_map<std::string, PsyQFunction> m_database;

    // Hash -> signature. Keys are the high 64 bits of SHA-256.
    std::unordered_map<uint64_t, LoadedSig> m_byMasked;
    std::unordered_map<uint64_t, LoadedSig> m_byFull;

    // Cross-name collisions are normally rejected. Keep their candidates so
    // a linked wrapper that explicitly loads its own diagnostic name string
    // (for example LoadImage/StoreImage before checkRECT) can still be
    // resolved without guessing.
    std::unordered_map<uint64_t, std::vector<LoadedSig>> m_ambiguousMasked;
    std::unordered_map<uint64_t, std::vector<LoadedSig>> m_ambiguousFull;

    // Loader steps
    void loadDefaults();
    bool loadMetadata(const std::string& path);
    bool loadSignatures(const std::string& path);

    // Match passes
    void matchByName(const ElfParser& elf);
    void matchByPrefix(const ElfParser& elf, const FunctionFinder& finder);
    void matchByHash(const ElfParser& elf, const FunctionFinder& finder);
};

} // namespace ps1recomp
