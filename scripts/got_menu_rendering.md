# Ghost of Tsushima menu rendering: verified partial repair

The latest cloud-enabled baseline can be reproduced with:

```powershell
./scripts/got_menu_rendering.ps1 -ExperimentalSkyNoise -MaxSeconds 150 -CaptureScreenshots -ExtraFlag SHADPS4_DIAG_GOT_B0_DYNAMIC_IMAGES=1
```

This adds the existing experimental dynamic sky-noise descriptors and indexed SRT weights. Together with the current source's GDS write tracking, it restores textured animated clouds. The September 29 upstream comparisons reproduced that improvement; the black stepped gap and dark/incomplete foreground grass remain. Menu-entry differences from the supplied reference are expected for game version 1.0. See `Build/got-ir-audit/github-review-20260929/REVIEW.md` for the new comparisons and `CONTINUATION_20260928.md` for the previous model's cloud investigation. The default invocation below retains the older baseline without the sky-noise option.

The command also enables the B0 material-image diagnostic used by the September30
verified baseline. It is still an experimental rendering configuration. The latest
review, exact launch records and limitations are in
`Build/got-ir-audit/review-completion-20260930/RESULTS.md`; individual upstream
decisions are in that directory's `UPSTREAM_TRIAGE.md`. The complete suite command
is `scripts\test_checkout.cmd` (456 passing cases in the latest restored build).

Run `./scripts/got_menu_rendering.ps1` from this checkout. For a bounded capture:

```powershell
./scripts/got_menu_rendering.ps1 -MaxSeconds 120 -CaptureScreenshots
```

The launcher temporarily enables Precise buffer readbacks and image readbacks for CUSA11456, and enables the existing targeted shared-memory barrier experiment for compute shader `dc800181`. It restores the previous per-game profile on exit. The menu preset caps fragment loops except shaders containing the recognized entry-EXEC wave minimum sequence, which now have a translation repair on hosts with 64-lane subgroups. Other shader loops still require investigation. The obsolete guest dependency tracing and proxy-release bypass are no longer enabled: the September 30 fence interrupt repair reached responsive gameplay in three fresh launches without either. Always wait for the launcher to finish restoring settings before starting another run.

Image readbacks resolve the disappearing sword: the game uses two linear 480x270 float images, stored with pitch512, as CPU occlusion maps. Without readbacks, both actual CPU maps contain138240 zero floats. With readbacks, the same maps are populated and the sword persists. Precise buffer readbacks alone do not solve this image-transfer requirement.

The targeted compute barrier removes the large black/white bands in the tested configuration. The scene still has incorrect particle/material detail and is not a complete reference-matching menu repair. The barrier override remains experimental and specific to this shader; it has not been validated across gameplay or other GPUs.

`-ExperimentalMaterials` additionally enables the existing material-image array experiments. Their contribution is not established by moving whole-screen images, and they remain off by default. Raising the fragment-loop limit and enabling raw-buffer synchronization or full tiled-image readbacks did not yield a correct menu.

The older `got_menu_occlusion_workaround.ps1` remains available for diagnosis, but bypassing the CPU depth test is no longer necessary with image readbacks enabled.

The source also includes narrowly scoped upstream compute FP-mode, stencil-view, sparse-memory-offset, scheduler-order, and scalar carry/borrow corrections. These address independent correctness defects; they are not individually proven causes of the remaining menu artifacts. Run records, before/after source snapshots, regression checks, and screenshots are under `Build/got-ir-audit/leads-20260928/`.
