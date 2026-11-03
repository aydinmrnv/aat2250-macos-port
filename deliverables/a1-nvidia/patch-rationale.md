# Patch rationale: publish the NVDEC/NVENC instances RM reports (A1)

- Branch: `port/a1-5090` in the worktree `work\nvidia-a1`
- Commit: `7f99e4be2d5f1106c862c1190e2768c9013ab294` (parent `470e391e8cf8d7e499cdabfa7f2daadb6e14bcfd`). Not pushed.
- Patch: `patches\a1-nvrm-video-engine-instances.patch` (`git format-patch`; `git apply --check` passes on 470e391)
- Files: `kexts/NVRM/nvrm_fbinfo.cpp` (+54/-1), `kexts/NVRM/NVRM.cpp` (+11/-2), `tools/test_gpu_profile_video_engines.py` (new, 165 lines)

Labels: **D** = demonstrated, **P** = project claim, **H** = hypothesis.

## What

1. `nvrm_query_gpu_profile()` already holds an RM client and subdevice. On that subdevice it now also issues `NV2080_CTRL_CMD_GPU_GET_ENGINES_V2` (0x20800170) with `{ NvU32 engineCount; NvU32 engineList[0x54]; }`. It logs the raw engine list as hex (`NVRM-fbinfo: profile: gpu ... engines (N): ... -> NVDEC mask 0x.., NVENC mask 0x..`). Using a fixed table of NVIDIA engine ids, it returns two masks: NVDEC0-7 = 0x13-0x1a, NVENC0-2 = 0x1b-0x1d, and **NVENC3 = 0x3f**.
2. `NVRM::publishGpuProfile()` adds `nvdec-instances` and `nvenc-instances` (bit n = instance n) to the existing `nvrm-gpu-profile` dictionary on the PCI device, and logs them. The keys appear **only** when the RM answered, so "unknown" can be told apart from "none". A failed engine query logs its status and leaves the rest of the profile unchanged. `ok` is still decided by the architecture.
3. Nothing reads the new keys. No engine selection, decoder choice or behaviour changes.

## Why: the evidence

| Fact | Source | Label |
|---|---|---|
| The profile publishes only the newest video **class**, the same `0xCFB0`/`0xCFB7` on every GB20x, and cannot express which instances exist | `drv kexts/NVRM/nvrm_fbinfo.cpp:226-288`, `NVRM.cpp:1297-1298`, `g_gpu_class_list.c` | D |
| RM class tables: GB202 has NVDEC0-3, NVENC0-3 and NVJPG0-3. GB206 (tested RTX 5060) has instance 0 of each. GB203 has NVDEC0-2 and NVENC0-1. The same in 610.57.04 and 615.78.08. | `ogkm615 src/nvidia/generated/g_gpu_class_list.c:3205-3317, 3380-3487, 3716-3819`, plus the `ogkm610` copy | D |
| On an RTX 5070 Ti (GB203) RM's engine list had NVENC1 and NVENC2 (0x1c, 0x1d) but no NVENC0 (0x1b). The closed encoder libraries hard-coded NVENC0 and failed with `NV_ERR_OBJECT_NOT_FOUND`. Fixed in 1.14.0. | issue #105 (closed) | P (user logs) |
| The RTX 5090 "does not support hardware decoding". No usable logs. | issue #425 (open) | P |
| The decoder libraries are closed, so a source fix to their engine choice is impossible here | `git ls-files`, #105 text | D/P |
| NVENC3 is engine type 0x3f, not contiguous with NVENC2 (0x1d) | `ogkm615 cl2080_notification.h:315-317,355`; `ogkm610 cl2080_notification.h:312-315,353` | D |
| The control id and list size are identical in 610.57.04 and 615.78.08, so the patch is safe whichever tree the out-of-tree RM was built from | `ogkm615 ctrl2080gpu.h:779-789`; `ogkm610 ctrl2080gpu.h:778-781` | D |

