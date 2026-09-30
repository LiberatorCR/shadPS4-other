# Ghost of Tsushima: partial menu workaround

**Update:** Prefer `got_menu_rendering.ps1`. Image readbacks now restore the actual CPU depth maps and retain the sword without this occlusion bypass. See `got_menu_rendering.md` for the improved configuration and remaining limitations.

This optional workaround restores the sword that disappears after the first menu frame in the current CUSA11456 checkout. **It does not fix the black/white background corruption or establish why the depth buffers are empty.**

Run from the repository:

```powershell
.\scripts\got_menu_occlusion_workaround.ps1
```

The launcher uses the existing menu boot flags, enables `SHADPS4_DIAG_GOT_SKIP_CPU_OCCLUSION=1` only for this run, and records the executable hash and settings under `Build/got-runs`. Close the emulator normally. To capture a bounded comparison:

```powershell
.\scripts\got_menu_occlusion_workaround.ps1 -MaxSeconds 70 -CaptureScreenshots
.\scripts\got_diagnostic_launch.ps1 -Preset menu -MaxSeconds 70 -CaptureScreenshots
```

## What was established

The PS4 continued submitting the sword's three draws throughout 390 complete captured menu frames. The emulator submitted them for one frame, then stopped. Guest-code tracing located the loss before main-view draw-list emission, in the CPU depth-occlusion path. Two sampled depth maps each contained 138,240 zero floats. Bypassing that depth check retained the three sword draws for 558 consecutive frames (submissions717–1274); the control retained them only at713 and lost them afterward. The sword is visibly present in the bypass screenshots, while severe shading corruption remains.

## Scope

`src/core/signals.cpp` patches the verified instruction sequence at guest offset0x1028dae to select the game's existing no-depth-map path. Earlier frustum and distance checks still run. This is a diagnostic game-specific bypass, not a general emulation correction. It can increase rendering work and has only been tested in the main menu, not gameplay. It is disabled unless explicitly enabled alongside the existing dependency-trace boot flag. Instruction mismatch leaves the workaround unapplied. Game files and console settings are unchanged.

Detailed investigation and reproducible probe sources are in `Build/got-ir-audit/REPAIR_20260928.md` and `Build/got-ir-audit/cpu-selection-experiment-20260928`. The shader/resource cause of the remaining corruption and the producer of the empty depth maps remain unresolved.
