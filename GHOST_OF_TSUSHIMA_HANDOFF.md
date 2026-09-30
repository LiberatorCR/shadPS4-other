# Current status — September 30, 14:49 EDT

User redirected work to upstream/main/PR/fork testing after the extended local investigation. That testing is completed at a stable stopping point. Fresh main is unchanged at 2338a06f, 62 commits beyond checkout HEAD. Inventories cover 100 open PRs, 100 newest forks, and selected contributor branches; this is targeted review, not an exhaustive review of every fork. Full evidence: Build/got-ir-audit/upstream-tests-20260930/REPORT.md.

Retained PR5194 V_CVT_PK_U8_F32 fix: correct round-to-even, saturation, NaN handling and byte selector. Added an actual GPU regression with 18 console-reported vectors. Final suite456/456 passes, zero skips,15.86s. PR5196 tiled mip layout and PR5120 raw buffer/image synchronization were each adapted, built and tested in isolated menu runs. Neither visibly restores plumes or ground/tint, so both experiments were removed. A combined detiling/raw-sync run failed; isolated raw sync did not. Initial incremental tests were confounded by Copy-Item preserving old source timestamps; restored inputs were subsequently touched and verified to recompile. Do not interpret the combined failure as evidence against raw sync alone.

Final build passes, SHA256 704D2AC49F913D142D1D6C3251908E5AAF42D6986C7E2E69DB2313B0F9FC4F15. Detiling/raw-sync sources match their pre-test backups. Native input mappings restored exactly; per-game temporary background input removed. No emulator running. No reset, push or console deployment. The final source combination reached menu in isolated PR5194 run20260930T183854787Z-19980; this final relink has NOT completed three gameplay launches.

Continue repair remains: PM4 INT_SEL=3 confirms writes without IRQ; spurious IRQs prematurely released guest consumers. Cleaned pre-extension binary1D7E6210 completed three fresh responsive gameplay runs174131476Z-25496,174658147Z-2552,175258713Z-27608 (214/338/314s). Preserve that evidence without assigning it to later binaries.

Generic entry-EXEC wave-minimum repair now recognizes multiple strict reductions and actual scalar write spans. Fresh FS2a3cacd4 IR has TWO GroupUMin operations; other lane reads remain unchanged. Recognized reductions run uncapped on native64-lane hosts only when no unmatched lane reads remain. Other fragment loops still need the diagnostic cap; all-caps-off lost the device after44s on the earlier cleaned build. Shader cache11, persistent metadata7. White plumes, ground/horizon and lighting mismatch remain unresolved. Five absent draw groups are not identified as plume meshes. PS4 ports were unreachable at the last check; bounded console mesh probe is built locally but not deployed.

Older sections below are historical; superseded pending-test statements must not override this checkpoint.
# Current status — September 30, 13:51 EDT

Continue is repaired: PM4 INT_SEL=3 means data after write confirmation WITHOUT IRQ; extra IRQs prematurely released guest consumers. Three fresh launches confirmed responsive camera/movement for minutes on SHA256 9E7BF36A699425EE47569BD5F821A62B710A247DEB975127B42A0590B9846E05 without guest dependency patches. A subsequent generic shader repair recognizes the entry-EXEC wave minimum idiom and avoids reading inactive Vulkan lanes. Controlled A/B isolated FS014d2f46; the retained implementation uses no shader hash. Its recognized shaders run uncapped on 64-lane subgroup hosts. Other fragment loops still need a diagnostic cap: latest all-caps-off run loses the Vulkan device after44s.

Suite452/452 passes, zero skips. Nine obsolete proxy/inactive-lane/sky probes were removed with archived source. Current cleaned executable SHA256 1D7E6210ABDE14DBE244D614C1C34C45E60DC54942EBABC5130609AFBCA6661E reaches responsive gameplay; comparable final-build validations are in progress. Do not confuse earlier binaries' three-run validation with three launches of this binary. White plumes, ground/horizon and gameplay material defects remain. Neo comparison fails shader decoding before menu; no vegetation conclusion. PS4 ports21/2121/9090 remain unreachable. A bounded version103 console mesh/descriptor probe is built/tested locally, NOT deployed. Historical FS2a3cacd4 has a related reduction pattern rejected by the strict matcher; capture fresh IR and prove complex masks/lifetimes before extending it. See the latest dated sections of Build/got-ir-audit/review-completion-20260930/RESULTS.md. Earlier pending gameplay statements below are historical.


## Grass draw coverage and rejected alpha experiment — September30,09:33

Corrected diagnostic run20260930T131153162Z-17792 has nonzero MRT1/2/3 grass data.
B0 draw1 changes244010 RT2 pixels; draws3/4/5/7/8/9/12 add smaller patches. All14
B0 draws leave coverage within y794..1079 at1920x1080. Reference plume heads rise
considerably higher. Actual geometry/material correlation remains incomplete.
B0 storage run20260930T132225892Z-24828 shows VS981a33a9(firstdraw) and242b14d7
(otherdraws), NO fetch shader,8 storage descriptors. Conventional vertex attribute
probe was empty and replaced with bounded VS storage-buffer captures. Position
records stride40 with65536 records; material table base140000C000 stride224 count49.
Captures are raw GPU buffers, capped256KiB perdescriptor,16 descriptor maximum,
14 draws in one menu submit. Some index references may exceed the captured prefix;
do not treat these prefixes as complete meshes.

Temporary B0 EmitDiscard/EmitDiscardCond bypass was tested in20260930T132535541Z-11524,
then restored byte-for-byte and production rebuilt. It was a NO-OP: original saved
B0 SPIR-V and test SPIR-V both have0 demote/kill/terminate instructions. It does NOT
constitute a meaningful alpha-discard A/B or rule out other shaders' alpha handling.
Current source has NO alpha bypass. Coverage remained short grass; no plumes fixed.

Current-CPU call probe with JOB_RECORDS saw NO sceKernelGetCurrentCpu calls from
startup through a confirmed Continue/B8D4E0 failure in20260930T131735585Z-22404.
The probe was removed; process.cpp returns to original content. Do not blame its
constant0 return for this run. Static red-zone protection comparison is in progress;
not yet evaluated. Latest restored suite442/442 passed, zero skips,15.84s in
latest-restored-suite.log. Current restored binarySHA
6989341C60196FF267128C3F48EAFDDC627894D3697F8F82772B8E88F54729D8.
All five acceptance objectives remain incomplete, especially gameplay and plumes.

## Decoder fault and capture correction — September 30, 2026, 09:15

Run20260930T124237307Z-14408 failed at host RIP700000e478de, reading2b802c5a.
The release link map resolves this to ZydisDecoderDecodeInstruction+430; verified
instructions match the original executable. It is not a printf failure. TryPatchJit
used GetModule, which returns a preceding module even for addresses beyond its end.
It now requires GetContainingModule. Illegal-instruction dispatch also rejects
out-of-module addresses before inspecting bytes. No guest fault is suppressed.
Original CMAKE_EXE_LINKER_FLAGS_RELEASE=/INCREMENTAL:NO was restored after map
creation. The map lives here as host-fault.map.

Run20260930T125907419Z-3012, SHA35202073547EA11A205E332D7D3C818638E5E75969253088EB13ED9C0128E058,
reproduces the usual B8D4E0 fault on four workers, so the bounds guard is not a GoT
loading repair. At missing consumers, companionF0D8 is state2/count0, last release
152959 from102C4D9 (GPU label polling completion), F168 state2/count0 last release
153379 from7ED226 (recursive dependent release). Prior finalizer finishes152839;
next clears precede consumer starts153890..153899 and finalizer153928. This narrows
which dependencies had completed but does not identify the initiating fault.

