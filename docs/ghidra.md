# Reading the game's code with Ghidra

For your own copy, to work out how the game does things (interoperability). The dump and everything Ghidra writes from it
is game code: **keep it on your PC, never commit or share it** (the repo ignores `tools/re/ghidra/out*.txt`).

## Once
1. Install **Java 21** (e.g. Temurin) and **Ghidra 11.x** (https://ghidra-sre.org). Unzip Ghidra anywhere.
2. Set `GHIDRA_HOME` to the Ghidra folder (the one with `ghidraRun.bat`), e.g. in PowerShell:
   `setx GHIDRA_HOME "C:\tools\ghidra_11.3_PUBLIC"`, then open a new terminal.

## Each time the game changes (and the first time)
1. Start the game with the mod, open song select (the code is decrypted by then), press Ctrl+F8 (`research=1` in the ini)
   and click **Dump game image to file (research)**. It writes `%LOCALAPPDATA%\MultiDiva2\dump\game_140000000.bin`
   (the first 16 MB of the game's memory image: its code and the tables after it, loaded at 0x140000000, so addresses are the same ones the notes use) and
   `game_sections.txt`. Pages that could not be read are zero; the log says how many.

## Asking questions
1. Edit `tools/re/ghidra/commands.txt`: one command per line.
   - `decompile 1406DE9F0` the function at that address as C (made if Ghidra has none there)
   - `callers 1406DE9F0` every call to it (E8 rel32) in the image
   - `disasm 1406E1098 60` 60 instructions from an address
2. Run `powershell -File tools/re/ghidra/run.ps1`. It imports the dump into a project under
   `%LOCALAPPDATA%\MultiDiva2\ghidra` (no auto analysis: only the functions you ask for are built, so it takes a minute)
   and writes `tools/re/ghidra/out.txt`.
3. Open `out.txt`, or send the part you need to Claude.

## Notes
- Part of the game is virtualized by its protector: those functions decompile to nonsense or a jump into a stub. Stick to
  functions the mod's watches pointed at (they ran as normal code).
- Without analysis Ghidra has no names and finds no cross references of its own; `callers` is a plain byte scan and may
  list a few false hits.
