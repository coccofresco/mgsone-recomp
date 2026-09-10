// ps1Analyzer -- Config Generator Implementation
// Generates TOML config file from analyzer results for ps1Recomp

#include "ps1recomp/config_generator.h"
#include "ps1recomp/elf_parser.h"
#include "ps1recomp/function_finder.h"
#include "ps1recomp/psyq_signatures.h"

#include <fstream>
#include <toml.hpp>
#include <fmt/format.h>

namespace ps1recomp {

// String Helpers

static std::string hexAddr(uint32_t addr) {
    return fmt::format("0x{:08X}", addr);
}

// Build TOML Tree

static toml::value buildConfig(const ElfParser& elf,
                               const FunctionFinder& finder,
                               const PsyQMatcher& matcher,
                               const std::string& elfPath) {
    toml::value root = toml::table{};

    // [binary] section
    {
        toml::table binary;
        binary["path"] = elfPath;
        binary["entry_point"] = hexAddr(elf.getEntryPoint());

        const auto* text = elf.getTextSection();
        if (text) {
            binary["text_start"] = hexAddr(text->vaddr);
            binary["text_size"]  = static_cast<int64_t>(text->size);
        }

        binary["total_code_size"] = static_cast<int64_t>(elf.getTotalCodeSize());
        binary["total_data_size"] = static_cast<int64_t>(elf.getTotalDataSize());

        root["binary"] = std::move(binary);
    }

    // [stats] section
    {
        std::size_t hleFunctionCount = 0;
        for (const auto& match : matcher.getMatches()) {
            if (match.stubType != StubType::Recompile) {
                ++hleFunctionCount;
            }
        }

        toml::table stats;
        stats["total_functions"] = static_cast<int64_t>(finder.getFunctionCount());
        stats["psyq_functions"]  = static_cast<int64_t>(hleFunctionCount);
        stats["game_functions"]  = static_cast<int64_t>(
            finder.getFunctionCount() > hleFunctionCount
                ? finder.getFunctionCount() - hleFunctionCount
                : 0);
        stats["stubs"]           = static_cast<int64_t>(matcher.getStubs().size());
        stats["skips"]           = static_cast<int64_t>(matcher.getSkips().size());
        stats["passthroughs"]    = static_cast<int64_t>(matcher.getPassthroughs().size());

        root["stats"] = std::move(stats);
    }

    // [[functions]] array of tables
    {
        toml::array funcs;
        for (const auto& fi : finder.getFunctions()) {
            // Skip PsyQ functions handled by the runtime. A hash match with
            // StubType::Recompile is identification-only and must retain its
            // original MIPS body.
            bool isPsyQ = false;
            for (const auto& m : matcher.getMatches()) {
                if (m.address == fi.address) {
                    isPsyQ = (m.stubType != StubType::Recompile);
                    break;
                }
            }
            if (isPsyQ) continue;

            toml::table f;
            f["name"]    = fi.name.empty()
                ? fmt::format("func_{:08X}", fi.address)
                : fi.name;
            f["address"] = hexAddr(fi.address);
            f["size"]    = static_cast<int64_t>(fi.size);

            // Source string
            switch (fi.source) {
                case FunctionSource::Symbol:     f["source"] = "symbol"; break;
                case FunctionSource::EntryPoint: f["source"] = "entry_point"; break;
                case FunctionSource::JALTarget:  f["source"] = "jal_target"; break;
                case FunctionSource::Prologue:   f["source"] = "prologue"; break;
                case FunctionSource::DataPointer:f["source"] = "data_pointer"; break;
                case FunctionSource::ComputedPointer:
                    f["source"] = "computed_pointer"; break;
                default:                         f["source"] = "heuristic"; break;
            }

            f["leaf"] = fi.isLeaf;

            funcs.push_back(std::move(f));
        }
        root["functions"] = std::move(funcs);
    }

    // [[stubs]] array -- PsyQ functions needing runtime stubs
    {
        toml::array stubs;
        for (const auto* m : matcher.getStubs()) {
            toml::table s;
            s["name"]      = m->name;
            s["address"]   = hexAddr(m->address);
            s["subsystem"] = PsyQMatcher::subsystemName(m->subsystem);
            if (!m->library.empty()) s["library"] = m->library;
            stubs.push_back(std::move(s));
        }
        root["stubs"] = std::move(stubs);
    }

    // [[skips]] array -- debug/profiling functions to skip
    {
        toml::array skips;
        for (const auto* m : matcher.getSkips()) {
            toml::table s;
            s["name"]    = m->name;
            s["address"] = hexAddr(m->address);
            if (!m->library.empty()) s["library"] = m->library;
            skips.push_back(std::move(s));
        }
        root["skips"] = std::move(skips);
    }

    // [[passthroughs]] array -- host-equivalent functions
    {
        toml::array pass;
        for (const auto* m : matcher.getPassthroughs()) {
            toml::table p;
            p["name"]    = m->name;
            p["address"] = hexAddr(m->address);
            if (!m->library.empty()) p["library"] = m->library;
            pass.push_back(std::move(p));
        }
        root["passthroughs"] = std::move(pass);
    }

    // [[hle_functions]] -- forward-compat schema for ps1Recomp Sessao 0.5
    // Every match shows up here with the canonical
    // `<library>_<basename>` identifier the recompiler will use to look up
    // the C++ HLE stub. Recompile matches remain visible for diagnostics but
    // have hle=false so ps1Recomp emits their original MIPS implementation.
    // Only emitted when the `library` field is filled (i.e., the match came
    // from the hash-based pass).
    {
        toml::array hle;
        for (const auto& m : matcher.getMatches()) {
            if (m.library.empty()) continue;
            toml::table f;
            f["address"]   = hexAddr(m.address);
            f["hle"]       = (m.stubType != StubType::Recompile);
            f["name"]      = fmt::format("{}_{}", m.library, m.name);
            f["library"]   = m.library;
            f["subsystem"] = PsyQMatcher::subsystemName(m.subsystem);
            f["stub_type"] = PsyQMatcher::stubTypeName(m.stubType);
            hle.push_back(std::move(f));
        }
        root["hle_functions"] = std::move(hle);
    }

    return root;
}

// Public API

std::string ConfigGenerator::generateString(const ElfParser& elf,
                                            const FunctionFinder& finder,
                                            const PsyQMatcher& matcher,
                                            const std::string& elfPath) {
    auto config = buildConfig(elf, finder, matcher, elfPath);
    return toml::format(config);
}

bool ConfigGenerator::generate(const ElfParser& elf,
                               const FunctionFinder& finder,
                               const PsyQMatcher& matcher,
                               const std::string& elfPath,
                               const std::string& outputPath) {
    m_error.clear();

    try {
        auto content = generateString(elf, finder, matcher, elfPath);

        std::ofstream file(outputPath);
        if (!file.is_open()) {
            m_error = fmt::format("Could not open output file: {}", outputPath);
            return false;
        }

        file << content;
        if (!file.good()) {
            m_error = fmt::format("Error writing to: {}", outputPath);
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        m_error = fmt::format("Config generation error: {}", e.what());
        return false;
    }
}

} // namespace ps1recomp