Run20260930T130528825Z-23416 captured all14 B0 indirect argument blocks in submit720.
First draw index_count4836/instances180; later draws2040 indices and instance counts
8,168,73,4,6,43,8,32,0,27,50,2,9. These are actual GPU buffer values at draw time.
IMPORTANT: its ALL-ZERO render-target captures are INVALID diagnostic evidence.
The inherited indirect-argument readback finishes the command buffer AFTER vertex
and index bindings, losing those bindings before the subsequent draw. Diagnostic
bindings were moved after argument readbacks; repeat before inferring geometry
coverage. B0 also writes MRT1/2/3, so color0 captures alone do not establish coverage.
New bounded split capture exports per-draw RT2 plus final RT1/3 and records formats.
PS4 ports21/2121/9090/9020 still unreachable at09:05; fresh console comparison absent.

## Owner timeline and LDS fork lead — September 30, 2026, 08:40

Owner-scoped run20260930T121711843Z-21284 confirms (one owner only) record clears
112997..113062, consumer entries113560..113570, finalizer start113587, and prior
completion112668. This supports consumers overlapping reconstruction, not a final
allocation failure. Extended queue run20260930T122038976Z-25688 retains loading
queue logs: state1/count1, target4587, consumer groups queued219587..219595 after
finalizer215630 completed219478. Next reconstruction clears219826..220034;
consumer entries220258..220273 precede finalizer220314. The same GPU label4587
releases these groups. Why the next reconstruction overtakes outstanding consumers
remains unknown. No null-store bypass, fake material record or job suppression was
added. Earlier GLOBAL markers and4096-queue traces are qualified above; corrected
queue bound65536 captures the observed run. Do not claim the initiating dependency
bug is now fixed or identified.

All402 console census frames1200..1601 have B0 hashb0db526b direct0/indirect14,
RT0=1429ea0000. Emulator submitted candidates also have14 B0 draws. This does not
prove matching indirect argument/vertex counts or matching plume meshes. Five
absent direct groups remain candidates for ground/shadow/coverage, not known plumes.

Fork9825c2db's two generic LDS fixes were adapted: storage-backed LDS converts byte
offsets to16/32/64-bit element indices; workgroup barriers include UniformMemory
for the storage-backed case. Local scratch Commit already matched the fork. Added
IR regression covers load/store widths and64-bit atomic addition across workgroups.
442/442 cases pass, zero skips (lds-storage-atomic-suite.log). ShaderBinaryVersion7,
metadata7. A trial test exposed pre-existing unsupported64-bit buffer CAS lowering;
no CAS support claim is made. This is not established as a GoT-used path.

Runtime20260930T123546614Z-22792, binary
3C62BE52034AA33CB4E91F7434F0163FD0E30ECFDAE9C07BEA5E0E1D0E574457,
visibly reached the white loading emblem and reproduced B8D4E0 on four workers
WITHOUT job-record probes. Shared-storage diagnostic emitted no binding in this run;
AMD enables explicit workgroup layout. No plume/horizon improvement. Current menu
still depends on the documented shader-specific experimental flags and readback
profile; all five acceptance objectives remain incomplete.

## Trace qualification and interrupt repair — September 30, 2026, 08:18

441/441 tests now pass, zero skips (irq-context-suite.log). IrqController's shared
unordered_map was accessed through try_emplace on concurrent registration/signal
threads without a map lock. Bounded IRQ IDs now use a preallocated65-context array;
per-context subscription locks remain. The new test exercises8 concurrent channels,
persistent callbacks, one-shot callbacks and unregister. The runtime still crashes
in20260930T121238890Z-25800 after visibly confirming Continue; this repair is not a
proven GoT loading solution.

IMPORTANT qualification: initial finalizer start/end counters were GLOBAL across
several scene owners. Runs20260930T115851818Z-7088 and20260930T120224860Z-25276 show
consumer entry after clears and before a finalizer start, but do NOT establish a
phase-perfect finalizer completion timeline for one owner. Other owners can replace
the global markers. Do not promote that inference as definitive root cause. Corrected
markers track only owner=eboot_base+3BFDED0-F120 (the crashing proxy).

The first consumer queue trace at7ED260 logged every proxy and exhausted4096 entries
at the menu, so it is not usable for the loading queue transition. Replacement trace
at7ED2D7 observes the dependency state after its guest mutex lock, faithfully emulates
MOV ECX,[RBX], and filters groups with functionB8D170 and the crashing owner obtained
from the argument's first pointer. It logs dependency count/state, target and bounded
phase sequence. No guest jobs are skipped or fabricated. Current owner-scoped run is
in progress; evaluate it before claiming the consumers release prematurely.

Cropped-alias fork07488693 requires prior contents_version/CopySubrect/IsSubrectOf
infrastructure absent locally. Do not blindly cherry-pick; read its dependencies and
prove relevant aliases before adapting. No cropped-alias source change was made.
## New verified evidence — September 30, 2026, 08:00 local

The assignment remains incomplete. The latest full-suite run is flat-ud-suite.log:
440/440 tests pass, zero skips. The two additional tests exercise production equeue
polling with concurrent consumers and one-shot re-registration. Removing only the
new poll mutex reproduced 2355 deliveries for 2000 triggers and 73 invalid payload
observations; the restored mutex passes. This is a verified race repair, but Continue
still fails. Graphics EOP/EOS asynchronous and synchronous experiments were rejected;
the original graphics signaling blocks are restored. Compute timeline signaling remains.

Upstream 8c91be2b / PR5181 flat user data is now adapted, not deferred. User registers
and ReadConst use the flat uniform buffer; auxiliary PushData offsets move to byte16.
The test runner binds flat UD at binding1 and zeroes the auxiliary push block. Shader
binary version6 / metadata7 invalidate old cache layouts, preserving the local guard
schema. Binary 27E4308949700B41296F00679CB1B19BC0287241BA7A17AFA369F7CE6027398C
renders the same partial menu. Run 20260930T115540692Z-10812 visibly faded from
Continue and reproduced B8D4E0 on four workers, exit -1073741819. Flat UD is not a GoT
loading fix. Run 20260930T114509604Z-6932 timed out at the menu, not gameplay.

Submitted census run 20260930T112221776Z-14924 scans nested buffers before SubmitGfx
queues them. Roots8978..9578 span200 B0 frames: zero unknown, malformed and unsupported
state loads. Five console candidate hashes have zero submitted candidate packets,
even including conditional regions. Evidence: submitted-menu-census.json. The scanner
reads IB contents present at submission; GPU-generated later contents and conditional
state paths remain limitations. This narrows the boundary upstream of rasterizer but
does not identify the objects or prove complete console phase matching. Initial census
run 20260930T112010795Z-7812 hit recursion16 and is invalid for absence claims; fixed
bound256 traverses the observed49 chained IBs. Default-off flag is
SHADPS4_DIAG_GOT_SUBMITTED_DRAW_CENSUS.

absent-shader-assets.json locates all five shaders in packs opened before the menu.
Actual GCN decoding (absent-shaders-decoded.txt): e3f4c691 and2074d477 are128-byte
alpha-discard shaders exporting only null/coverage;4ca1cd7a and39bb734f export MRT0..3;
654fbaae is10KB, samples many images and exports depth and MRT0..4. No mesh/material
identity is established. White plumes must not be attributed to these hashes without
correlation. Obsolete unconditional B0 table hashes and mislabeled sampler-tail dump
were removed; preserved in removed-grass-table-probes.txt.

Consumer phase run20260930T113827523Z-8616 reports rebuild_workers=0,entries=1022
while record histories were created/visited before their latest clear. Zero worker
count does not prove the final worker finished: B89E40 decrements F1B0 before the last
worker's record-building loop. New default-off MOV trace markers at B8ACF3 and B8CB87
compare finalizer start/completion against consumer histories, without a null bypass.
The PS4 at192.168.0.27 was unreachable on FTP/log ports; a user question to bring it
online is pending. Temporary input mapping remains in place for ongoing UI tests and
must be restored from input-before.ini before final delivery. Three final gameplay
launches, perfect plumes, ground/horizon and final dead-probe cleanup remain outstanding.

