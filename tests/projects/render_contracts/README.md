# U16 render contracts v1

This is an executable test contract, not a claim that any GPU/platform passed.
The Python driver's unit tests use synthetic receipts and pixels only. Actual
D3D11/OpenGL evidence must come from the production-renderer probe below.

## Frozen probe invocation

```text
CaesuraRenderContractProbe --backend dx11|opengl --case ID --manifest ABS --resource-root ABS --output-dir NEWABS
```

The driver creates a fresh empty case directory before launching the probe.
The probe must reject an existing `result.json`, use resource-root as its CWD,
and write all outputs beneath output-dir. It must not select another backend
after a failure, retry a scene, update the manifest, or install dependencies.
One process executes one case. Every accepted result, including a successfully
observed expected core failure, exits **0**; failure/unavailability exits nonzero.

```text
python scripts/verify_render_contracts.py --probe ABS --manifest ABS --resource-root ABS --output-dir NEWABS
```

Default scope is the fixed nine cases below on `dx11`, then `opengl` (18 serial
owned processes, once each, 60 seconds per case). Explicit `--backend` and/or
`--case` selects a diagnostic subset and always records `scope_complete=false`.
Such a subset cannot produce overall `PASS`; a successful subset is `PARTIAL`.
Unavailable required backends/cases cannot pass. There is no retry or threshold
override. `validation_process.run_owned_command` owns and closes each process
tree, including on timeout/interruption.

## Case and capture IDs (order is fixed)

| Case ID | expected / fault_requested | Capture IDs, in order |
|---|---|---|
| `text-cjk-ruby` | `rendered` / `none` | `text-opaque`, `text-alpha`, `ruby-cjk`, `ruby-ascii` |
| `alpha-layers-batch` | `rendered` / `none` | `alpha-texture`, `opacity-unbatched`, `opacity-batched`, `multitexture-unbatched`, `multitexture-batched` |
| `rtt-fill-resize` | `rendered` / `none` | `fill-control`, `fill-first-a`, `fill-same-a`, `fill-changed-b`, `resized`, `returned`, `recreated` |
| `transition` | `rendered` / `none` | `blend-0`, `blend-025`, `blend-050`, `blend-1`, `wipe-half`, `rule-half` |
| `lut3d` | `rendered` / `none` | `baseline`, `identity-16`, `identity-64`, `swap-16`, `swap-64`, `half-16`, `half-64`, `strength-zero`, `cleared` |
| `shader-core-fallback` | `core_failure` / `fallback-missing-fragment` | none |
| `shader-core-blend` | `core_failure` / `blend-missing-fragment` | none |
| `shader-optional-softblur` | `optional_degrade` / `softblur-missing-fragment` | `baseline`, `degraded` |
| `shader-optional-transition` | `optional_degrade` / `transition-missing-fragment` | `baseline`, `degraded` |

All nine cases require checks `actual_backend`, `shutdown`. Rendered and
optional-degrade cases additionally require `core_ready`, `not_ifh`, and
`capture:<capture-id>` for every listed capture. Core failures instead require
`fault_applied_once`, `core_failed`, `capture_rejected`, `no_invalid_submit`.
Optional failures additionally require `fault_applied_once`,
`optional_degraded`, `no_invalid_submit`. The RTT case additionally requires
`fill_cache_reuse`, `resize_roundtrip`, `rtt_recreated`; the LUT case requires
`lut_borrowed_texture_alive`. No missing, duplicate or extra check IDs are
accepted. These checks supplement independent Python image checks.
The checks array may follow actual event order (including shutdown last); only
its exact unique ID set is fixed. Capture order remains fixed.

## Result JSON protocol

Every completed case also writes `checkpoint.json` with exactly
`{"phase":"after-shutdown","owner_advances":N}`. It must be a regular,
distinct output file no larger than 16 KiB. `N` is an integer between the
greater of one or the capture count and 128. Missing/unfinished checkpoints,
extra fields and `diagnostic.json` invalidate a passing receipt.

`result.json` has exactly these top-level fields:

```json
{
  "schema_version": 1,
  "suite_id": "u16-render-contracts-v1",
  "case_id": "transition",
  "requested_backend": "dx11",
  "actual_backend": "dx11",
  "backend_name": "Direct3D 11",
  "binary_path": "ABSOLUTE ACTUAL EXECUTABLE",
  "pid": 1234,
  "expected": "rendered",
  "status": "PASS",
  "shutdown_completed": true,
  "shader_health": {
    "core_ready": true,
    "rendering_disabled": false,
    "ifh": false,
    "fault_requested": "none",
    "fault_applied_count": 0,
    "failed_programs": []
  },
  "checks": [{"id":"actual_backend","passed":true,"detail":{}}],
  "captures": [{
    "id":"blend-0", "png":"blend-0.png", "rgba":"blend-0.rgba",
    "width":640, "height":360,
    "png_sha256":"LOWERCASE SHA256", "rgba_sha256":"LOWERCASE SHA256",
    "ticket":{"request_id":1,"generation":1,"frame_id":1,"status":"Completed"}
  }]
}
```

The example omits the rest of the required checks/captures for readability;
the actual sets must exactly match the selected manifest case. `detail` is an
object with measured context; it never defines the expectation or tolerance.
Capture arrays retain manifest order. Each capture is from a distinct normal
presentation, so ticket request IDs are unique, generation is unchanged within
the case, and frame IDs strictly increase (gaps for the two fill frames are
allowed). File paths are relative, unique, regular files under the case output;
absolute paths, `..`, symlink/junction escapes and aliases between outputs fail.

`actual_backend` is canonical `dx11`/`opengl`/`noop`; it must equal the requested
backend. Noop never passes. A normal/optional result must have core_ready=true,
rendering_disabled=false, ifh=false. Normal failed_programs is empty. Core
failure must report core_ready=false, rendering_disabled=true, the requested
fault applied exactly once, and only the relevant failed program (`Fallback`
or `Blend`). Its status is **EXPECTED_FAILURE_OBSERVED**, captures is empty;
it is not a successful render. Optional failure status is PASS with the intact
core and exactly `PostFxSoftBlur` or `Transition` in failed_programs.

The probe writes RGBA by decoding its own Completed PNG through the production
IImageDecoder. The driver independently parses PNG signature/chunks/CRC/IHDR,
bounded zlib output and scanline filters 0..4, requiring noninterlaced RGBA8.
It verifies pixel bytes equal the RGBA file, both hashes and exact dimensions,
then checks manifest RGB regions/formulas and cross-capture comparisons.

## Manifest recipes

The complete v1 gate binds the manifest's canonical semantic SHA256:
`37e8832971dac6a1c440caac54f14e2efebf2c5707b58623dea4c6af1be97095`.
Canonicalization is UTF-8 JSON with sorted object keys, no optional whitespace,
and Unicode preserved. Keeping case IDs while changing colors, LUT parameters,
font identity, regions or expectations cannot claim the same complete suite.

`manifest.json` is authoritative. Every capture gives width/height/background,
`regions` with half-open `[left,top,right,bottom]` rectangles, optional
`compare_to` and optional `font`. Recipe drawing rectangles are **xywh**;
regions are **ltrb**. Region expectations are `rgb`, `alpha_over`, `lerp`, or
`lut` (identity/swap_rb), with tolerance fixed at 2 or 3. Font edge tolerance is
4. The driver validates these bounds before starting a child; result files
cannot define or relax thresholds.

Font and other recipes are fixed data. Cube packing is x=b*N+r, y=g with each
index normalized by N-1; identity stores RGB, swap_rb stores BGR. LUT stage
strength is 0, 0.5 or 1. Same source color appears at separate screen positions.
Fill retains the original S/A/two-frames/same-A/B recipe independently of the
unchanged older 34-check fill probe. No threshold in the old probe is changed.
Before the first GPU run, the resize recipe was strengthened to draw the same
`GPU 15` string at `(32,260)` in `fill-changed-b`, `resized` and `returned`.
This exercises cached geometry across a real size change; it does not evade
cache reuse by changing the string or its origin. Frame count and tolerances
are unchanged.

## Independent FreeType reference protocol

