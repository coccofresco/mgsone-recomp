# ExportPS1Functions.py -- Ghidra Script
#
# Exports all non-thunk functions from a PS1 binary loaded in Ghidra to a CSV
# file that ps1Analyzer can consume to skip its own function-boundary detection.
#
# Usage (Ghidra Script Manager):
#   1. Load your PS1 EXE / BIN in Ghidra and run auto-analysis.
#   2. Open Script Manager -> Run Script -> select this file.
#   3. When prompted, choose the output path (default: <project>/ps1_functions.csv).
#
# Alternatively run headless:
#   analyzeHeadless /path/to/project ProjectName \
#       -import SLUS_000.05 \
#       -postScript ExportPS1Functions.py /tmp/ps1_functions.csv \
#       -scriptPath /path/to/tools/ghidra
#
# CSV format (no header):
#   Name,StartAddress,EndAddress,Size
#   func_8012D030,0x8012D030,0x8012D09C,0x6C
#
# Adapted from PS2Recomp/ps2xRecomp/tools/ghidra/ExportPS2Functions.py

import os
from ghidra.program.model.symbol import SymbolType

# Configuration
DEFAULT_OUTPUT = os.path.join(
    currentProgram.getDomainFile().getParent().getProjectLocator().getProjectDir().toString(),
    "ps1_functions.csv",
)
SKIP_THUNKS   = True   # Exclude thunk functions (wrappers with a single JR)
SKIP_EXTERNAL = True   # Exclude external / imported symbols
MIN_SIZE      = 4      # Skip functions smaller than N bytes (likely false positives)

# Ask user for output path
output_path = askString(
    "Export PS1 Functions",
    "Output CSV path:",
    DEFAULT_OUTPUT,
)

# Iterate functions
function_manager = currentProgram.getFunctionManager()
listing = currentProgram.getListing()
addr_factory = currentProgram.getAddressFactory()

rows = []
for func in function_manager.getFunctions(True):   # True = forward iteration
    if SKIP_THUNKS and func.isThunk():
        continue
    if SKIP_EXTERNAL and func.isExternal():
        continue

    entry = func.getEntryPoint()
    body  = func.getBody()
    size  = body.getNumAddresses()

    if size < MIN_SIZE:
        continue

    # Compute end address (inclusive last byte of last range)
    end_addr = body.getMaxAddress()

    name       = func.getName()
    start_hex  = "0x{:08X}".format(entry.getOffset())
    end_hex    = "0x{:08X}".format(end_addr.getOffset())
    size_hex   = "0x{:X}".format(size)

    rows.append("{},{},{},{}".format(name, start_hex, end_hex, size_hex))

# Write output
with open(output_path, "w") as f:
    f.write("Name,StartAddress,EndAddress,Size\n")
    for row in rows:
        f.write(row + "\n")

print("[ExportPS1Functions] Exported {} functions to: {}".format(len(rows), output_path))