**Active repair checkpoint — September 30, 2026, 06:45 local**

Read `Build/got-ir-audit/review-completion-20260930/RESULTS.md` before the older sections.
This assignment remains incomplete: plumes, horizon/ground and responsive gameplay
have not met acceptance. The incoming executable/patch, GitHub snapshots, build
logs and run evidence are preserved in that directory. `cmd /c scripts\test_checkout.cmd`
runs all four suites; all 438 cases passed with zero skips in
`all-tests-final-scaffolding.log`. The bitcmp U64 conversion and ISA half-precision
Inv2Pi constant are repaired. New GPU regressions cover BFM and ALIGN edges;
ancillary lowering preserves multiple extracts. Diagnostic settings are now owned,
thread-safe cached launch settings (267 original lookups, 152 original names).

The prior agent's broad unsupported-format descriptor rejection was removed in
favor of conservative scalar descriptor-use dominance. Applicable upstream image,
sampler, SampleId, BFM, ancillary and page-probe changes were adapted while retaining
per-component interpolation and dynamic material images. Current upstream/main is
3f21ee0b (61 commits ahead), not the historical 51. See the triage table for decisions.

Continue now reproduces a guest worker crash at B8D4E0, writing through r9=0 to
address 0x168. Validated MOV traces prove the CPU object table's records were
created, then cleared during guest table reconstruction at B8AA66 before consumers
accessed them. Run `20260930T103215935Z-27320` records this. The fork-derived compute
RELEASE_MEM completion ordering repair also reproduces the same crash in
`20260930T103633335Z-17912`; it is not sufficient to fix GoT. The next trace compares
producer-entry visits, record creation and clear sequence for the exact failing
indices, and logs their visibility values. Do not bypass the null store.

Temporary keyboard/mouse mappings and background-input settings are diagnostic
only and must be restored. Computer-use short taps can be missed by latest-state
pad polling. A mapped left-button drag or repeated observed clicks has successfully
entered loading. Default-off job-record probes have timing overhead and must be
disabled for final validation. The earlier long menu-only run is not gameplay evidence.

**Historical investigation — September 30, 2026: Continue crash, no source changes**

The user temporarily prioritized diagnosing the gameplay-loading crash over white plumes. Read `Build/got-ir-audit/continue-crash-20260930/RESULTS.md`. Three launches of the unchanged `11BF60CB...` executable hit `liverpool_to_vk.cpp:791 SurfaceFormat`: `Unknown data_format=16 and num_format=1` (5:6:5 SNORM). The error occurs on the GPU command processor after CS `0x9716bed2` compilation. Complete logs show the assertion before secondary shutdown exceptions; do not diagnose from only the final log lines. The second run visibly reached black loading after Continue. Working fix difficulty: **medium for this blocker**, not a promise of fully working gameplay.

Strongest unproven lead: CS9716 has four conditional downsample outputs, a count from SRT dword 0, and eager descriptor fetches through dword 40. Implausible descriptors from this shader suggest inactive output slots may be bound as real textures. Next record the exact failing descriptor/caller, shader, output index, raw words, SRT count and fetch offset; prove whether that output is reachable before changing binding or format behavior. No arbitrary SNORM-to-UNORM fallback was added. Emulator source/executable unchanged; original input config restored byte-for-byte. No emulator remains running. The attempted external minidump failed and captured only secondary shutdown breakpoints; its empty dump is not evidence of the original fault.

**Newest confirmed fix — September 30, 2026: interpolation must be tracked per component**

Read `Build/got-ir-audit/integer-interface-20260930/RESULTS.md` first. The directory name reflects an abandoned initial experiment. The retained repair is generic **per-component interpolation**, not a hash-specific integer bridge. FS b0db526b mixes interpolated Param3.x with raw material-ID Param3.y; a single vector-wide interpolation record let P1/P2 for x overwrite MOV's per-vertex mode for y. The backend now splits mixed inputs into scalar Location/Component declarations while retaining vectors for uniform inputs. Normal guest bits survive, and near/distant grass stays green. The old `SHADPS4_DIAG_GOT_GRASS_ID_TRANSPORT` bridge was removed. No RuntimeInfo/pipeline-cache changes from this turn remain.

Reproduce: `.\scripts\got_menu_rendering.ps1 -ExperimentalSkyNoise -MaxSeconds 155 -CaptureScreenshots -ExtraFlag 'SHADPS4_DIAG_GOT_B0_DYNAMIC_IMAGES=1'`. The material-image flag is independently required. Ten targeted tests pass, including mixed MOV/P2 component interpolation in both instruction orders on AMD and KHR generated SPIR-V. Two component-fix menu runs completed normally; final binary SHA256 `11BF60CB369BF0CC95745C19662C3EB610B5971DBA30BC3D6667DF906811A778`. `final-menu.png` in the new report directory is a normal-occlusion final-binary screenshot. White plumes and residual horizon gaps remain unresolved.

New plume lead: five direct-draw shader groups present on the real PS4 are absent across all200 sampled emulator submits: e3f4c691, 4ca1cd7a, 39bb734f, 654fbaae, 2074d477. A controlled temporary CPU-depth-occlusion bypass also leaves them absent across200 submits. Their rendered objects are not identified yet; do not claim these are proven plume shaders. Pursue guest draw construction/earlier visibility or mesh selection, checking correspondence with the console capture before introducing a workaround. Read the report for exact evidence and limits. The old sampler interpretation below remains retracted.
**Newest reviewed result — September 30, 2026: grass material-ID transport**

Read `Build/got-ir-audit/grass-review-20260930/RESULTS.md` first. The sampler diagnosis immediately below is **retracted**: record offsets 16/48/80 are image-descriptor tails, not samplers. Guest samplers come from SRT dwords 63–66. Source/IR inspection and a separate read-only GLM5.3 review agree. The incorrect table-derived sampler override and forced unnormalized coordinates were removed; the incoming agent's source and handoff are preserved under the new report's `before/` directory.

New GPU readbacks show gray/green texture pixels but solid magenta grass output. A late raw material-index probe yields byte 0 = zero across 295,831 covered pixels; screenshot luminance cannot substantiate the prior claim of IDs 23–37. A default-off transport experiment carries VS `0x981a33a9` and `0x242b14d7` Param3.y numerically rather than as float-subnormal integer bits, then reconstructs the bits in FS `0xb0db526b`. It restores green grass across near and distant regions without replacing colors or forcing IDs. The tall white plumes and residual gaps remain unresolved. This is a demonstrated partial improvement and a concrete new investigation point, not a generic completed fix. Read the report for current run identities, validation, remaining investigation, and experiment limitations.

**Superseded claim — September 30, 2026: grass material-table sampler fix**

Read `Build/got-ir-audit/grass-sampler-20260930/RESULTS.md` first. The FS `0xb0db526b` material table V# (ud0 dword 59) is valid (49 records x 224 bytes at 0x1400f0c000, written once); its image side was already correct. The real defect was the sampler half: the statically flattened fallback S# (linear, normalized, LOD 0-4095) contradicts the table's per-record S#s (point, unnormalized, LOD clamped to 0). A flag-gated bind-time sampler derived from the table plus forced unnormalized coordinate normalization raised foreground grass band luminance from 30-66 to 84-158 (reference 100-121), reproduced across three fresh launches with no failures, with the no-flag baseline unchanged. Remaining: magenta grass tints where the reference is green (per-blade UV/entry selection or the GPU-side bake of the BC1 tint strips), a boot-lottery where the B0 matcher misses (miss-logger `SHADPS4_DIAG_GOT_B0_TRACE_MISS` is in place), and eventual graduation of the hash-gated experimental path.

