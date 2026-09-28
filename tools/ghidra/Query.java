// Headless Ghidra query against the analysed Souls.exe project.
// Args: <cmd> <arg> [len]
//   refs ADDR [LEN]   every reference (read/write/call/data) into [ADDR, ADDR+LEN), with the
//                     referring function, ref type and instruction
//   writes ADDR [LEN] as refs, WRITE references only
//   callers FUNC      call sites of a function (address or name)
//   decomp FUNC       decompiled C of one function
//   disasm FUNC       disassembly of one function
//   func ADDR         the function containing ADDR
// Output goes to stdout, prefixed "Q| " so the wrapper can strip Ghidra's own log lines.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class Query extends GhidraScript {
    private void out(String s) { println("Q| " + s); }

    private Function resolve(String s) throws Exception {
        if (s.startsWith("FUN_")) s = "0x" + s.substring(4);
        if (s.startsWith("0x") || s.matches("[0-9a-fA-F]+")) {
            Address a = toAddr(Long.parseLong(s.replace("0x", ""), 16));
            Function f = getFunctionContaining(a);
            if (f == null) f = getFunctionAt(a);
            return f;
        }
        for (Function f : currentProgram.getFunctionManager().getFunctions(true))
            if (f.getName().equals(s)) return f;
        return null;
    }

    private String where(Address a) {
        Function f = getFunctionContaining(a);
        Instruction ins = getInstructionAt(a);
        return a + " " + (f == null ? "-" : f.getName() + "@" + f.getEntryPoint())
            + " | " + (ins == null ? "" : ins.toString());
    }

    private void refs(String addr, long len, boolean writesOnly) {
        Address base = toAddr(Long.parseLong(addr.replace("0x", ""), 16));
        for (long i = 0; i < len; ++i) {
            Address t = base.add(i);
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(t);
            while (it.hasNext()) {
                Reference r = it.next();
                if (writesOnly && !r.getReferenceType().isWrite()) continue;
                out(t + " <- " + r.getReferenceType() + " " + where(r.getFromAddress()));
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        if (a.length < 2) { out("usage: refs|writes ADDR [LEN] | callers|decomp|disasm|func X"); return; }
        String cmd = a[0];
        long len = a.length > 2 ? Long.decode(a[2]) : 1;
        switch (cmd) {
        case "refs": refs(a[1], len, false); break;
        case "writes": refs(a[1], len, true); break;
        case "func": {
            Function f = resolve(a[1]);
            out(f == null ? "no function" : f.getName() + " " + f.getEntryPoint() + " " + f.getBody());
            break;
        }
        case "callers": {
            Function f = resolve(a[1]);
            if (f == null) { out("no function"); break; }
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while (it.hasNext()) {
                Reference r = it.next();
                out(r.getReferenceType() + " " + where(r.getFromAddress()));
            }
            break;
        }
        case "decomp": {
            Function f = resolve(a[1]);
            if (f == null) { out("no function"); break; }
            DecompInterface d = new DecompInterface();
            d.openProgram(currentProgram);
            DecompileResults res = d.decompileFunction(f, 120, monitor);
            String c = res.getDecompiledFunction() == null ? "decompile failed" : res.getDecompiledFunction().getC();
            for (String line : c.split("\n")) out(line);
            d.dispose();
            break;
        }
        case "disasm": {
            Function f = resolve(a[1]);
            if (f == null) { out("no function"); break; }
            InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
            while (it.hasNext()) { Instruction ins = it.next(); out(ins.getAddress() + "  " + ins); }
            break;
        }
        default: out("unknown command " + cmd);
        }
    }
}
