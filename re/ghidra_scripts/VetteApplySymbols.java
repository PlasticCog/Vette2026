// Applies re/symbols.csv (image-relative SEG:OFF names) to the DOS VETTE.EXE program.
// Ghidra's MZ loader relocates the image to a base segment (0x1000 by default); the base is
// derived from the entry point so the CSV can stay loader-independent.
//@category Vette
import java.io.BufferedReader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.address.SegmentedAddressSpace;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class VetteApplySymbols extends GhidraScript {
    static final int ENTRY_SEG = 0x3009; // image-relative CS of the real entry point

    @Override
    public void run() throws Exception {
        if (!currentProgram.getName().toLowerCase().contains("vette_unpacked")) {
            println("VetteApplySymbols: skipping " + currentProgram.getName());
            return;
        }
        Path csv = Path.of(getSourceFile().getParentFile().getParentFile().getAbsolutePath(), "symbols.csv");
        int base = loaderBaseSegment();
        SegmentedAddressSpace space =
            (SegmentedAddressSpace) currentProgram.getAddressFactory().getDefaultAddressSpace();

        int applied = 0;
        try (BufferedReader r = Files.newBufferedReader(csv)) {
            String line = r.readLine(); // header
            while ((line = r.readLine()) != null) {
                if (line.isBlank() || line.startsWith("#")) {
                    continue;
                }
                List<String> f = parseCsv(line);
                String[] so = f.get(0).split(":");
                Address addr = space.getAddress(Integer.parseInt(so[0], 16) + base, Integer.parseInt(so[1], 16));
                String name = f.get(1), kind = f.get(2), comment = f.size() > 3 ? f.get(3) : "";
                if (!currentProgram.getMemory().contains(addr)) {
                    printerr("not in memory: " + f.get(0) + " " + name);
                    continue;
                }
                if (kind.equals("func")) {
                    Function fn = getFunctionAt(addr);
                    if (fn == null) {
                        disassemble(addr);
                        fn = createFunction(addr, name);
                    }
                    if (fn != null) {
                        fn.setName(name, SourceType.USER_DEFINED);
                    }
                    if (!comment.isEmpty()) {
                        setPlateComment(addr, comment);
                    }
                } else {
                    createLabel(addr, name, true, SourceType.USER_DEFINED);
                    if (!comment.isEmpty()) {
                        currentProgram.getListing().setComment(addr, CodeUnit.EOL_COMMENT, comment);
                    }
                }
                applied++;
            }
        }
        println("VetteApplySymbols: applied " + applied + " symbols (loader base segment " +
            Integer.toHexString(base) + ")");
    }

    int loaderBaseSegment() {
        Address entry = currentProgram.getSymbolTable().getExternalEntryPointIterator().next();
        return ((SegmentedAddress) entry).getSegment() - ENTRY_SEG;
    }

    static List<String> parseCsv(String line) {
        List<String> out = new ArrayList<>();
        StringBuilder cur = new StringBuilder();
        boolean quoted = false;
        for (int i = 0; i < line.length(); i++) {
            char c = line.charAt(i);
            if (quoted) {
                if (c == '"' && i + 1 < line.length() && line.charAt(i + 1) == '"') {
                    cur.append('"');
                    i++;
                } else if (c == '"') {
                    quoted = false;
                } else {
                    cur.append(c);
                }
            } else if (c == '"') {
                quoted = true;
            } else if (c == ',') {
                out.add(cur.toString());
                cur.setLength(0);
            } else {
                cur.append(c);
            }
        }
        out.add(cur.toString());
        return out;
    }
}
