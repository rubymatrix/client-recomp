// Export the recompilation metadata for FINAL FANTASY XIV 1.23b ffxivgame.exe, in the format
// recomp/recomp.py reads ("ffxi-recomp-meta/1"): every function's entry and body ranges, every
// `jmp [reg*4 + table]` switch with its case targets, and the indirect jumps that are tail calls.
//
// This is Export.py and Switches.py in one Java post-script, because Ghidra 12 no longer runs
// Jython scripts headless. It is read-only: switch bounds the analysis left unresolved are read
// from the MSVC guard (cmp reg, N ; ja default ; [movzx reg, byte [reg + index]] ; jmp [reg*4 +
// table]) and the cases are added to the metadata only, not to the Ghidra program.
//
// Addresses and shapes only: no bytes of the game image are written.
//
// Args: <out.json> <sha256 of ffxivgame.exe> <build label>
//
//@category FFXIVRecomp

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.FlowType;

public class ExportFFXIV extends GhidraScript {

    private static final Pattern SCALED =
        Pattern.compile("JMP dword ptr \\[(E[A-Z]{2})\\*0x4 \\+ (0x[0-9a-f]+)\\]");
    private static final Pattern CMP = Pattern.compile("CMP (E[A-Z]{2}),(0x[0-9a-f]+|\\d+)$");
    private static final Pattern IDX =
        Pattern.compile("(?:MOV|MOVZX) (?:[A-Z]{2,3}),byte ptr \\[(E[A-Z]{2}) \\+ (0x[0-9a-f]+)\\]");

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = args[0], sha256 = args[1], label = args[2];
        Listing listing = currentProgram.getListing();
        Memory mem = currentProgram.getMemory();
        MemoryBlock text = mem.getBlock(".text");
        AddressSpace space = currentProgram.getAddressFactory().getDefaultAddressSpace();

        StringBuilder fn = new StringBuilder();
        int nfn = 0;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            if (!text.contains(f.getEntryPoint())) continue;
            if (nfn++ > 0) fn.append(',');
            fn.append("{\"entry\":").append(f.getEntryPoint().getOffset()).append(",\"ranges\":[");
            boolean first = true;
            for (AddressRange r : f.getBody()) {
                if (!first) fn.append(',');
                first = false;
                fn.append('[').append(r.getMinAddress().getOffset()).append(',')
                  .append(r.getMaxAddress().getOffset() + 1).append(']');
            }
            fn.append("],\"thunk\":").append(f.isThunk()).append('}');
        }

        StringBuilder sw = new StringBuilder();
        StringBuilder tj = new StringBuilder();
        List<String> report = new ArrayList<>();
        int nsw = 0, ntj = 0, unbounded = 0;
        InstructionIterator it = listing.getInstructions(text.getStart(), true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            if (!text.contains(ins.getAddress())) break;
            if (!ins.getMnemonicString().equals("JMP")) continue;
            Matcher m = SCALED.matcher(ins.toString());
            FlowType ft = ins.getFlowType();
            if (!m.matches()) {
                if (ft.isComputed() || ft.isCall()) {
                    if (ntj++ > 0) tj.append(',');
                    tj.append(ins.getAddress().getOffset());
                }
                continue;
            }
            long table = Long.parseLong(m.group(2).substring(2), 16);
            TreeSet<Long> targets = new TreeSet<>();
            for (Address a : ins.getFlows()) targets.add(a.getOffset());

            // The guard's bound, a few instructions up.
            Long bound = null, idxTable = null;
            Instruction p = ins;
            for (int k = 0; k < 12 && p != null; k++) {
                p = p.getPrevious();
                if (p == null) break;
                String s = p.toString();
                Matcher mi = IDX.matcher(s);
                if (mi.matches() && idxTable == null) idxTable = Long.parseLong(mi.group(2).substring(2), 16);
                Matcher mc = CMP.matcher(s);
                if (mc.matches()) {
                    bound = Long.decode(mc.group(2));
                    break;
                }
            }
            if (bound != null && bound < 4096) {
                long n = bound + 1;
                if (idxTable != null) {
                    int max = 0;
                    for (long i = 0; i <= bound; i++)
                        max = Math.max(max, mem.getByte(space.getAddress(idxTable + i)) & 0xFF);
                    n = max + 1;
                }
                List<Long> read = new ArrayList<>();
                boolean ok = true;
                for (long i = 0; i < n; i++) {
                    long v = mem.getInt(space.getAddress(table + 4 * i)) & 0xFFFFFFFFL;
                    Address a = space.getAddress(v);
                    // Cases live near their jump: a far target means the guard is not this switch's.
                    if (!text.contains(a) || Math.abs(a.subtract(ins.getAddress())) > 0x10000) {
                        ok = false;
                        break;
                    }
                    read.add(v);
                }
                if (ok) targets.addAll(read);
                else report.add("BAD_TABLE " + ins.getAddress());
            } else if (targets.isEmpty()) {
                unbounded++;
                report.add("UNBOUNDED " + ins.getAddress() + " " + ins);
            }
            if (nsw++ > 0) sw.append(',');
            sw.append("{\"at\":").append(ins.getAddress().getOffset()).append(",\"table\":").append(table)
              .append(",\"index_reg\":\"").append(m.group(1)).append("\",\"targets\":[");
            boolean first = true;
            for (Long t : targets) {
                if (!first) sw.append(',');
                first = false;
                sw.append(t);
            }
            sw.append("]}");
        }

        try (PrintWriter w = new PrintWriter(new FileWriter(outPath))) {
            w.print("{\"format\":\"ffxi-recomp-meta/1\",\"module\":\"ffxivgame.exe\",\"build\":\"" + label + "\",");
            w.print("\"sha256\":\"" + sha256 + "\",\"image_base\":" + currentProgram.getImageBase().getOffset() + ",");
            w.print("\"text\":[" + text.getStart().getOffset() + "," + (text.getEnd().getOffset() + 1) + "],");
            w.print("\"functions\":[" + fn + "],\"switches\":[" + sw + "],\"tail_jumps\":[" + tj + "]}");
        }
        for (String r : report) println(r);
        println(String.format("functions %d switches %d (unbounded %d) tail_jumps %d", nfn, nsw, unbounded, ntj));
    }
}