Because the engine choice is inside closed binaries, the smallest change that settles the #425 question is to make the RM's own answer visible: does this 5090 expose NVDEC0, and how many instances does it have? Only then can someone tell "the library asks for an instance that does not exist" apart from the sandbox, delegation or pacing explanations listed in `2b85-trace.md` section 3. Unlike the alternatives considered below, this needs no hardware assumption.

## Alternatives rejected

- **Changing the decoder library choice for GB202:** no evidence says what is wrong, and the libraries are closed. That would be guessing.
- **Raising the 8 GB small-BAR resize cap or adding 32 GB handling:** the project reports 16 GB resizes failing on a 3060 and a 5080 (`NVRM.cpp:1043`, P). With no macOS boot log from this 5090, changing BAR policy would invent hardware behaviour. This is documented as a test (T-BAR-1/2) instead.
- **Adding `GB202` to `test_card_support.py`:** that test needs macOS clang, and `GB20` prefix matching already covers GB202. It adds no information.

## Validation that was actually run (Windows 11, Python 3.14.0, no C compiler on PATH)

```
$ cd work/nvidia-a1
$ python -I tools/test_gpu_profile_video_engines.py . ../../src/open-gpu-kernel-modules
ok   snapshot matches open-gpu-kernel-modules headers ({'ids': {'NV2080_CTRL_CMD_GPU_GET_ENGINES_V2': 545259888, 'NV2080_GPU_MAX_ENGINES_LIST_SIZE': 84, 'NV2080_ENGINE_TYPE_LAST': 84}, 'dec': {0: 19, 1: 20, 2: 21, 3: 22, 4: 23, 5: 24, 6: 25, 7: 26}, 'enc': {0: 27, 1: 28, 2: 29, 3: 63}})
ok   the kext sends NV2080_CTRL_CMD_GPU_GET_ENGINES_V2 (0x20800170)
ok   its list is NV2080_GPU_MAX_ENGINES_LIST_SIZE entries, so the params size matches RM's struct (0x54)
ok   the params struct is { NvU32 engineCount; NvU32 engineList[kEngineListMax]; }
ok   kVideoEngines parsed from nvrm_fbinfo.cpp (12 rows)
ok   NVDEC0-7 use NVIDIA's engine ids 0x13-0x1a ({0: '0x13', ..., 7: '0x1a'})
ok   NVENC0-3 use NVIDIA's engine ids, NVENC3 at 0x3f ({0: '0x1b', 1: '0x1c', 2: '0x1d', 3: '0x3f'})
ok   no engine id appears twice
ok   #105 engine list: NVDEC mask 0x1, NVENC mask 0x6 (no NVENC0) (0x1 0x6)
ok   a GB202 list with all four NVDEC and NVENC gives masks 0xf / 0xf ((15, 15))
ok   a GB206-shaped list (one NVDEC, one NVENC) gives 0x1 / 0x1 ((1, 1))
ok   engineCount beyond the list size is clamped (no read past engineList)
ok   engineCount 0 yields no instances
ok   negative control: NVENC3 numbered 0x1e reports only three of GB202's encoders (0x7)
ok   NVRM.cpp declares nvrm_query_gpu_profile with the definition's parameters (12 vs 12 params)
ok   negative control: the pre-patch 9-parameter declaration is reported as a mismatch (9 params)
ok   the call passes as many arguments as the definition takes (12 args)
ok   nvdec-instances / nvenc-instances are published only when the engine list was read
ok   a failed GET_ENGINES_V2 does not fail the profile (ok is still decided by the architecture)
PASS
exit=0
```
(The `dec` dict line is abbreviated here. The full output is identical in form to the run without a tree.)

The same test with no NVIDIA tree passes on the embedded snapshot (`note: no open-gpu-kernel-modules tree given`), with exit 0.

**Mutation check:** the new test run against the **unpatched** tree (`python -I tools/test_gpu_profile_video_engines.py src/nvidia-macos-driver`) prints 15 FAIL lines and exits 1.

