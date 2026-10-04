// Exports every function's disassembly and decompiled C to re/out/<program>/ for offline reading.
// Output is derived from the copyrighted binary and is gitignored.
// Addresses are written image-relative (SEG:OFF with load segment 0) to match re/symbols.csv.
//@category Vette
import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

public class VetteExportFunctions extends GhidraScript {
    int base;

    @Override
    public void run() throws Exception {
        Path root = Path.of(getSourceFile().getParentFile().getParentFile().getAbsolutePath(), "out",
            currentProgram.getName().replaceAll("[^A-Za-z0-9_.-]", "_"));
        Path funcs = root.resolve("funcs");
        Files.createDirectories(funcs);
        base = loaderBaseSegment();

        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        int count = 0;
        try (PrintWriter index = new PrintWriter(Files.newBufferedWriter(root.resolve("functions.tsv")))) {
            index.println("addr\tname\tbytes\tcallers");
            for (Function fn : currentProgram.getFunctionManager().getFunctions(true)) {
                monitor.checkCancelled();
                String addr = rel(fn.getEntryPoint());
                index.printf("%s\t%s\t%d\t%d%n", addr, fn.getName(), fn.getBody().getNumAddresses(),
                    fn.getCallingFunctions(monitor).size());
                Path file = funcs.resolve(addr.replace(':', '_') + "_" + fn.getName() + ".txt");
                try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(file))) {
                    w.println("// " + addr + " " + fn.getName());
                    int fnSeg = ((SegmentedAddress) fn.getEntryPoint()).getSegment();
                    for (Instruction ins : currentProgram.getListing().getInstructions(fn.getBody(), true)) {
                        w.printf("%s  %s%n", rel(ins.getAddress()), relOperands(ins.toString(), fnSeg));
                    }
                    w.println();
                    DecompileResults res = decomp.decompileFunction(fn, 60, monitor);
                    if (res.decompileCompleted()) {
                        w.println(res.getDecompiledFunction().getC());
                    } else {
                        w.println("// decompile failed: " + res.getErrorMessage());
                    }
                }
                count++;
            }
        }
        decomp.dispose();
        println("VetteExportFunctions: wrote " + count + " functions to " + root);
    }

    String rel(Address a) {
        if (a instanceof SegmentedAddress sa) {
            return String.format("%04X:%04X", sa.getSegment() - base, sa.getSegmentOffset());
        }
        return a.toString();
    }

    static final Pattern SEG_ADDR = Pattern.compile("0x([0-9a-f]{1,4}):([0-9a-f]{1,4})");

    // Ghidra prints branch/call targets in a normalized segment form (e.g. 0x5000:078d). Rewrite them
    // image-relative: the current function's segment if the target lies inside it, otherwise the
    // segment of the memory block (MZ segment) containing the target.
    String relOperands(String text, int fnSeg) {
        Matcher m = SEG_ADDR.matcher(text);
        StringBuilder sb = new StringBuilder();
        while (m.find()) {
            int mSeg = Integer.parseInt(m.group(1), 16);
            int linear = mSeg * 16 + Integer.parseInt(m.group(2), 16);
            int seg = fnSeg;
            if (linear - fnSeg * 16 < 0 || linear - fnSeg * 16 > 0xFFFF) {
                seg = mSeg;
                var space = (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();
                MemoryBlock block = currentProgram.getMemory().getBlock(space.getAddress(mSeg, linear - mSeg * 16));
                if (block != null && block.getStart() instanceof SegmentedAddress bs) {
                    seg = bs.getSegment();
                }
            }
            m.appendReplacement(sb, String.format("%04X:%04X", seg - base, linear - seg * 16));
        }
        m.appendTail(sb);
        return sb.toString();
    }

    int loaderBaseSegment() {
        var it = currentProgram.getSymbolTable().getExternalEntryPointIterator();
        if (!it.hasNext()) {
            return 0;
        }
        Address entry = it.next();
        // DOS build: real entry is image-relative 3009:0025. Other programs keep loader addresses.
        return currentProgram.getName().toLowerCase().contains("vette_unpacked")
            ? ((SegmentedAddress) entry).getSegment() - 0x3009 : 0;
    }
}
