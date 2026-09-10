// ps1Analyzer -- ELF Parser Implementation
// Loads PS1 ELF binaries using ELFIO and extracts sections/symbols

#include "ps1recomp/elf_parser.h"

#include <algorithm>
#include <elfio/elfio.hpp>
#include <fmt/format.h>
#include <fstream>

namespace ps1recomp {

// Load

bool ElfParser::load(const std::string &path) {
  m_loaded = false;
  m_error.clear();
  m_sections.clear();
  m_symbols.clear();

  // Read entire file into memory (keeps data pointers valid)
  {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
      m_error = fmt::format("Could not open file: {}", path);
      return false;
    }
    auto size = file.tellg();
    if (size <= 0) {
      m_error = fmt::format("File is empty or unreadable: {}", path);
      return false;
    }
    m_elfData.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(m_elfData.data()), size);
  }

  // Check for PS-X EXE magic before trying ELFIO
  if (m_elfData.size() >= 0x800 &&
      std::memcmp(m_elfData.data(), "PS-X EXE", 8) == 0) {
    return loadPsExe();
  }

  return loadElf(path);
}

// PS-EXE Loader

bool ElfParser::loadPsExe() {
  // PS-EXE header layout (2048 bytes):
  //   0x00: magic "PS-X EXE"
  //   0x10: initial PC
  //   0x14: initial GP
  //   0x18: destination address (where to load in RAM)
  //   0x1C: file size (payload size in bytes)
  //   0x30: initial SP base
  //   0x34: initial SP offset
  //   0x800: payload start
  auto read32 = [this](size_t offset) -> uint32_t {
    return static_cast<uint32_t>(m_elfData[offset]) |
           (static_cast<uint32_t>(m_elfData[offset + 1]) << 8) |
           (static_cast<uint32_t>(m_elfData[offset + 2]) << 16) |
           (static_cast<uint32_t>(m_elfData[offset + 3]) << 24);
  };

  m_entryPoint = read32(0x10);
  uint32_t destAddr = read32(0x18);
  uint32_t fileSize = read32(0x1C);

  // Validate
  if (!isInRAM(m_entryPoint)) {
    m_error = fmt::format("PS-EXE entry point 0x{:08X} is not in PS1 RAM range",
                          m_entryPoint);
    return false;
  }

  // Clamp fileSize to actual available data
  size_t availablePayload = m_elfData.size() - 0x800;
  if (fileSize == 0 || fileSize > availablePayload) {
    fileSize = static_cast<uint32_t>(availablePayload);
  }

  // Create a single .text section from the payload
  Section textSec;
  textSec.name = ".text";
  textSec.vaddr = destAddr;
  textSec.size = fileSize;
  textSec.type = SectionType::Text;
  textSec.data = m_elfData.data() + 0x800;
  textSec.alignment = 4;
  m_sections.push_back(std::move(textSec));

  // No symbols in PS-EXE format
  m_loaded = true;
  return true;
}

// ELF Loader

bool ElfParser::loadElf(const std::string &path) {
  // Parse with ELFIO
  ELFIO::elfio reader;
  if (!reader.load(path)) {
    m_error = fmt::format("ELFIO failed to parse: {}", path);
    return false;
  }

  // Validate PS1 constraints
  if (!validatePS1Elf(reader.get_machine(), reader.get_class(),
                      reader.get_encoding())) {
    return false;
  }

  m_entryPoint = static_cast<uint32_t>(reader.get_entry());

  // Validate entry point is in PS1 RAM
  if (!isInRAM(m_entryPoint)) {
    m_error = fmt::format("Entry point 0x{:08X} is not in PS1 RAM range",
                          m_entryPoint);
    return false;
  }

  // Extract sections
  for (const auto &section : reader.sections) {
    if (section->get_size() == 0 && section->get_name().empty()) {
      continue; // Skip null section
    }

    Section sec;
    sec.name = section->get_name();
    sec.vaddr = static_cast<uint32_t>(section->get_address());
    sec.size = static_cast<uint32_t>(section->get_size());
    sec.alignment = static_cast<uint32_t>(section->get_addr_align());
    sec.type = classifySectionType(sec.name,
                                   static_cast<uint32_t>(section->get_type()),
                                   static_cast<uint32_t>(section->get_flags()));

    // Set data pointer into our owned m_elfData buffer
    if (section->get_type() != ELFIO::SHT_NOBITS &&
        section->get_data() != nullptr &&
        section->get_offset() + sec.size <= m_elfData.size()) {
      sec.data = m_elfData.data() + section->get_offset();
    } else {
      sec.data = nullptr;
    }

    m_sections.push_back(std::move(sec));
  }

  // Extract symbols
  for (const auto &section : reader.sections) {
    if (section->get_type() != ELFIO::SHT_SYMTAB &&
        section->get_type() != ELFIO::SHT_DYNSYM) {
      continue;
    }

    ELFIO::symbol_section_accessor symbols(reader, section.get());
    for (ELFIO::Elf_Xword i = 0; i < symbols.get_symbols_num(); ++i) {
      std::string name;
      ELFIO::Elf64_Addr value;
      ELFIO::Elf_Xword size;
      unsigned char bind, type, other;
      ELFIO::Elf_Half section_index;

      if (!symbols.get_symbol(i, name, value, size, bind, type, section_index,
                              other)) {
        continue;
      }

      if (name.empty()) {
        continue;
      }

      Symbol sym;
      sym.name = std::move(name);
      sym.address = static_cast<uint32_t>(value);
      sym.size = static_cast<uint32_t>(size);
      sym.sectionIndex = section_index;

      switch (type) {
      case ELFIO::STT_FUNC:
        sym.type = SymbolType::Function;
        break;
      case ELFIO::STT_OBJECT:
        sym.type = SymbolType::Object;
        break;
      case ELFIO::STT_NOTYPE:
        sym.type = SymbolType::NoType;
        break;
      default:
        sym.type = SymbolType::Other;
        break;
      }

      m_symbols.push_back(std::move(sym));
    }
  }

  m_loaded = true;
  return true;
}

