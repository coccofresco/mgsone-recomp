#pragma once

// ps1Analyzer -- ELF Parser
// Loads PS1 ELF/PS-X EXE binaries and extracts sections, symbols, and raw data

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ps1recomp {

// PS1 Memory Map Constants

constexpr uint32_t PS1_RAM_SIZE = 2 * 1024 * 1024;  // 2MB
constexpr uint32_t PS1_RAM_MASK = PS1_RAM_SIZE - 1; // 0x1FFFFF

constexpr uint32_t PS1_KUSEG_START = 0x00000000;
constexpr uint32_t PS1_KUSEG_END = 0x001FFFFF;
constexpr uint32_t PS1_KSEG0_START = 0x80000000;
constexpr uint32_t PS1_KSEG0_END = 0x801FFFFF;
constexpr uint32_t PS1_KSEG1_START = 0xA0000000;
constexpr uint32_t PS1_KSEG1_END = 0xA01FFFFF;

constexpr uint32_t PS1_SCRATCHPAD_START = 0x1F800000;
constexpr uint32_t PS1_SCRATCHPAD_SIZE = 1024;

constexpr uint32_t PS1_IO_BASE = 0x1F801000;
constexpr uint32_t PS1_BIOS_START = 0xBFC00000;

// Section Types

enum class SectionType {
  Text, // Executable code (.text)
  Data, // Initialized data (.data, .rodata, .sdata)
  Bss,  // Uninitialized data (.bss, .sbss)
  Other // Unknown or unclassified
};

// Symbol Types

enum class SymbolType {
  Function, // STT_FUNC
  Object,   // STT_OBJECT (data)
  NoType,   // STT_NOTYPE
  Other
};

// Section

struct Section {
  std::string name;
  uint32_t vaddr; // Virtual address in PS1 memory
  uint32_t size;  // Size in bytes
  SectionType type;
  const uint8_t *data; // Pointer to raw data (nullptr for BSS)
  uint32_t alignment;  // Section alignment

  bool containsAddress(uint32_t addr) const {
    return addr >= vaddr && addr < vaddr + size;
  }
};

// Symbol

struct Symbol {
  std::string name;
  uint32_t address;
  uint32_t size;
  SymbolType type;
  uint16_t sectionIndex; // Index of containing section

  bool isFunction() const { return type == SymbolType::Function; }
};

// ElfParser

class ElfParser {
public:
  ElfParser() = default;
  ~ElfParser() = default;

  // Non-copyable (owns ELFIO reader)
  ElfParser(const ElfParser &) = delete;
  ElfParser &operator=(const ElfParser &) = delete;
  ElfParser(ElfParser &&) = default;
  ElfParser &operator=(ElfParser &&) = default;

  /// Load and parse a PS1 ELF file.
  /// Returns true on success.
  bool load(const std::string &path);

  /// Check if a file has been loaded successfully.
  bool isLoaded() const { return m_loaded; }

  /// Get the ELF entry point address.
  uint32_t getEntryPoint() const { return m_entryPoint; }

  /// Get all parsed sections.
  const std::vector<Section> &getSections() const { return m_sections; }

  /// Get all parsed symbols.
  const std::vector<Symbol> &getSymbols() const { return m_symbols; }

  /// Find the .text section (primary code section).
  const Section *getTextSection() const;

  /// Find a section by name.
  const Section *findSection(const std::string &name) const;

  /// Find a section containing the given address.
  const Section *findSectionByAddress(uint32_t addr) const;

  /// Get all function symbols.
  std::vector<const Symbol *> getFunctionSymbols() const;

  /// Get the total size of loaded code (all text sections).
  uint32_t getTotalCodeSize() const;

  /// Get the total size of loaded data (data + bss sections).
  uint32_t getTotalDataSize() const;

  /// Get load error message (empty if no error).
  const std::string &getError() const { return m_error; }

  // Static Helpers

  /// Convert a virtual address to a physical RAM offset.
  /// Returns std::nullopt if address is not in RAM.
  static std::optional<uint32_t> vAddrToPhysical(uint32_t vaddr);

  /// Check if a virtual address is in PS1 RAM (any segment).
  static bool isInRAM(uint32_t addr);

  /// Check if a virtual address is likely PsyQ SDK code.
  static bool isLikelyPsyQ(uint32_t addr);

private:
  bool m_loaded = false;
  uint32_t m_entryPoint = 0;
  std::string m_error;

  std::vector<Section> m_sections;
  std::vector<Symbol> m_symbols;

  // Raw file data kept alive for section data pointers
  std::vector<uint8_t> m_elfData;

  bool loadPsExe();
  bool loadElf(const std::string &path);
  bool validatePS1Elf(uint16_t machine, uint8_t elfClass, uint8_t encoding);
  SectionType classifySectionType(const std::string &name, uint32_t type,
                                  uint32_t flags);
};

} // namespace ps1recomp
