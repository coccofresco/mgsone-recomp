// ExportPS1Functions.java -- Ghidra Script
//
// Java equivalent of ExportPS1Functions.py.
// Preferred when running via Ghidra's auto-analysis (headless mode) since
// Java scripts compile ahead of time and run faster than Jython.
//
// Usage:
//   analyzeHeadless /path/to/project ProjectName \
//       -import SLUS_000.05 \
//       -postScript ExportPS1Functions.java /tmp/ps1_functions.csv \
//       -scriptPath /path/to/tools/ghidra
//
// @category PS1Recomp
// @author ps1-recomp project

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.address.AddressSetView;

import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportPS1Functions extends GhidraScript {

    private static final int  MIN_SIZE     = 4;
    private static final boolean SKIP_THUNKS   = true;
    private static final boolean SKIP_EXTERNAL = true;

    @Override
    public void run() throws Exception {
        // Output path: first script argument, or default beside project dir
        String outputPath = getScriptArgs().length > 0
            ? getScriptArgs()[0]
            : currentProgram.getDomainFile()
                .getParent()
                .getProjectLocator()
                .getProjectDir()
                .getAbsolutePath() + "/ps1_functions.csv";

        FunctionManager fm = currentProgram.getFunctionManager();
        int count = 0;

        try (PrintWriter writer = new PrintWriter(new FileWriter(outputPath))) {
            writer.println("Name,StartAddress,EndAddress,Size");

            for (Function func : fm.getFunctions(true)) {
                if (SKIP_THUNKS   && func.isThunk())    continue;
                if (SKIP_EXTERNAL && func.isExternal())  continue;

                AddressSetView body = func.getBody();
                long size = body.getNumAddresses();

                if (size < MIN_SIZE) continue;

                long startOff = func.getEntryPoint().getOffset();
                long endOff   = body.getMaxAddress().getOffset();

                writer.printf("%s,0x%08X,0x%08X,0x%X%n",
                    func.getName(), startOff, endOff, size);
                count++;
            }
        }

        printf("[ExportPS1Functions] Exported %d functions to: %s%n", count, outputPath);
    }
}