**Newest result — September 29, 2026: indexed GDS addressing fix**

Read `Build/got-ir-audit/foliage-20260929/RESULTS.md` first. Ordinary GDS reads/writes/atomics ignored the M0 byte base, so grass generation and its counter reader used counter zero while the game reset different counters. The correction in `frontend/translate/data_share.cpp` changes a measured first-batch limit from over five million records to 9,511, restores additional foliage coverage, and has three new regression tests. The grass/material appearance still does not match the reference; this is a confirmed partial repair, not final acceptance. The new investigation's temporary rasterizer probes were archived and removed, preserving all earlier work. The report records current executable/run identities and validation.

**Older stopping checkpoint — September 28, 2026, 17:53 UTC**

The user's latest instruction was to stop at a *great* handoff point if no complete fix was close. This is an evidence-backed stopping point, **not** acceptance: the sky and sword are substantially improved, but the stepped black ground gap, dark materials, and particle behavior are not fully repaired or validated. There is no active emulator process. Preserve the dirty checkout and ignored `Build/` evidence. The current executable includes default-off diagnostic probes and is not a cleaned validation binary. No final three-launch, two-minute-each verification has occurred.

**New ground-pass result:** The 14 indirect draws using FS `0xb0db526b` are active in some menu frames. The first B0 MRT probe was accidentally gated behind a separate draw-trace flag, so run `Build/got-runs/20260928T174103530Z-8044` measured nothing. After fixing that gate, run `Build/got-runs/20260928T174510209Z-23252` (135 s, exit 0, SHA256 `553DB3EB8AC0DC5268A57E41AA7C4A332A37A4B34812F2B11F154AFF95477529`) captured submit703: all four G-buffer targets were zero before B0; after its 14 draws, RT0 had 253,825 nonzero 32-bit words and RT1/2/3 also became populated. A second run `Build/got-runs/20260928T174950468Z-28940` (130 s, exit 0, executable SHA256 `952202CBE15126A85659DCCA954BBC482600F46FA9F017B871523CB328AAB201`) captured submit718: 251,622 RT0 nonzero words and populated RT1/2/3. The emitted RT0 PPM has RGB zero, but its alpha mask `b0-s718-split-after14.ppm.alpha.pgm.png` in that run shows a dense sloped **foreground grass** region from y791 to the bottom. Gap point x300/y870 and foreground test point x300/y900 both have zero in this B0 alpha mask. The historical all-zero B0 sample at submit726 is phase-dependent and cannot establish that these draws are broken generally. Do not force B0 albedo or assume its zero RGB is the cause without tracing deferred interpretation.

**What is established about the gap:** An explicit sky-LUT6 output probe plus no depth/stencil attachment and a white preclear showed that the sky shader actively writes zero at sampled pixels below the horizon (x100/y900, x300/y900, x100/y1000), while x900/y900 is bright. The full-screen triangle and target storage are sound. A depth-point probe after the first FS `0x141a6d03` depth batch and all 19 FS `0x2a3cacd4` direct scene draws found zero main-view depth at gap point x300/y870, with nonzero depth at nearby foreground x300/y900. B0 is confirmed to draw foreground grass in another frame. These results suggest missing/dark **distant ground coverage or lighting**, but are not yet a causal root fix. The next controlled experiment should capture same-submit color/depth masks for the direct ground passes before and after each pass, and compare the precise black-step boundary with the PS4 draw/material/descriptor state. Avoid trying to fill the region with arbitrary sky color. Detailed experiments are in `Build/got-ir-audit/CONTINUATION_20260928.md` and run logs.

**LUT coverage check after this checkpoint:** `Build/got-runs/20260928T175616802Z-6996` (125 s, exit 0, executable SHA256 `952202CBE15126A85659DCCA954BBC482600F46FA9F017B871523CB328AAB201`) logged both LUT6 writers `0x0f16a579` and `0x9c3cb720` with direct dispatch `1/32/16` workgroups and `4/4/4` local threads, no partial groups. The latter shader IR writes image19 with x/y derived from group y/z and loops x in steps of four from group x, covering the full 128x64x64 volume. An earlier raw LUT6 z32 slice has a stepped zero cutoff matching the screen shape; zero voxel count grows from 0 at z0/8 to 1,164 at z48..63. Thus a *simple under-dispatch* does not explain the zero region. It may be computed from the sky model or another shader/input fault. Do not assume the zero LUT values are wrong without checking the producer computation and intended PS4 coverage.

**Latest continuation checkpoint — September 28, 2026, after cloud investigation**

The section below supersedes this document's older "current result" and "next work" sections. The full experiment history, including rejected hypotheses and run IDs, is in `Build/got-ir-audit/CONTINUATION_20260928.md`. Preserve the extensive pre-existing dirty work; do not reset or clean the repository. The user supplied the actual menu reference at `C:\Users\Lintwer\AppData\Local\Temp\codex-clipboard-408ae217-158b-466b-8662-c9f94a0dc3ad.png` (bright continuous grass, small soft particles, textured storm sky, sword and UI).

**Best normal visual result so far:** `Build/got-runs/20260928T162032208Z-25840/`, executable SHA256 `B88766785061533A27C4F662DC5ECC110C478A9990793C70A2175BACE190D78B`, config SHA256 `9AD17510321B4CAD2CA7361F30F592D5B338B344525B4AC21C72C0B12901D439`, 155 s run, time-limit exit 0. Late screenshot `CUSA11456_20260928_122252_703_game_000010.png` and `contact-late.png` show textured animated clouds and no obvious large square blobs in six sampled late frames. The sword/UI remain. The hard black stepped gap above foreground grass and overly dark grass remain; this is not acceptable final rendering. Reproduce with `./scripts/got_menu_rendering.ps1 -MaxSeconds 155 -ExtraFlag 'SHADPS4_DIAG_GOT_DYNAMIC_SKY_NOISE_IMAGES=1','SHADPS4_DIAG_GOT_SYNC_WEATHER_RAW_IMAGE=1'` (use `-CaptureScreenshots` or screenshot flags for image evidence). The launcher sets Precise+linear image readbacks and targeted dc80 barrier, with normal CPU occlusion. A controlled run `...T163121727Z-16084` shows the broad clouds with dynamic sky noise enabled even without weather sync. GDS fix alone (`...T162513469Z-26448`) and GDS plus weather sync alone (`...T162818286Z-18672`) retain flat sky.

**Confirmed new synchronization fix:** `src/video_core/renderer_vulkan/vk_rasterizer.cpp` now adds the special `GdsBuffer` binding to `bound_buffers` so `ResetBindings` records shader writes. Previously CS `0x8c5d11bd` GDS counter writes could be invisible to the later copy into indirect arguments for sky-tile CS `0xb7458b04`/`0xfabd68f2`. This is a general tracking error, independent of the experimental sky flags. `SHADPS4_DIAG_GOT_DYNAMIC_SKY_NOISE_IMAGES=1` currently binds three runtime 64-cubed noise inputs and unrolls Phi-indexed SRT weights for CS `0x3c2e229a`/`0x11d5a4b7`; their outputs change from all zero to populated. This is a causally supported *experimental* path, still default-off. Weather image-to-buffer sync is also default-off and repairs an observed source/destination data-flow issue but is not required for the broad-cloud change.

