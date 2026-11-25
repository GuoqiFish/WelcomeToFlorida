$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& 'C:\Program Files\LLVM\bin\clang-cl.exe' /nologo /W4 /O2 /Zi `
    (Join-Path $PSScriptRoot 'probe_debugger.c') `
    ("/Fo" + (Join-Path $out 'probe_debugger.obj')) `
    ("/Fe" + (Join-Path $out 'probe_debugger.exe')) `
    /link dbghelp.lib user32.lib ("/PDB:" + (Join-Path $out 'probe_debugger.pdb'))
if ($LASTEXITCODE -ne 0) { throw 'Debugger build failed' }