// Queries

const Section *ElfParser::getTextSection() const {
  return findSection(".text");
}

const Section *ElfParser::findSection(const std::string &name) const {
  for (const auto &sec : m_sections) {
    if (sec.name == name) {
      return &sec;
    }
  }
  return nullptr;
}

const Section *ElfParser::findSectionByAddress(uint32_t addr) const {
  for (const auto &sec : m_sections) {
    if (sec.containsAddress(addr)) {
      return &sec;
    }
  }
  return nullptr;
}

std::vector<const Symbol *> ElfParser::getFunctionSymbols() const {
  std::vector<const Symbol *> funcs;
  for (const auto &sym : m_symbols) {
    if (sym.isFunction()) {
      funcs.push_back(&sym);
    }
  }
  return funcs;
}

uint32_t ElfParser::getTotalCodeSize() const {
  uint32_t total = 0;
  for (const auto &sec : m_sections) {
    if (sec.type == SectionType::Text) {
      total += sec.size;
    }
  }
  return total;
}

uint32_t ElfParser::getTotalDataSize() const {
  uint32_t total = 0;
  for (const auto &sec : m_sections) {
    if (sec.type == SectionType::Data || sec.type == SectionType::Bss) {
      total += sec.size;
    }
  }
  return total;
}

// Static Helpers

std::optional<uint32_t> ElfParser::vAddrToPhysical(uint32_t vaddr) {
  // KUSEG: 0x00000000 - 0x001FFFFF
  if (vaddr <= PS1_KUSEG_END) {
    return vaddr;
  }
  // KSEG0: 0x80000000 - 0x801FFFFF (cached)
  if (vaddr >= PS1_KSEG0_START && vaddr <= PS1_KSEG0_END) {
    return vaddr - PS1_KSEG0_START;
  }
  // KSEG1: 0xA0000000 - 0xA01FFFFF (uncached)
  if (vaddr >= PS1_KSEG1_START && vaddr <= PS1_KSEG1_END) {
    return vaddr - PS1_KSEG1_START;
  }
  return std::nullopt;
}

bool ElfParser::isInRAM(uint32_t addr) {
  return vAddrToPhysical(addr).has_value();
}

bool ElfParser::isLikelyPsyQ(uint32_t addr) {
  // PsyQ SDK functions are typically linked at higher addresses
  // Game logic is usually in lower KSEG0 range
  return addr >= 0x800A0000;
}

// Private

bool ElfParser::validatePS1Elf(uint16_t machine, uint8_t elfClass,
                               uint8_t encoding) {
  if (machine != ELFIO::EM_MIPS) {
    m_error = fmt::format("Not a MIPS ELF (machine type: {})", machine);
    return false;
  }
  if (elfClass != ELFIO::ELFCLASS32) {
    m_error = "Not a 32-bit ELF";
    return false;
  }
  if (encoding != ELFIO::ELFDATA2LSB) {
    m_error = "Not a little-endian ELF (PS1 is MIPS LE)";
    return false;
  }
  return true;
}

SectionType ElfParser::classifySectionType(const std::string &name,
                                           uint32_t type, uint32_t flags) {
  // BSS sections (no data in file)
  if (type == ELFIO::SHT_NOBITS) {
    return SectionType::Bss;
  }

  // Executable sections
  if (flags & ELFIO::SHF_EXECINSTR) {
    return SectionType::Text;
  }

  // Named section classification
  if (name == ".text" || name == ".init" || name == ".fini") {
    return SectionType::Text;
  }
  if (name == ".bss" || name == ".sbss") {
    return SectionType::Bss;
  }
  if (name == ".data" || name == ".rodata" || name == ".sdata" ||
      name == ".rdata" || name == ".lit4" || name == ".lit8") {
    return SectionType::Data;
  }

  // Fallback: writable -> data, otherwise other
  if (flags & ELFIO::SHF_ALLOC) {
    return (flags & ELFIO::SHF_WRITE) ? SectionType::Data : SectionType::Other;
  }

  return SectionType::Other;
}

} // namespace ps1recomp