**Latest gap localization:** The hard black stair-step is already visible immediately after FS `0x167bdbe8` sky draw. Same-submit depth capture at submit700 (`Build/x64-Clang-Release/scene-s700-before-sky-depth.raw`) has depth zero at x300,y850/870 inside the gap, and nonzero at y900 where grass begins. Disabling sky depth/stencil for diagnosis overpainted the sword and grass but left the black gap. Bypassing CPU occlusion for diagnosis also left it unchanged. **Correction to the earlier white-sky interpretation:** the sample-output probe used in run `...T165256308Z-23328` was accidentally inserted in the implicit-LOD operation, while FS167 uses explicit-LOD samples. Its near-white output was therefore not reliable evidence about LUT6. The probe has now been moved to `EmitImageSampleExplicitLod`, with a compile log proving activation. Corrected combined sample-output/no-depth run `Build/got-runs/20260928T170407069Z-21012` (130 s, exit0, SHA256 `72061E2F7C59F0572158DC5E08BEE79CCE3A1629E0B4D065A72FE742B7D05FF4`) captured submit726 and **still has the same hard black steps** despite white/near-white sky output and sword/grass overpaint. See `Build/got-ir-audit/s726-explicit-lut6-nodepth.png`. This strongly points to missing sky fragments or a target write mask/alias effect, not LUT6's dark values, depth or stencil.

**Draw-state result:** `Build/got-runs/20260928T165630325Z-10084` (130 s, exit0, SHA256 `5C98A5E4D8C925D77969AFC42701084B5378680BF8DB5A51F9447BA9B313953C`) logged FS167 once each submit724..755. It draws one nonindexed three-vertex triangle, one instance, VS `0xbc086a4d`, screen/window scissor `(0,0)-(1920,1080)`, generic scissor `(0,0)-(16384,16384)`, viewport scissor disabled, blend disabled, RGBA target mask `0xf`, ROP `0xcc`. Thus a narrow scissor, alpha blending, or multiple tiled sky draws cannot explain the black steps. A combined white-sky-output/no-depth run `...T165921501Z-19256` (130 s, exit0) had both diagnostic flags in `run.json`, but its submit700 capture showed normal shaded sky (max RGB0.6665) rather than white diagnostic output, so it **does not test** the combined hypothesis. Confirm diagnostic shader activation at the captured submit before inferring coverage; inspect shader variant/cache behavior if needed. No run remains active.

**New ground lead (supersedes earlier sky-coverage inference):** The sky VS buffer contains the proper full-screen positions `(3,-1,0,1)`, `(-1,3,0,1)`, `(-1,-1,0,1)` (`...T171247652Z-1148`). A full-image clear after sky draw fills all 2,073,600 pixels on the same HDR target (`...T171602570Z-23488`), so storage/readback is sound. A white in-rendering preclear just before normal sky draw (`...T172431191Z-28800`) is retained in much of the foreground silhouette, confirming the normal sky draw is masked there. With depth/stencil attachment omitted, explicit image6 sample output, and the same preclear (`...T172729505Z-25180`), the sky draw overwrites the preclear with sampled **zero** at x100/y900, x300/y900 and x100/y1000; x900/y900 samples `(7.8555,7.1055,7.168,1)`. The sky lookup's zero region is real, but it may be correct below the horizon. The exposed black gap points to **missing terrain/grass coverage** there. Do not force sky color into that region as a repair. Locate the missing ground draw/visibility/material coverage using same-frame depth and G-buffer masks, then compare the real PS4 draw census. The extensive `Build/got-ir-audit/CONTINUATION_20260928.md` has exact run IDs and caveats. Preclear, clear, sample output, and no-depth options are diagnostic only.

**Cleanup and validation still required:** The source contains many default-off readback, screenshot, descriptor, SPIR-V sample/UV, and draw probes from this investigation; the current executable is *not* a cleaned final build. Remove unsuccessful temporary probes while preserving earlier dirty work and justified fixes, build again, test, and verify three fresh launches for at least two minutes after menu arrival with normal animation/navigation and no crash. The historical GCN run had 52 pass/3 pre-existing failures (`pk_add_f16_2`, `pk_add_f16_5`, `bitcmp1_b64_bit32`); rerun relevant tests after cleanup. No final three-launch verification has occurred. The crash is historically intermittent, so one clean exit does not settle it.

**Ghost of Tsushima rendering investigation — continuation handoff, September 28, 2026**

**Read the newer continuation first:** `Build/got-ir-audit/CONTINUATION_20260928.md` records the subsequent GDS write-tracking and dynamic sky-noise breakthroughs that restored textured animated clouds. `Build/got-ir-audit/github-review-20260929/REVIEW.md` records the September 29 upstream review, imported corrections, and controlled comparisons. Those documents supersede this original handoff's executable identity and remaining-cloud diagnosis. Reproduce the newer baseline with `scripts/got_menu_rendering.ps1 -ExperimentalSkyNoise -MaxSeconds 150 -CaptureScreenshots`. The black stepped gap and foliage mismatch remain unresolved; compare scene rendering against the supplied reference while ignoring menu-entry differences for game version 1.0.

Continue the existing investigation from this working tree. The user wants the main menu to render correctly, has authorized local fixes, builds, tests, and investigation, and values practical progress without repeated confirmation. The latest request is to preserve everything needed for another agent to resume seamlessly. This document records the state after the latest experiments; it does not authorize unrelated destructive actions or publishing.

**Current result and the breakthrough**

The menu now shows a persistent textured sword, readable UI, coherent gray sky, and some ground detail. The severe black-and-white bands are gone in the tested configuration. It remains a partial repair: square particles, black ground areas, and incorrect/incomplete scene materials remain. Do not describe it as a fully correct menu or validated gameplay.

Two changes together produced the major improvement:

1. Enable **linear image readbacks**. The game reads two GPU-produced linear float images on the CPU for occlusion. With image readbacks disabled, the actual maps consumed by that test were entirely zero, and the game stopped submitting the main-view sword draws. Enabling image readbacks populates those maps and restores the sword through the normal visibility path. Precise ordinary buffer readbacks alone do not fix this.
2. Enable the existing targeted shared-memory barrier experiment for compute shader **0xdc800181**, using `SHADPS4_DIAG_GOT_FORCE_DC80_LDS_BARRIERS=1`. In combination with image readbacks, this removes the severe bands. Earlier tests of this barrier with readbacks disabled produced poor images and understated its usefulness. The barrier is still a shader-specific workaround, not a proven general synchronization repair.

The CPU occlusion bypass is **not needed or enabled** in the current launcher. The strongest new diagnosis is specifically the missing-sword cause; the remaining material/particle defects do not yet have a definitive root cause.

Latest verified image: [final-menu.png](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/Build/got-ir-audit/leads-20260928/final-menu.png).

**Workspace and preservation**

- Repository: `C:\Users\Lintwer\Desktop\shadps4\emu\shadPS4`.
- Branch: `Getting-GoT-to-main-menu-via-fragment-loop-cap-and-dependency-guard`.
- HEAD: `76906160e3a64324664724d5eac36bb5774e1cfb`.
- Origin: https://github.com/LiberatorCR/shadPS4-other.git . Upstream: https://github.com/shadps4-emu/shadPS4.git .
- Upstream main fetched during the review: `e868daca088f5ae6f171f3dfca0ac9dfc2df4602`, 42 commits newer than local HEAD. This is a research snapshot, not a claim about future remote state.
- The checkout was extensively dirty before the latest work and remains dirty. About 35 tracked source/test files are modified, plus untracked launcher files and `externals/MoltenVK/` and `externals/dear_imgui/`. Preserve all of this. A clean checkout of HEAD will not reproduce the current result.
- No commits, pushes, new PRs, or upstream merge were performed during the latest experiments. Do not reset, clean, blindly cherry-pick entire experimental PRs, or apply saved patches on top of changes already present.
- `Build/got-ir-audit/leads-20260928/initial-worktree.patch` records the tracked dirty delta before the latest experiments. `changes-this-turn.patch` records the incremental source changes from those experiments. These are not complete backups of all untracked files.
- Most investigation evidence is under ignored `Build/`. It will not survive a fresh clone unless explicitly copied. Keep this checkout and its artifacts.