Cases with any capture `font` also write `font-reference.json` with exactly:

```json
{
  "schema_version":1,
  "font_path":"assets/fonts/NotoSansCJKsc-Regular.otf",
  "font_sha256":"MATCH MANIFEST AND ACTUAL FONT",
  "freetype_version":"2.13.3",
  "pixel_size":28,
  "load_flags":"FT_LOAD_DEFAULT",
  "render_mode":"FT_RENDER_MODE_NORMAL",
  "sampling":"pixel-center-bilinear",
  "ascender":33,
  "references":[{
    "capture_id":"ruby-cjk", "width":640, "height":360,
    "coverage":"ruby-cjk.coverage", "coverage_sha256":"LOWERCASE SHA256",
    "opaque_pixels":100, "edge_pixels":100,
    "glyphs":[{
      "run":0, "codepoint":28450, "glyph_index":123,
      "advance_x":28, "bitmap_left":1, "bitmap_top":24,
      "bitmap_width":26, "bitmap_height":26,
      "pen_x":64, "baseline":209,
      "left":65, "top":185, "width":26, "height":26
    }]
  }]
}
```

Numbers above illustrate shape, not authoritative FT metrics. The actual
ascender/glyph metrics come directly from the fixed font through FreeType,
never TextRenderer geometry or a candidate screenshot. One glyph record is
required for every codepoint in every manifest font run, including spaces, in
run/codepoint order. Non-space glyph IDs and bitmap extents must be positive.
advance/bearings/bitmap dimensions are **unscaled** FT values; pen/baseline and
left/top/width/height are final screen geometry. The driver checks the manifest
run's origin/scale, FT advance sum, bearing equations, sequence and bounds.

Ordinary run origin is manifest x/y. `center_over` replaces x with
`base_x + (base_advance_sum*base_scale - ruby_advance_sum*ruby_scale)/2`.
Baseline is `run.y + ascender*scale`; pen increments `advance_x*scale`.
Ruby is the same 28px raster scaled 0.5, not a separately hinted 14px font.
Centered origin tolerance is 0.75 pixels; other supplied geometry must agree
within 1e-6. Vertical positions/gap are fixed by manifest.

Coverage is exactly width*height bytes, row-major, 8-bit grayscale, all zero
outside the manifest font ROI. FT masks are combined without training against
the GPU image. The driver recalculates opaque(255) and edge(1..254) counts,
requires at least **64 opaque and 16 edge pixels**, and nonempty coverage in
every non-space glyph rectangle. Every reference opaque pixel must match the
foreground/background alpha formula within RGB 2; edge and zero-coverage
pixels in the ROI within RGB 4. No shift search, ignored missing glyph or
percentage threshold is allowed. Font-reference capture IDs exactly match
those with font contracts, and coverage paths/hashes are checked like images.

FreeType version is recorded, not silently treated as measured cross-version
equivalence. The fixed font hash and flags are mandatory. The reference path is
an independently audited FT implementation; this protocol does not claim a
signature proves its mathematical correctness.

## Evidence and failures

The driver preserves command/cwd/exit or timeout, stdout/stderr, output hashes,
input identities before/after and the original result. It writes a separate
validation report without rewriting probe claims. No stale output is reused.
All expected captures/checks must exist even if other checks fail; a truncated
report is failure. Unexpected invalid-handle/already-destroyed resource logs
also fail; declared shader-failure diagnostics alone are allowed in fault cases.

Full PASS requires all 18 case contracts and stable source/binary/manifest/font
identities. Required UNAVAILABLE is not PASS. Diagnostic subsets are PARTIAL
even if all selected cases meet their contracts. Python synthetic receipt unit
tests validate these rejection rules; they are not GPU, FreeType or platform
execution evidence.

Driver exit codes are 0 for full PASS, 1 for a failed execution/contract, 2 for
invalid preflight or a successful but incomplete diagnostic subset, and 130 for
interruption after owned-process cleanup. Run the driver-only synthetic suite
with `python -B tests/scripts/test_render_contracts_driver.py`; it mocks the
owned process boundary and never launches the GPU executable.
