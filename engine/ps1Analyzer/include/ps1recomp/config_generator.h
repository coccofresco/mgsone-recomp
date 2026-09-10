#pragma once

// ps1Analyzer -- Config Generator
// Generates TOML config files for ps1Recomp from analysis results

#include <cstdint>
#include <string>
#include <vector>

namespace ps1recomp {

// Forward declarations
class ElfParser;
class FunctionFinder;
class PsyQMatcher;

// Config Generator

class ConfigGenerator {
public:
    /// Generate TOML config and write to file.
    /// Returns true on success, false on error (call getError()).
    bool generate(const ElfParser& elf,
                  const FunctionFinder& finder,
                  const PsyQMatcher& matcher,
                  const std::string& elfPath,
                  const std::string& outputPath);

    /// Generate TOML config as a string (for testing).
    std::string generateString(const ElfParser& elf,
                               const FunctionFinder& finder,
                               const PsyQMatcher& matcher,
                               const std::string& elfPath);

    /// Get last error message.
    const std::string& getError() const { return m_error; }

private:
    std::string m_error;
};

} // namespace ps1recomp