**Reproduce the best verified configuration first**

Use [got_menu_rendering.ps1](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/scripts/got_menu_rendering.ps1). From PowerShell:

```powershell
Set-Location 'C:\Users\Lintwer\Desktop\shadps4\emu\shadPS4'
.\scripts\got_menu_rendering.ps1 -ValidateOnly
.\scripts\got_menu_rendering.ps1 -MaxSeconds 120 -CaptureScreenshots
```

Omit `-MaxSeconds` for an interactive run until the emulator closes. Close an existing emulator before starting another. The launcher uses:

- Game: `G:\ps5games\PS4\CUSA11456\eboot.bin`.
- Executable: `C:\Users\Lintwer\Desktop\shadps4\emu\shadPS4\Build\x64-Clang-Release\shadps4.exe`.
- Temporary per-game profile: `C:\Users\Lintwer\AppData\Roaming\shadPS4\custom_configs\CUSA11456.json`.
- GPU profile properties: `readbacks_mode=2` (Precise) and `readback_linear_images_enabled=true`.
- `SHADPS4_DIAG_GOT_FORCE_DC80_LDS_BARRIERS=1`.
- Existing menu-boot flags: `SHADPS4_DIAG_GOT_DEP_TRACE=1`, `SHADPS4_DIAG_GOT_GUARD_PROXY_RELEASE=1`, `SHADPS4_DIAG_CAP_ALL_FS_LOOPS=1`.
- Screenshot switch adds `SHADPS4_DIAG_AUTO_SCREENSHOT=1`.

The wrapper calls [got_diagnostic_launch.ps1](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/scripts/got_diagnostic_launch.ps1), which isolates diagnostic environment variables and records executable hash, flags, effective settings/logs, screenshots, Git state, exit status, and observed failure in a timestamped `Build/got-runs/` directory. Use this infrastructure for comparable experiments.

The rendering wrapper preserves the original per-game profile bytes and restores them in `finally`, or removes the temporary file if none existed. Normal global settings are not deliberately changed. Cleanup was verified after both a crash and a successful run. At the latest completed checkpoint, the per-game file was absent, no emulator remained running, and global `config.json` was unchanged with SHA256 `50B6317DEE6F0F900EF2E855597B064A6F57D5BF79344C399E2BCE63635802D3`.

`-ExperimentalMaterials` additionally enables `SHADPS4_DIAG_GOT_DYNAMIC_IMAGE_ARRAY`, `SHADPS4_DIAG_GOT_DYNAMIC_SCENE_IMAGES`, and `SHADPS4_DIAG_GOT_DYNAMIC_SECONDARY_IMAGE`. These are off by default; they have not established a complete visual fix. Do not silently add them to the baseline.

The older `scripts/got_menu_occlusion_workaround.ps1` and default-off source bypass remain for diagnosis. Prefer the new rendering launcher. Setting diagnostic environment variables to the string `0` may still enable paths that use `getenv` existence checks; remove variables instead. The diagnostic launcher handles environment isolation.

**Build, hardware, and tests**

Environment is Windows/PowerShell. Tested GPU is Radeon RX 7800 XT, driver `32.0.31041.1004`; logs report physical subgroup size 64. The machine also has integrated AMD graphics. No other GPU or gameplay validation was performed.

Final verified executable SHA256: `9450C7363386024BDD51F4C08C94AB4CAB95A3EEF6BA5CF87DAFD16512570FDF`.

Build with the saved command file:

```powershell
& 'C:\Users\Lintwer\Desktop\shadps4\emu\shadPS4\Build\got-ir-audit\leads-20260928\build.cmd'
```

It loads `C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat` for x64, then invokes `C:\Program Files\CMake\bin\cmake.exe --build` on `Build/x64-Clang-Release`, target `shadps4`, parallelism 6. LLVM is under `C:\Program Files\LLVM\bin`. Do not link while the emulator is running: Windows locks the executable. If restoring old source backups, ensure their timestamps trigger a rebuild.

Latest validation:

- Final release build succeeded (`leads-20260928/build-final.log`).
- Five carry/borrow GPU tests passed, including three newly imported regression cases. Build helper: `leads-20260928/build-gcn.cmd`; test executable: `Build/review-tests/tests/shadps4_gcn_test.exe`, argument `--gtest_filter=*addc*:*subb*`; results in `gcn-carry-tests.log`.
- `test-compute-registers.cmd`: compiles against actual `regs_shader.h`; all 256 floating-point mode combinations and RSRC2 scratch/userdata/TGID/LDS field checks pass.
- `test-backing.cmd`: extracts actual Backing methods and uses the real IntervalList with an integer stand-in for the Vulkan handle. Old code fails split offset (524290 versus expected 655360); corrected code passes split, replacement, and remerge.
- `git diff --check` passed, with Windows line-ending warnings only.
- The complete GPU suite was not rerun. Do not claim all tests pass.

**Evidence establishing the missing-sword mechanism**

The investigation first separated missing geometry from bad shading. On real PS4 hardware, corrected PM4 census logging showed repeated sword submissions. In the emulator without readbacks, sword commands appeared once and then disappeared already at the GNM submission boundary. Thus those missing draws were not being dropped later by Vulkan presentation.

CPU investigation localized rejection to guest helper `+0x1028bf0`, with optional depth-map structure loaded at `+0x1028dae`. Temporarily selecting the existing no-map path preserved frustum/distance tests and restored repeated sword submissions. This established CPU depth occlusion as the rejection mechanism, but was initially only a bypass.

The latest experiments identified the actual maps and their producer-to-CPU transfer:

- Texture download probes identified **linear 480x270, pitch 512, 32-bit float images**, observed at `0x110accb000` and `0x110ae1fb00`.
- A signature-checked guest hook read the actual current CPU structure, rather than assuming a fixed allocation. Encoded dimensions were `0x10d000001df`; stride was `0x80` four-float groups.
- With image readbacks enabled, maps contained populated values: only 4325/4320 zero floats out of 138240, no NaNs, maximum around 1e7. Counts include row padding; small negative padding values are not proof of bad depth.
- On the same corrected build with Precise buffer readbacks but image readbacks disabled, both maps returned to **138240 zero floats**, min/max zero, and the sword disappeared.
- Earlier ordinary buffer-binding/copy probes found no writer because these allocations were image-backed. Those negative probes did not disprove GPU feedback.
- Historical heap addresses are observations, not stable identifiers for a production fix.

The default-off CPU bypass in `src/core/signals.cpp` remains exactly as it was before the latest experiments. Temporary depth-map probes were removed; their sources are archived. Current success does not depend on those probes or ignored headers.

**Source corrections retained from GitHub leads**

These fixes are in the dirty working tree, not separate local commits. None alone yielded a correct menu; the strongest visible gains came from the readback settings plus the pre-existing targeted barrier.

| Upstream lead | Local change and files |
| --- | --- |
| PR #5126, merge `d96278e1e26d2c2af56239e3ad1605955aad0d5d` | Compute FP round/denorm mode decoding and runtime assignment. `src/video_core/amdgpu/regs_shader.h`, `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp`. Preserve RSRC2 bit layout. |
| PR #5118 | Only redirect stencil-compatible views to associated depth images. `src/video_core/renderer_vulkan/vk_rasterizer.cpp`. Probe confirmed GoT exercised the wrong redirect for R32Sfloat, format 100, observed at `0x14278b0000`; probe removed, guard retained. |
| Isolated part of PR #5134 | Sparse interval distances are blocks while offsets are bytes. Backing stores `block_shift`, scales split/merge offsets, and checks equal shifts before merging. `src/video_core/buffer_cache/buffer_cache.h/.cpp`. The broad PR bundle was not imported. |
| PR #5113, commit prefix `41f2a428` | Invoke submission callbacks before EndSession ends the command buffer, allowing runtime.FlushBarriers to record legally. `src/video_core/renderer_vulkan/vk_scheduler.cpp`. |
| PR #5030/#5031, commit prefixes `282e5c79`/`e518a651` | S_ADDC_U32 carry and S_SUBB_U32 borrow behavior. `src/shader_recompiler/frontend/translate/scalar_alu.cpp`, plus three cases in `tests/gcn/test_gcn_instructions.cpp`. |

