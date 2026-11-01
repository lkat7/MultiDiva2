# Runs commands.txt through Ghidra (headless) on a dump made by the mod's "Dump game image" button.
#   .\run.ps1                       uses %LOCALAPPDATA%\MultiDiva2\dump\game_140000000.bin and .\commands.txt
#   .\run.ps1 -Commands other.txt -Out other-out.txt
# Needs Java 21 and Ghidra 11.x. Set GHIDRA_HOME to the Ghidra folder (the one with ghidraRun.bat) or pass -Ghidra.
param(
    [string]$Dump = "$env:LOCALAPPDATA\MultiDiva2\dump\game_140000000.bin",
    [string]$Commands = "$PSScriptRoot\commands.txt",
    [string]$Out = "$PSScriptRoot\out.txt",
    [string]$Ghidra = $env:GHIDRA_HOME
)
if (-not $Ghidra) { throw "Set GHIDRA_HOME to the Ghidra folder (with ghidraRun.bat), or pass -Ghidra" }
if (-not (Test-Path $Dump)) { throw "No dump at $Dump: press 'Dump game image' in the mod's Ctrl+F8 panel first" }
$headless = Join-Path $Ghidra "support\analyzeHeadless.bat"
$project = Join-Path $env:LOCALAPPDATA "MultiDiva2\ghidra"
New-Item -ItemType Directory -Force $project | Out-Null
# -noanalysis: decompile exactly the functions asked for (fast); the image is a raw dump loaded at the game's base.
& $headless $project MultiDiva -import $Dump -overwrite -noanalysis `
    -loader BinaryLoader -loader-baseAddr 0x140000000 -processor "x86:LE:64:default" -cspec windows `
    -scriptPath $PSScriptRoot -postScript ExportDecompiled.java $Commands $Out
Write-Host "Done: $Out"
