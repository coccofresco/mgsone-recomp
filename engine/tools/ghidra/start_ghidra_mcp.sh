#!/usr/bin/env bash
# start_ghidra_mcp.sh -- Launch Ghidra with GhidraMCP server active
#
# This script:
#   1. Sets JAVA_HOME to JDK 21 (required by Ghidra 12+)
#   2. Launches Ghidra GUI
#   3. After Ghidra starts, GhidraMCP automatically exposes port 8080
#      which Claude Code reads via the "ghidra" entry in .mcp.json
#
# Usage:
#   bash tools/ghidra/start_ghidra_mcp.sh
#
# Workflow:
#   1. Run this script -> Ghidra opens
#   2. File -> New Project (or Open Existing Project)
#   3. File -> Import File -> select the PS1 binary (SLUS_000.05 or .bin)
#   4. Ghidra auto-analyzes (accept defaults, enable "PSX Executable Loader")
#   5. GhidraMCP is now live on http://localhost:8080/sse
#   6. Claude Code tools: decompile_function, find_callers, search_functions work
#      directly via the "ghidra" MCP server in .mcp.json

export JAVA_HOME="/usr/lib/jvm/java-21-openjdk-amd64"
export PATH="$JAVA_HOME/bin:$PATH"

GHIDRA_DIR="$HOME/ghidra"

if [ ! -f "$GHIDRA_DIR/ghidraRun" ]; then
    echo "[ERROR] Ghidra not found at $GHIDRA_DIR"
    echo "Run: wget <ghidra_url> && unzip && mv ghidra_* ~/ghidra"
    exit 1
fi

echo "[+] Launching Ghidra 12.0.4 with GhidraMCP..."
echo "    Java: $(java -version 2>&1 | head -1)"
echo "    Extensions: $(ls $GHIDRA_DIR/Ghidra/Extensions/ | tr '\n' ' ')"
echo ""
echo "    After Ghidra opens:"
echo "    1. Enable extensions: File -> Install Extensions (check GhidraMCP + ghidra_psx_ldr)"
echo "    2. Import the PS1 binary via File -> Import File"
echo "    3. GhidraMCP will be live on http://localhost:8080/sse"
echo ""

"$GHIDRA_DIR/ghidraRun" &
echo "[+] Ghidra PID: $!"
echo "[+] Waiting for GhidraMCP to start on port 8080..."
