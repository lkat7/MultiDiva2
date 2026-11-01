// Ghidra headless script: reads a list of commands and writes the results to a text file.
//   decompile <addr>        decompile the function at addr (created if there is none)
//   callers <addr>          every `call addr` (E8 rel32) in the image, with the call site's address
//   disasm <addr> <count>   <count> instructions from addr
// Addresses are hex (0x optional). Lines starting with # are ignored.
// Usage: analyzeHeadless ... -postScript ExportDecompiled.java <commands.txt> <out.txt>
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.mem.MemoryBlock;

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;

public class ExportDecompiled extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("usage: ExportDecompiled.java <commands.txt> <out.txt>");
            return;
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        byte[] image = null;
        Address imageStart = null;
        try (PrintWriter out = new PrintWriter(new File(args[1]), "UTF-8")) {
            for (String raw : Files.readAllLines(new File(args[0]).toPath())) {
                String line = raw.trim();
                if (line.isEmpty() || line.startsWith("#"))
                    continue;
                String[] part = line.split("\\s+");
                out.println("// ===== " + line);
                try {
                    String cmd = part.length > 1 ? part[0].toLowerCase() : "decompile";
                    String addrText = part.length > 1 ? part[1] : part[0];
                    Address addr = toAddr(Long.parseUnsignedLong(addrText.replace("0x", ""), 16));
                    if (cmd.equals("decompile")) {
                        Function f = getFunctionAt(addr);
                        if (f == null) {
                            disassemble(addr);
                            f = createFunction(addr, null);
                        }
                        if (f == null) {
                            out.println("// could not make a function at " + addr);
                            continue;
                        }
                        DecompileResults r = decompiler.decompileFunction(f, 90, monitor);
                        out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                    } else if (cmd.equals("disasm")) {
                        int count = part.length > 2 ? Integer.parseInt(part[2]) : 40;
                        disassemble(addr);
                        Instruction ins = getInstructionAt(addr);
                        for (int i = 0; i < count && ins != null; i++) {
                            out.println(ins.getAddress() + "  " + ins);
                            ins = ins.getNext();
                        }
                    } else if (cmd.equals("callers")) {
                        if (image == null) {
                            MemoryBlock b = currentProgram.getMemory().getBlocks()[0];
                            image = new byte[(int) b.getSize()];
                            b.getBytes(b.getStart(), image);
                            imageStart = b.getStart();
                        }
                        long target = addr.getOffset();
                        long start = imageStart.getOffset();
                        int found = 0;
                        for (int i = 0; i + 5 <= image.length; i++) {
                            if (image[i] != (byte) 0xE8)
                                continue;
                            int rel = (image[i + 1] & 0xFF) | (image[i + 2] & 0xFF) << 8 | (image[i + 3] & 0xFF) << 16 | (image[i + 4] & 0xFF) << 24;
                            if (start + i + 5 + rel == target) {
                                out.println("call at " + Long.toHexString(start + i));
                                found++;
                            }
                        }
                        out.println("// " + found + " call site(s)");
                    } else {
                        out.println("// unknown command " + cmd);
                    }
                } catch (Exception e) {
                    out.println("// error: " + e);
                }
                out.flush();
            }
        }
    }
}
