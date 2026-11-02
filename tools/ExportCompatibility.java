import ghidra.app.script.GhidraScript;
import java.nio.file.Files;
import java.nio.file.Path;

// Run against the previously analyzed reference program, without modifying it.
public class ExportCompatibility extends GhidraScript {
    public void run() throws Exception {
        byte[] image = new byte[0x4000000];
        for (var block : currentProgram.getMemory().getBlocks()) {
            long offset = block.getStart().subtract(currentProgram.getImageBase());
            if (!block.isInitialized() || offset < 0 || offset + block.getSize() > image.length) continue;
            currentProgram.getMemory().getBytes(block.getStart(), image, (int)offset, (int)block.getSize());
        }
        Files.write(Path.of(getScriptArgs()[0]), image);
    }
}