The targeted barrier lives in `src/shader_recompiler/ir/passes/shared_memory_barrier_pass.cpp`. Its override applies only to hash `0xdc800181` with shared memory and a 64-thread workgroup. The pre-existing pass also avoids placing barriers inside potentially divergent loops. The normal AMD profile does not request this pass; establish why it is needed before generalizing.

Raw-image synchronization experiments, full tiled-image readbacks, temporary stencil/CPU probes, and the all-compute barrier override were removed from active source. `src/core/signals.cpp` and `src/video_core/texture_cache/texture_cache.cpp` were restored exactly to before-this-experiment content, preserving earlier work. Probe copies remain in `leads-20260928/probes/`.

**Latest experiments and what not to repeat blindly**

Seventeen emulator launches covered settings, isolated upstream fixes, and final delivery validation. Full records are in [RESULTS.md](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/Build/got-ir-audit/leads-20260928/RESULTS.md).

| Experiment | Outcome |
| --- | --- |
| Precise buffer readbacks alone | Sword absent; severe bands remain. |
| Image readbacks alone, then Precise plus images | Sword restored; severe bands remain. |
| Compute FP, stencil guard, sparse offset, scheduler fixes | Independently useful corrections; no complete visual repair. |
| Raw buffer/image sync | Did not remove corruption; experiment removed. |
| Full tiled-image readbacks | Much slower; no repair; removed. Actual CPU maps are linear. |
| Image readbacks plus targeted dc80 barrier | Major improvement: persistent sword and coherent sky, severe bands removed. |
| Dynamic material arrays | No established complete improvement; default off. |
| Fragment-loop cap increased from 32 to 1024 | Square particles/material defects persist; default cap unchanged. |
| All-compute LDS barrier override | Remaining defects persist; broad override removed. |
| Carry/borrow corrections | Targeted GPU tests pass; remaining visual defects persist. |

One still frame without square particles was an animation phase, not a particle fix. Judge several frames and fresh launches. Skipping a draw, nulling textures, clamping HDR values, or retaining a cleared render target may hide symptoms without restoring correct rendering.

The cleaned final executable was run through the delivered launcher twice:

- `Build/got-runs/20260928T074251880Z-12560`: access violation after approximately 29.5 seconds, exit `-1073741819` / `0xc0000005`, guest-exception. Similar intermittent failures predate this turn. JobWorker1 fault was observed at `0x700000dfb5ae`; exact cause remains unresolved. A nearest-symbol name is not sufficient function attribution.
- `Build/got-runs/20260928T074402661Z-20504`: completed 120-second limit, exit 0, no recorded failure; final screenshot `CUSA11456_20260928_034600_669_game_000006.png` confirms the improved but incomplete scene. This is the final accepted partial baseline.

**Useful earlier rendering localization**

Treat addresses/draw counts below as historical trace observations that need rechecking in a new run:

- Sword geometry hashes: FS `c5272859` (two direct draws) and `75512df2` (one), observed RT0 `0x1429ea0000`.
- Scene shader `2a3cacd4`: five-MRT pass, about ten scene draws/frame in an earlier trace. Runtime material descriptors include `index*340 + offset` patterns. Seven offsets `{0,32,64,96,128,160,192}` matched experimental 24-entry arrays; another runtime sample had a different pattern.
- FS `b0db526b` also had unresolved runtime-selected descriptors. The known limitation can result in null textures, but is not proof of the exact remaining visible defect.
- Dynamic sampled-image support was previously extended from implicit LOD to explicit LOD and gradient operations. Experimental scene arrays compile, but do not generically solve all runtime descriptors. Seven 24-entry arrays plus scalar resources consumed 179 image slots in one trace; table validity and cost matter.
- Full-color sky pass `167bdbe8` contributes to HDR scene `0x1426100000`. Post pass `d9002625` samples that scene and other inputs, writes `0x1424dd8000`; final background composite `9f9aac14` samples that image before UI. Final composite has a static sampled-image binding, so dynamic material selection there was not the identified problem.
- Compute `485454ae` explicitly clears the sword RT0; retaining stale contents is not justified.
- Older HDR values and LUT clamps were inconclusive: some large finite values are explicitly generated by guest shader arithmetic. Do not substitute arbitrary brightness changes for a correct shader/data path.
- Intrusive readbacks can change timing and disagree with normal screenshots. Confirm candidate fixes with probes removed.

**PS4 reference evidence and cleanup**

Console was at `192.168.0.27`, with GoldHEN FTP/logging used after the user brought it online. Do not assume it remains online or launch a new hardware capture unnecessarily.

Logger project: `C:\Users\Lintwer\Desktop\shadps4\hardware_logger\got_trace` (outside this repository). Its PM4 parser was improved for chains, budget/depth limits, and shader/RT0 draw census. Host tests passed and plugin built. Relevant valid capture: `full-census-20260928-klog.txt`, version 102, `full_pm4=1`, both submission hooks ready. Submissions 1200..1589 supplied 390 complete censuses, malformed=0 and unknown=0, with repeated sword draws. User confirmed the menu was visible. This counts submitted commands, not GPU pixel output. The older malformed census is superseded.

After the user confirmed GoT was closed, temporary `/data/GoldHEN/plugins.ini` and `/data/GoldHEN/plugins/got_trace.prx` were removed and absence verified. Capture stopped. Console config.ini was unchanged, SHA256 `99C1402ACFCC66F8D881A8E4D5288B3973C2FD16DD78A4F4D8D8FA5FE91EDF0C`. PluginLoader had already been enabled; it was not enabled by this investigation. Hardware logger source changes and backups remain locally; no temporary logger should currently be installed from this work.

**GitHub research and unresolved leads**

[GITHUB_REVIEW_20260928.md](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/Build/got-ir-audit/GITHUB_REVIEW_20260928.md) preserves linked PRs, branches, compatibility reports, and ranked leads; API responses/patches are in adjacent `github-review-20260928/`. Public GitHub access was used; `gh` was unauthenticated during that review. No independently verified complete fix for this exact GoT menu was found in the checked material.

The latest experiments supersede its suggestion that readbacks were merely a hypothesis. PR #4887 tiled readbacks and #5120 raw synchronization were tested in isolated experiments without a complete fix. PR #4773 is broader alias/readback reference work, not a demonstrated GoT repair. #5116 wave32 work is lower priority on the observed subgroup-64 host. #5129 depends on newer descriptor Summary machinery absent here. #5155 concerns cache permutations, but the effective tested configuration had pipeline caching disabled. #4972 was already in HEAD; #5103 is this fork's own work, not independent corroboration. Refresh remote status only if further investigation needs it.

**Next work and completion criteria**

