# Ghost of Tsushima — current checkout

This main branch contains upstream main2338a06f plus the functional changes from latest main259e815a (#5199 thread-id operand patching and #5200 ADDC/constant folding), with local menu/gameplay repairs retained. Cleanup and latest-main adaptations are included in the published main checkpoint. Final build passes, suite449/449 passes18.31s. ExecutableSHA41E3DB2030EA2C53C9C5626B82018A8A77437ECEA8B9F787E0ED913D9B874DC9 reached cloud-enabled menu and gameplay in run20261001T010023899Z-16260. This is one bounded final-binary launch. Latest cleanup evidence is Build/got-ir-audit/main-cleanup-20260930/REPORT.md. Historical source/handoff is archived in that directory's before-cleanup.zip, with a full patch alongside it.

## Reproduce

Build with the configured Build/x64-Clang-Release target. The local batch helper is Build/got-ir-audit/leads-20260928/build.cmd. Run all tests with scripts/test_checkout.cmd (449 cases after upstream's test removals/additions).

Launch the verified partial menu configuration from the repository:

```powershell
./scripts/got_menu_rendering.ps1 -ExperimentalSkyNoise -MaxSeconds 300 -CaptureScreenshots -ExtraFlag SHADPS4_DIAG_GOT_B0_DYNAMIC_IMAGES=1
```

Game is G:/ps5games/PS4/CUSA11456/eboot.bin, version1.0. Extra reference-image menu options do not apply. The wrapper temporarily enables Precise buffer/image readbacks and restores the per-game profile. The cloud and B0 descriptor options remain necessary; the default invocation omits those experimental options. Continue is the highlighted entry. Keyboard injection may fail; temporary Cross=leftbutton works with a click, with input bytes restored after the run.

## Repairs that must remain

- PM4 INT_SEL=3 confirms data writes without IRQ. Spurious IRQs prematurely released the guest's dependent consumers and caused the Continue/B8D4E0 loading crash. Mode0/1/2/3 regression tests cover writes and interrupt ordering. Compute release packets are copied and published after Vulkan completion; GDS copies are recorded before that tick.
- Generic entry-EXEC wave-minimum recognizer: strict saved-mask/CNDMASK/five-swizzle/ReadLane/scalar-min pattern, multiple reductions, actual scalar write spans and clobber rejection. Native64-lane GroupUMin avoids undefined inactive-lane reads. Fresh FS2a3cacd4 IR had two GroupUMin operations. Preserve unmatched lane reads. Recognized shaders are exempt from the diagnostic cap only when no unmatched lane reads remain. Other fragment loops still need the menu preset's cap; an earlier all-caps-off run lost the device after44s.
- Dynamic image-array descriptors, conditional resource guards, indexed SRT sky descriptors, per-component interpolation, dc800181 shared-memory barrier override, GPU-produced image readbacks, LDS byte/element addressing and memory-barrier semantics.
- Packed-byte conversion uses round-to-even, saturation, NaN-to-zero and selector masking. Actual GPU regression checks18 console vectors. Scalar carry/borrow and packed-half checks also remain.
- Sparse backing offsets are bytes, depth/stencil image references are reacquired after SlotVector growth, non-stencil textures cannot redirect through stale depth associations, IRQ contexts are stable and equeue polling is locked.
- Invalid host-module JIT/signals accesses are rejected by bounds checks; allocator/thread-library implementations remain.
- Cache versions after cleanup: binary12, metadata8. Merge changed serialized layouts. Never load old version7 metadata into this layout. Fetch prologue conditions use Empty(), not its negation, preserving pre-merge behavior.

## Cleanup

Removed approximately5,000 lines of old rasterizer captures/render substitutions, guest breakpoint tracing, PM4 submitted draw tracing and weather-buffer probes.119 diagnostic controls archived;32 remain. Core module/signals return to main's structure. Rasterizer Draw/DrawIndirect/Dispatch/OnSubmit/OnFence largely return to main; retain dynamic descriptors and compute completion. The obsolete CPU occlusion bypass and launcher were removed; image readbacks preserve the sword without patching guest instructions. No guest dependency bypass is active. Keep historical dumps in Build; do not infer findings from probes that are no longer compiled.

## Outstanding rendering work

White pampas plumes remain absent, foreground tint/luminance differs, and a stepped black horizon gap persists. Gameplay has speckled/incorrect ground materials. Cloud-enabled menu still shows textured clouds and sword. The five shader groups e3f4c691/4ca1cd7a/39bb734f/654fbaae/2074d477 had zero emulator submits versus recurrent hardware draws, but have NOT been identified as plume meshes. CPU occlusion A/B did not restore them. Pampas fluff texture opens during menu startup; missing texture files alone are unsupported.

Prior B0 MRT captures cover short grass only (y794..1079 at1920x1080). Some old all-zero RT captures were invalid because a readback finished the command buffer after binding; corrected captures showed nonzero MRTs. Mesh/material correlation is incomplete. PS4 GoldHEN FTP/logging was unreachable at last investigation; bounded console mesh probe103 was built locally, not deployed. No current console capture or deployment claimed.

## Previous external tests and limits

PR5194 conversion retained. PR5196 detiling and PR5120 raw-image synchronization were removed after isolated menu runs showed no visible improvement. The first combined raw-sync/detiling run failed but was confounded by restored source timestamps: Copy-Item preserved old mtimes and Ninja reused experimental objects. Always touch restored sources and inspect the compilation log. Relevant fork wave64 paths did not affect this native64 GPU; sparse wait-stage arrays and raw-write invalidation were already equivalent locally.

Earlier cleaned1D7E6210 completed three gameplay runs174131476Z-25496/174658147Z-2552/175258713Z-27608 (214/338/314s). Post-merge F2A53A2B completed one bounded Continue launch151s with animating gameplay. Do not assign either set to a newer executable. Latest binary/run hashes and precise validation belong in the cleanup report.