**Regression check:** the existing pure-Python test was read in full first. It only reads `nvk/nvk-macos.patch`.
```
$ python -I tools/test_copy_class_selection.py .
note: ~/ogkm610 not present -- running on the embedded snapshot only (no drift re-check)
ok   every supported chip selects a copy class RM says it offers (offenders: )
... (10 ok lines)
PASS   exit=0
```

The test reads repository files and, optionally, the NVIDIA headers. It creates no files and runs no subprocess.

## What remains unverified

1. **Not compiled.** The kext changes have not been built. There is no macOS SDK or clang here, and the RM objects come from an out-of-tree build that is not in the repo (`architecture.md` section 3). Points to check at build time: `-Wall` with the static arrays, and the `*(cond ? a : b) |=` expression (valid C++).
2. **Not run.** It has not run on any GPU. H: `GET_ENGINES_V2` from the kernel RM API client (`rm_kernel_rmapi_op`) on a subdevice is allowed, as the existing `MC_GET_ARCH_INFO` and `FB_GET_INFO_V2` calls on the same handle are. Unconfirmed under GSP-RM on GB202.
3. H: RM's `GET_ENGINES_V2` list reflects floor-sweeping (#105's list suggests it does on GB203). Not verified for GB202.
4. **Unexplained:** #105's GB203 list contains NVENC2 (0x1d), but GB203's class table lists only NVENC0-1. The runtime list and the class table use different numbering, or the table is not a ceiling. That is why the test treats the GB202 "all four" case as a synthetic ceiling, not a prediction.
5. The patch does not fix #425. It only produces the data to diagnose it (T-MEDIA-1 in `macos-gpu-tests.md`).
6. Upstreaming: `drv/AGENTS.md` and CONTRIBUTING say AI-written PRs are closed. A human would have to rewrite and submit the change. Nothing was pushed or posted.

## Review follow-up (coordinator, 2026-11-02)
Independent review (`../review-a1-patch.md`) verdict: approve-with-changes. Changes applied in commit `9c764471fcf41004edf2f932c566580d72219e69` on `port/a1-5090`:
- **Corrected:** earlier text in this doc and in commit 7f99e4b says "GB203 lacks NVENC0 (#105)". That is **not established**. The #105 list, read as NV2080 engine types, contradicts RM's GB203 class table (NVENC0-1). Its numbering is unverified, and settling it is one purpose of the diagnostic.
- The test now checks the C loop itself (clamp, clamped iteration count, mask routing). Mutating the clamp or swapping the masks in a copy makes it FAIL (output verified).
- The 610.57.04 headers are now checked: "snapshot matches ogkm-610-headers headers", ok.
- Not addressed: the raw engine list is logged via FBILOG/kprintf only; the masks themselves go through LOG and the IORegistry.
- Still not compiled, not run on macOS, not run on a GB202.
Patch series: `patches\0001-*.patch`, `0002-*.patch`. It applies cleanly via `git am` onto 470e391, and the result is identical to the branch.

## First compile on macOS SDK (2026-11-02, Mac Studio)
**Setup**
- Xcode 27.0, MacOSX27.0 SDK, `clang++ -target x86_64-apple-macos15.0 -mkernel -fapple-kext`.
- Headers: open-gpu-kernel-modules 615.78.08 (sparse).
- Flag set: reconstructed by the coordinator, because the maintainer's `compile_cmds.sh` is private. Script: `patches\kcompile.sh`.
- The tree on the Mac is `git am` of 0001+0002 onto 470e391. Its tree hash `f79ea71c` is identical to `port/a1-5090`.

**Results**
- Both `nvrm_fbinfo.cpp` and `NVRM.cpp` compile to `Mach-O 64-bit object x86_64`, at baseline 470e391 and patched.
- Warnings in the project's own sources: 35 before and 35 after. **The patch adds no warnings or errors.**

**Limits**
- These are not the release's exact flags. Nothing is linked, because the NVRM.kext link script (`build-nvrm.sh`) is missing upstream. Nothing has run.
