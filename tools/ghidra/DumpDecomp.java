// Headless Ghidra script: dump every function's decompiled C plus a function index.
// Usage: analyzeHeadless <proj> <name> -import Souls.exe -postScript DumpDecomp.java <outdir>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.File;
import java.io.PrintWriter;

public class DumpDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args.length > 0 ? args[0] : "decomp");
        out.mkdirs();
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        try (PrintWriter all = new PrintWriter(new File(out, "all.c"));
             PrintWriter idx = new PrintWriter(new File(out, "functions.tsv"))) {
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext() && !monitor.isCancelled()) {
                Function f = it.next();
                if (f.isExternal() || f.isThunk()) continue;
                idx.printf("%s\t%s\t%d%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses());
                DecompileResults r = ifc.decompileFunction(f, 60, monitor);
                all.printf("// ==== %s %s ====%n", f.getEntryPoint(), f.getName());
                if (r != null && r.decompileCompleted()) {
                    all.println(r.getDecompiledFunction().getC());
                } else {
                    all.println("// decompile failed");
                }
            }
        }
        ifc.dispose();
    }
}
