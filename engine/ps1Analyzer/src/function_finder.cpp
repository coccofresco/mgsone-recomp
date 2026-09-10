// ps1Analyzer -- Function Finder Implementation
// Multi-pass function detection for PS1 MIPS binaries

#include "ps1recomp/function_finder.h"

#include <algorithm>
#include <fmt/format.h>

namespace ps1recomp {

namespace mips {

bool writesRegister(uint32_t instr, uint32_t reg) {
  if (reg == 0)
    return false; // $zero is never really written

  const uint32_t op = getOpcode(instr);

  if (op == OP_SPECIAL) {
    const uint32_t fn = getFunction(instr);
    // jr, jalr, mthi, mtlo, mult, multu, div, divu write no GPR rd we track
    const bool noRdWrite = (fn == 0x08 || fn == 0x09 || fn == 0x11 ||
                            fn == 0x13 || fn == 0x18 || fn == 0x19 ||
                            fn == 0x1A || fn == 0x1B);
    return !noRdWrite && getRd(instr) == reg;
  }
  if (isLoad(instr))
    return getRt(instr) == reg;
  // ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI -- all write rt
  if (op >= 0x08 && op <= 0x0F)
    return getRt(instr) == reg;
  return false;
}

} // namespace mips

namespace {

/// Does the instruction at `idx` terminate the function?
///
/// `jr $ra` always does. For `jr $reg` we walk back to whoever wrote `$reg`:
/// a load means the target came out of memory -- a jump table or dispatch
/// through a pointer, both of which stay inside the function -- while anything
/// else (notably `move $reg, $ra`) means the register carries a return address.
bool isFunctionEnd(const std::vector<uint32_t> &words, size_t idx) {
  const uint32_t instr = words[idx];
  if (!mips::isJR(instr))
    return false;
  const uint32_t reg = mips::getRs(instr);
  if (reg == mips::REG_RA)
    return true;

  for (size_t k = idx; k-- > 0;) {
    if (!mips::writesRegister(words[k], reg))
      continue;
    return !mips::isLoad(words[k]);
  }
  // Nothing in this range wrote it, so it came in from the caller.
  return true;
}

} // namespace

uint32_t refineFunctionEnd(const std::vector<uint32_t> &words,
                           uint32_t startAddr, uint32_t maxEndAddr) {
  if (words.empty() || maxEndAddr <= startAddr)
    return maxEndAddr;

  uint32_t reach = startAddr;

  for (size_t i = 0; i < words.size(); ++i) {
    const uint32_t addr = startAddr + static_cast<uint32_t>(i * 4);
    if (addr >= maxEndAddr)
      break;
    const uint32_t instr = words[i];

    // Track the furthest forward target: while any branch still jumps past
    // this point, the body continues regardless of terminators in between.
    if (mips::isBranch(instr) || mips::isJ(instr)) {
      const uint32_t target = mips::isJ(instr)
                                  ? mips::jalTarget(addr, instr)
                                  : mips::branchTarget(addr, instr);
      if (target > reach && target > startAddr && target < maxEndAddr)
        reach = target;
    }

    if (isFunctionEnd(words, i) && addr >= reach) {
      const uint32_t end = addr + 8; // terminator + its delay slot
      return end > maxEndAddr ? maxEndAddr : end;
    }
  }

  return maxEndAddr;
}

void clampOverlappingSizes(std::vector<FunctionInfo> &funcs) {
  if (funcs.size() < 2)
    return;
  if (!std::is_sorted(funcs.begin(), funcs.end()))
    std::sort(funcs.begin(), funcs.end());

  for (size_t i = 0; i + 1 < funcs.size(); ++i) {
    if (funcs[i + 1].address <= funcs[i].address)
      continue; // duplicate address -- not this function's problem
    const uint32_t room = funcs[i + 1].address - funcs[i].address;
    if (funcs[i].size > room)
      funcs[i].size = room;
  }
}

// Main Entry Point

void FunctionFinder::findFunctions(const ElfParser& elf) {
    m_functions.clear();
    m_jalTargets.clear();

    // Pass 1: Entry point
    addEntryPoint(elf);

    // Pass 2: ELF symbol table
    addSymbolFunctions(elf);

    // Pass 3-6: Heuristic scans on .text section
    const Section* text = elf.getTextSection();
    if (text != nullptr && text->data != nullptr && text->size >= 4) {
        scanJALTargets(*text);
        scanPrologues(*text);
        scanDataPointers(*text);
        scanComputedPointers(*text);
    }

    // Pass 7: Compute sizes from sorted addresses
    if (text != nullptr) {
        computeBoundaries(*text);
    }
}

// Pass 6: Function pointers constructed in executable code
//
// Callback addresses are not always present as literal 32-bit words. Optimized
// PsyQ-era code commonly materializes one with `lui reg, upper` followed by
// `addiu reg, reg, lower` (or `ori`) before passing it to an event API. These
// callbacks can also be leaf routines without a conventional prologue, so a
// JAL/prologue/data scan cannot find them.
//
// Treat a materialized in-text address as a callable entry only when it starts
// immediately after a complete `jr $ra` + delay-slot pair. This deliberately
// excludes ordinary constants and internal labels while recovering indirect
// callback targets.
void FunctionFinder::scanComputedPointers(const Section &text) {
    const uint32_t numInstructions = text.size / 4;
    constexpr uint32_t maxPairDistance = 4;

    for (uint32_t i = 0; i < numInstructions; ++i) {
        const uint32_t lui = readInstruction(text, i * 4);
        if (mips::getOpcode(lui) != 0x0Fu)
            continue;

        const uint32_t reg = mips::getRt(lui);
        if (reg == 0)
            continue;
        const uint32_t upper = (lui & 0xFFFFu) << 16;

        const uint32_t limit =
            std::min(numInstructions, i + maxPairDistance + 1);
        for (uint32_t j = i + 1; j < limit; ++j) {
            const uint32_t instr = readInstruction(text, j * 4);
            if (!mips::writesRegister(instr, reg))
                continue;

            const uint32_t op = mips::getOpcode(instr);
            if ((op != mips::OP_ADDIU && op != 0x0Du) ||
                mips::getRs(instr) != reg || mips::getRt(instr) != reg) {
                break;
            }

            const uint32_t lower = instr & 0xFFFFu;
            const uint32_t target =
                op == mips::OP_ADDIU
                    ? upper + static_cast<uint32_t>(
                                  static_cast<int32_t>(
                                      static_cast<int16_t>(lower)))
                    : upper | lower;

            if ((target & 3u) != 0 || !text.containsAddress(target) ||
                hasFunction(target) || target < text.vaddr + 8) {
                break;
            }

            const uint32_t targetOffset = target - text.vaddr;
            const uint32_t previous =
                readInstruction(text, targetOffset - 8);
            if (mips::isJR_RA(previous)) {
                addFunction(target, fmt::format("func_{:08X}", target),
                            FunctionSource::ComputedPointer);
            }
            break;
        }
    }
}

// Pass 1: Entry Point

void FunctionFinder::addEntryPoint(const ElfParser& elf) {
    addFunction(elf.getEntryPoint(), "__start", FunctionSource::EntryPoint);
}

// Pass 2: ELF Symbols

void FunctionFinder::addSymbolFunctions(const ElfParser& elf) {
    for (const auto& sym : elf.getSymbols()) {
        if (sym.isFunction() && sym.address != 0) {
            addFunction(sym.address, sym.name, FunctionSource::Symbol);
        }
    }
}

// Pass 5: Function pointers embedded in data
//
// Stripped PS-X executables frequently keep callback tables in the same load
// section as code.  A leaf callback may have neither a stack prologue nor a
// direct JAL reference, so the only evidence for its entry point is a stored
// address.  Accept such a pointer conservatively when the target begins right
// after a `jr $ra` and its delay slot.  This excludes ordinary jump-table
// labels inside a function while recovering post-return leaf entries such as
// Crash 3's 0x800206C4 callback.
void FunctionFinder::scanDataPointers(const Section &text) {
    const uint32_t numWords = text.size / 4;

    for (uint32_t i = 0; i < numWords; ++i) {
        const uint32_t target = readInstruction(text, i * 4);
        if ((target & 3u) != 0 || !text.containsAddress(target) ||
            hasFunction(target) || target < text.vaddr + 8)
            continue;

        const uint32_t targetOffset = target - text.vaddr;
        const uint32_t previous = readInstruction(text, targetOffset - 8);
        if (!mips::isJR_RA(previous))
            continue;

        addFunction(target, fmt::format("func_{:08X}", target),
                    FunctionSource::DataPointer);
    }
}

// Pass 3: JAL Target Scan

void FunctionFinder::scanJALTargets(const Section& text) {
    const uint32_t numInstructions = text.size / 4;

    for (uint32_t i = 0; i < numInstructions; ++i) {
        uint32_t instr = readInstruction(text, i * 4);
        uint32_t pc = text.vaddr + (i * 4);

        if (mips::isJAL(instr)) {
            uint32_t target = mips::jalTarget(pc, instr);

            // Only accept targets within the text section
            if (text.containsAddress(target)) {
                m_jalTargets.insert(target);

                if (!hasFunction(target)) {
                    // Generate name based on address
                    std::string name = fmt::format("func_{:08X}", target);
                    addFunction(target, name, FunctionSource::JALTarget);
                }
            }
        }
    }
}

// Pass 4: Prologue Pattern Scan

void FunctionFinder::scanPrologues(const Section& text) {
    const uint32_t numInstructions = text.size / 4;

    for (uint32_t i = 0; i < numInstructions; ++i) {
        uint32_t instr = readInstruction(text, i * 4);
        uint32_t addr = text.vaddr + (i * 4);

        if (mips::isStackPrologue(instr) && !hasFunction(addr)) {
            // Additional validation: check if preceded by JR $ra + delay slot
            // or by NOP padding (common between functions)
            bool likelyStart = false;
            uint32_t entryAddr = addr;

            if (i == 0) {
                // First instruction in section -- likely function start
                likelyStart = true;
            } else if (i >= 2) {
                // Check if the instruction 2 slots back is JR $ra
                // (the instruction right before is the delay slot)
                uint32_t prevInstr = readInstruction(text, (i - 2) * 4);
                if (mips::isJR_RA(prevInstr)) {
                    likelyStart = true;
                }
            }

            // Optimized functions can load globals before allocating their
            // stack frame.  If the prologue is one or two instructions after
            // a prior JR $ra + delay slot, the real entry is the first word
            // following that return.  Keep the window deliberately small and
            // reject control flow in the prefix so an internal branch target
            // is not mistaken for a callable boundary.
            if (!likelyStart) {
                constexpr uint32_t maxPrefixInstructions = 2;
                for (uint32_t prefix = 1;
                     prefix <= maxPrefixInstructions && i >= prefix + 2;
                     ++prefix) {
                    const uint32_t returnInstr =
                        readInstruction(text, (i - prefix - 2) * 4);
                    if (!mips::isJR_RA(returnInstr))
                        continue;

                    bool straightLinePrefix = true;
                    for (uint32_t p = i - prefix; p < i; ++p) {
                        const uint32_t prefixInstr =
                            readInstruction(text, p * 4);
                        if (mips::isJ(prefixInstr) ||
                            mips::isJAL(prefixInstr) ||
                            mips::isJR(prefixInstr) ||
                            mips::isBranch(prefixInstr)) {
                            straightLinePrefix = false;
                            break;
                        }
                    }
                    if (!straightLinePrefix)
                        continue;

                    entryAddr = text.vaddr + (i - prefix) * 4;
                    likelyStart = true;
                    break;
                }
            }

            if (i >= 1) {
                // Check if previous instruction is a NOP (padding)
                uint32_t prevInstr = readInstruction(text, (i - 1) * 4);
                if (mips::isNOP(prevInstr)) {
                    likelyStart = true;
                }
            }

            if (likelyStart) {
                std::string name = fmt::format("func_{:08X}", entryAddr);
                addFunction(entryAddr, name, FunctionSource::Prologue);
            }
        }
    }
}

// Pass 6: Compute Boundaries

void FunctionFinder::computeBoundaries(const Section& text) {
    // Sort functions by address
    std::sort(m_functions.begin(), m_functions.end());

    // Compute sizes: each function extends to the start of the next
    for (size_t i = 0; i < m_functions.size(); ++i) {
        if (m_functions[i].size != 0) {
            continue; // Already has size from symbol table
        }

        if (i + 1 < m_functions.size()) {
            m_functions[i].size = m_functions[i + 1].address - m_functions[i].address;
        } else {
            // Last function: extends to end of text section
            uint32_t textEnd = text.vaddr + text.size;
            if (m_functions[i].address < textEnd) {
                m_functions[i].size = textEnd - m_functions[i].address;
            }
        }
    }

    // No function may reach into the next one. Sizes set by earlier passes
    // (ELF symbols) and by `--add-func` after the fact never revisit each
    // other, so a containing function keeps its old extent and the same
    // bytes get emitted twice, under two names.
    clampOverlappingSizes(m_functions);

    // Determine leaf functions: scan each function for JAL/JALR instructions
    for (auto& func : m_functions) {
        if (func.size == 0 || !text.containsAddress(func.address)) {
            continue;
        }

        func.isLeaf = true;
        uint32_t offset = func.address - text.vaddr;
        uint32_t numInstr = func.size / 4;

        for (uint32_t i = 0; i < numInstr; ++i) {
            uint32_t instr = readInstruction(text, offset + i * 4);
            uint32_t opcode = mips::getOpcode(instr);

            if (opcode == mips::OP_JAL) {
                func.isLeaf = false;
                break;
            }
            if (opcode == mips::OP_SPECIAL && mips::getFunction(instr) == mips::FUNC_JALR) {
                func.isLeaf = false;
                break;
            }
        }
    }
}

// Queries

const FunctionInfo* FunctionFinder::findByAddress(uint32_t addr) const {
    // Binary search (m_functions is sorted)
    auto it = std::lower_bound(m_functions.begin(), m_functions.end(), addr,
        [](const FunctionInfo& f, uint32_t a) { return f.address < a; });

    if (it != m_functions.end() && it->address == addr) {
        return &(*it);
    }
    return nullptr;
}

const FunctionInfo* FunctionFinder::findContaining(uint32_t addr) const {
    if (m_functions.empty()) return nullptr;

    // Find last function with address <= addr
    auto it = std::upper_bound(m_functions.begin(), m_functions.end(), addr,
        [](uint32_t a, const FunctionInfo& f) { return a < f.address; });

    if (it == m_functions.begin()) return nullptr;
    --it;

    // Check if addr is within this function's range
    if (it->size > 0 && addr < it->address + it->size) {
        return &(*it);
    }
    // If size unknown, assume it could contain it
    if (it->size == 0) {
        return &(*it);
    }
    return nullptr;
}

// Helpers

void FunctionFinder::addFunction(uint32_t addr, const std::string& name, FunctionSource source) {
    // Don't add if already exists (prefer earlier source -- higher priority)
    if (hasFunction(addr)) {
        return;
    }

    FunctionInfo info;
    info.address = addr;
    info.size = 0;      // Computed later in computeBoundaries
    info.name = name;
    info.source = source;
    info.isLeaf = false; // Determined later

    m_functions.push_back(std::move(info));
}

void FunctionFinder::recomputeBoundaries(const ElfParser& elf) {
    const Section* text = elf.getTextSection();
    if (text != nullptr) {
        computeBoundaries(*text);
    }
}

bool FunctionFinder::hasFunction(uint32_t addr) const {
    return std::any_of(m_functions.begin(), m_functions.end(),
        [addr](const FunctionInfo& f) { return f.address == addr; });
}

uint32_t FunctionFinder::readInstruction(const Section& sec, uint32_t offset) {
    if (offset + 4 > sec.size || sec.data == nullptr) {
        return 0;
    }
    // Little-endian read (PS1 is MIPS LE)
    const uint8_t* p = sec.data + offset;
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace ps1recomp
