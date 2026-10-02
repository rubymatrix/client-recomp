// Decompile the functions at the given addresses (hex) and print their C.
//@category FFXIVRecomp
import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (String a : getScriptArgs()) {
            Function f = getFunctionContaining(toAddr(Long.parseLong(a, 16)));
            if (f == null) { println("NOFUNC " + a); continue; }
            DecompileResults r = d.decompileFunction(f, 60, monitor);
            println("==== " + a + " " + f.getName());
            println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "FAILED " + r.getErrorMessage());
        }
    }
}
