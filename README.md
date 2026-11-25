# Xbox Scarlett compatibility probe

This directory contains a deliberately incomplete Windows-side probe for the
supplied Scarlett executable. It provides selected Xbox export names and a
DirectStorage adapter so initialization boundaries can be observed. It is not
an Xbox or GameOS emulator.

Build the probe DLLs with:

```powershell
.\compat\build.ps1
```

Run the executable under the diagnostic debugger with:

```powershell
.\compat\run-probe.ps1
```

The runner temporarily places the probe DLLs beside the executable, restores or
removes them in `finally`, and records hashes, logs, and cleanup status under
`compat/runs`. It refuses to start if another game instance is running. A probe
exit caused by an exception is diagnostic only; no exception is skipped.

## Current observed boundary

The DirectStorage self-test passes file and memory reads, queue submission,
completion status, events, and close-before-submit behavior. In the latest game
probe (`20261125-105006-fd7052a2`), the game also successfully obtains its
DirectStorage factory and sets a 32 MiB staging buffer. The game then reaches an
intentional `INT3` at executable RVA `0x2237178`, before its later Win32 window
creation and `D3D12XboxCreateDevice` path. The debugger stops there without
continuing past the breakpoint. No splash window or animation has been observed.

Before that breakpoint, the Windows Game Runtime reports that the app's display
name and publisher values do not satisfy its `ms-resource:` URI pattern. This is
evidence of missing packaged-app metadata in the loose-exe environment; a causal
link to the `INT3` has not been established.

The `XMemVirtualAlloc` shim currently translates only the exact observed
startup allocation signatures. Xbox 64 KiB and 2 MiB page flags are stripped
where observed, and Xbox graphics protection bits are reduced to ordinary
Windows CPU protections. These calls therefore supply CPU-backed virtual
memory only; they do not emulate Xbox physical pages, GPU aliases, graphics
coherency, or device memory. Unknown signatures still fail explicitly.

The next renderer boundary is `D3D12XboxCreateDevice`. Returning a raw Windows
D3D12 device is unsafe: subsequent title calls use Xbox-specific device vtable
slots. A useful graphics implementation will need a compatible device proxy
and resource, queue, and presentation behavior, after initialization can reach
that path.