1. Preserve and reproduce the current readback-plus-dc80 baseline before changing anything. Inspect effective settings in saved logs, not only the JSON file. Keep both missing geometry and shading as separate criteria.
2. Identify a specific remaining square-particle or bad-ground draw. Capture its actual selected texture, sampler, alpha/blending behavior, and shader inputs. Determine whether selection falls through a null/incorrect runtime descriptor. This is the next useful hypothesis, not an established diagnosis.
3. Compare isolated changes against multiple baseline frames. Broad material-array enabling already failed to establish a full repair; inspect the individual draw/resource instead of repeating wholesale flag combinations.
4. Investigate why dc80 requires the barrier on this host if pursuing a generic fix. Do not broaden the override without correctness evidence around subgroup/workgroup synchronization and divergence.
5. Track the intermittent access violation separately. Keep shader/material experiments from masking a stability regression.
6. A complete-menu claim requires correct sword, particles, grass/ground, lighting, UI, and sustained animation across at least two fresh launches, with ordinary visibility submission and no CPU occlusion bypass. Current evidence does not meet that bar. Gameplay would be a separate validation stage.

**Where to read next, in order**

- This handoff and [launcher notes](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/scripts/got_menu_rendering.md).
- [Latest results](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/Build/got-ir-audit/leads-20260928/RESULTS.md), final run folder, and `changes-this-turn.patch`.
- [Repair history](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/Build/got-ir-audit/REPAIR_20260928.md) for CPU tracing, hardware comparison, and archived probe locations. Read its appended superseding evidence before trusting earlier ongoing-state paragraphs.
- [Earlier review](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/Build/got-ir-audit/ONCE_OVER_20260928.md) and [long checkpoint](C:/Users/Lintwer/Desktop/shadps4/emu/shadPS4/Build/got-ir-audit/CHECKPOINT.md) only when a specific shader/experiment matters. Their historical next steps, running-session claims, failed external-model attempts, and missing-producer hypotheses are not current task state.

No active test, console capture, or background agent is pending at this handoff. Start from the successful partial result, preserve the existing work, and focus on the remaining visible defects.

### Current checkpoint — September30,10:06

The assignment is still incomplete. See review-completion-20260930/RESULTS.md,
consumer-completion and upstream checkpoint. Current suite442/442 passes. PR5193
GDS/barrier-budget improvements integrated selectively; sparse-offset repair
already equivalent locally. Continue still null-writesB8D4E0; consumer completion
state1/count12 shows outstanding work during next rebuild, not a lost count.
No plumes/horizon/gameplay acceptance. Decoder bounds guard fixes secondary host
Zydis fault only. Alpha bypass removed and rejected(no-op); CPU probe removed;
static redzone comparison still same failure, partial patch coverage. New queue
trace is default-off diagnostic work, not a guest ordering bypass.

### First visible post-Continue scene — September30,11:08

Temporary dependency-order intervention20260930T145758840Z-27532 reached rendered
opening ride and on-foot burning battlefield, with noB8D4E0 crash through600-second
launch limit. This is strong causal evidence, NOT acceptance: guest-specific
HOLD_PROXY_FOR_REBUILD + JOB_RECORDS intervention, scripted opening and no proven
camera/movement responsiveness. Menu plumes remain absent. Archived experimental
binary10C5DD17...A153 and source; intervention removed from production source.
See current RESULTS.md for exact hash/settings/evidence. GpuIdle Finish A/B failed
and was removed. Actual target writer identified at5822AC (generic582190 function).
Priority update TODO in pthread scheduler is next unverified lead.

## Depth-image lifetime and input validation — September30,11:40
Affinity-only run20260930T151608577Z-18236 reached a HOST GPU-command-thread
access violation at700000c47960 (not guestB8D4E0). The launcher misclassified it
as guest-exception; use exception address/context, not that status string.
Exact crashed binary preserved affinity-crash.exe SHA
EFF6675B6E3C89E7239841481BD38A5B810B7EB4D0751DB91E21AD8FCA241D68.
Relinked the same object files with /MAP (no recompilation) to affinity-crash.map.
Fault instruction reads source Image.usage (+2a0) after RegisterImage in
ResolveDepthOverlap. SlotVector insert at full capacity moves/frees old storage;
cache_image reference was acquired BEFORE insert. Repaired by reacquiring source
by cache_image_id after insertion; likewise FindDepthTarget stencil insertion
must reacquire its depth image for UID and FindView. No fake image/format fallback.
Production build passes; suite442/442 zero skips15.95s (suite-depth-overlap-lifetime.log).
Affinity probe REMOVED exactly from pthread-before-priority-probe.cpp. It did not
establish a repair; observed affinity calls omit JobWorkers/Proxy. Priorities remain
unimplemented host updates. Ordering intervention remains removed.
Runtime20260930T152920625Z-20996/153249756Z-6888 input tests did NOT establish
Continue: input-toggle implementation has two defects: chord sort reverses ordered
keys, and early pressed_keys.empty returns before checking toggled keys. Fixed
parser to parse each key separately and removed the early return. Build passes,
manual persistence validation still pending. Temporary default.ini toggles are
z->enter, n->w, h->l (avoid F9pause/F10FPS/F11fullscreen). Exact restore input-before.ini.
All five acceptance objectives still incomplete; no plume/horizon correction claimed.

## Fence interrupt semantics breakthrough — September30,11:48
Normal run20260930T153550326Z-24248 SHA8A20E79A8C3F9B01A492DD5858F58D904B03F4817BEC6D42EBEF725E37C5B97A
reached on-foot battlefield using corrected held Enter, with NO ordering intervention
and NO affinity override. Saved original CUA screenshot gameplay-no-order-intervention.png.
It later FAILED originalB8D4E0, so gameplay acceptance is NOT met. No camera/movement
response established before failure. Host depth lifetime fix remains valid separately.

NEW candidate root: PM4CmdReleaseMem treated INT_SEL=3 as IRQ (IrqUndocumented).
AMD PAL identifies3 send_data_and_write_confirm; Mesa defines3
EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM and its driver change explicitly says no IRQ.
Primary source:
https://raw.githubusercontent.com/GPUOpen-Drivers/pal/dev/src/core/hw/gfxip/gfx9/chip/gfx9_plus_merged_f32_mec_pm4_packets.h
Mesa driver discussion:
https://www.mail-archive.com/mesa-dev%40lists.freedesktop.org/msg166514.html
Guest generic582190/1E2E0 emits3 for compute labels. ProxyB897C0 blocks on equeue,
then releasesF120 whenever actual>=currenttarget. Extra IRQs while target is old can
prematurely release the next consumer group. This is a causal hypothesis pending
runtime, not a completed fix. Rename3 DataAfterWriteConfirm; no IRQ in RELEASE_MEM
or EVENT_WRITE_EOP while retaining memory write and GPU timeline completion.
Regression test matrix modes0/1/2/3 verifies label data, memory-before-callback,
and exact IRQ count for both packet types. Suite444/444 passes zero skips15.32s.
Build passes. CurrentSHA9E7BF36A699425EE47569BD5F821A62B710A247DEB975127B42A0590B9846E05.
Currenttest20260930T154610601Z-11428 clean preset: NO DEP_TRACE, NO GUARD_PROXY,
NO JOB_RECORDS, NO HOLD_PROXY, no guest int3 dependency patch. Menu reached correctly
(the same partial render, still NO white plumes). Continue/runtime testing pending.
launch-fence-no-guest-patches.ps1 and got_menu_rendering -Preset clean reproduce.
Still enables FS loop cap, DC80 LDS, dynamic sky/B0 and resource guard diagnostics;
not final no-bypass acceptance. Never use flag=0 to disable getenv-presence flags.

First clean-fence run20260930T154610601Z-11428 reaches on-foot battlefield.
Original screenshots fence-corrected-gameplay-before-camera.png,
fence-corrected-camera-held.png, fence-corrected-movement-held.png show responsive
camera (rear/front viewpoint) and movement (Jin walks to wall after held W).
No dependency int3 trace/guard or ordering intervention. Multiple-minute runtime
still in progress. This is the first CONTROL responsiveness evidence, not only
scripted intro. Menu still incomplete,3-fresh-launch acceptance pending.
