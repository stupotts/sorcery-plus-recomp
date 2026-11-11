# Native build

Follow the [README](../README.md) to supply a compatible CPC disk image and run the build. `-CheckDisk` stops before compilation. For a disk with a different SHA-256, `-TryOtherDisk` skips the confirmation prompt and runs the required data checks. The build accepts explicit tool paths when needed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1 -Disk 'C:\Games\Sorcery Plus (UK) (1985).dsk' -Python 'C:\Tools\python.exe' -Node 'C:\Tools\node.exe' -Zig 'C:\Tools\zig.exe'
```

The build extracts the disk into a temporary work folder and identifies the UK or French edition from the decoded code hash. `editions.py` supplies the matching Z80 addresses, save format, and atlas text locations. The build translates the original routines into C and compiles a console capture helper that draws rooms using that disk's renderer and tiles. The atlas scripts generate the interactive pages and verify the door links before the final Windows GUI build embeds the generated data. The final EXE is written to `native\build\SorceryPlus.exe` after successful compilation.

An existing EXE must be closed before it can be replaced. If a build fails, check the first error in the terminal. The original disk image is opened for reading.

For an independent extraction, pass the disk path and a new destination folder to the extractor. Keep the resulting files outside the source repository.

```powershell
node .\tools\sorcery-extract.mjs 'C:\Games\Sorcery Plus (UK) (1985).dsk' 'C:\Games\SorceryPlus-Local-Extraction'
```

To compare two optimization levels, build with `-Optimize 0`, copy the EXE to a separate folder, then rebuild the same disk with `-Optimize 2`. Pass both EXE paths to [`verify.ps1`](verify.ps1).
