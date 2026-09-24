# MMTS Edit GUI

A Qt 6/C++ prototype for non-destructive editing of MMTS (MMT/TLV) media.

## Features

- Reads text-format `MMTSMAP 1` and binary-format `MMTSMAP2` sidecar indexes.
- Displays the number of tracks, MPT entries, RAPs, and seek points.
- Draws a lightweight timeline with seek, RAP, and MPT markers.
- **Frame-accurate video preview** for decrypted `.mmts` files: decodes with
  dantto4k's MMT/TLV demultiplexer and libavcodec (HEVC). Supports a seek bar
  and ±1-frame stepping. Seeking starts at the preceding RAP recorded in the
  `.mmtsmap`, then decodes forward to the requested frame. Decoding runs on a
  worker thread, so the UI remains responsive. Only the latest scrub request
  is used (coalescing), and recently decoded frames are cached for fast
  forward stepping and immediate back-and-forth navigation.
- Sets In and Out points at the current preview position.
- **Interactive timeline**: the playhead follows the preview. Click or drag a
  lane to seek and select ranges. Drag the selected range's In/Out grips to
  trim it; **Snap to RAP** can snap boundaries to RAPs.
- **Multi-segment EDL**: manage a cut list in the Segments panel with Add,
  Split @ playhead, and Delete. The edited program is the ordered
  concatenation of its segments.
- Saves and loads `.mmtsedit` sidecars (version 1, with an ordered `timeline`
  of segments).
- Exports the edited result as **MPEG-2 TS (`.ts`)** (`File > Export Edited
  TS...`): each segment is demultiplexed and remultiplexed through dantto4k's
  `RemuxerHandler` (HEVC + AAC to TS, without re-encoding). PTS/DTS values are
  rebased for each segment and **made continuous across joins**, so the output
  plays smoothly in general-purpose players (total TS duration equals the sum
  of the segment durations). tsduck is statically linked and runs inside the
  GUI. ARIB subtitles are included with their segment offsets applied. Both
  **In and Out are frame-accurate** through smart rendering: when In is not a
  RAP, the lead-in `[IN, next RAP)` is re-encoded with hevc_nvenc as an
  IDR-starting GOP; when Out is not a RAP, the tail `[last RAP, OUT)` is
  re-encoded. The middle is stream-copied and joined to those portions. Copy
  ranges end in **decode order (DTS)**, preventing dangling boundary
  references from open-GOP RASL leading pictures and avoiding decode failures
  at joins.
- Exports the edited result as **MMTS (`.mmts`)** (`File > Export Edited
  MMTS...`): copied ranges pass media TLV packets through byte-for-byte,
  without quality loss. Only partial GOPs at boundaries are re-encoded and
  repacked as MMT/TLV packets, so both **In and Out are frame-accurate**.
  Audio and subtitle MPUs are gated to `[IN, OUT)`. **The output timeline is
  continuous**: MMT presentation times exist only in MPT `mpu_timestamp`
  descriptors and PCR-source NTP packets, so timestamp entries in all later
  MPTs and their NTP packets are shifted to close joins while media payloads
  remain untouched. **Sequence numbers are also continuous**: MMTP packet
  sequences for every PID and MPU sequences for video, audio, and subtitles
  are renumbered consecutively across the output; MPT reference numbers are
  translated at the same time. Data-carousel PIDs keep their original numbers
  because those numbers are identifiers.

## Usage

The editor is non-destructive and never changes the original `.mmts` file.
Edits are saved in a small `.mmtsedit` file as an ordered list of source time
ranges (an EDL). `Export` uses that list to create a new `.mmts` file.

### 1. Open media

- Choose a decrypted `.mmts` file with `File > Open Media...`.
- The matching sidecar index, `<name>.mmtsmap`, is loaded from the same
  location. **If no map exists, the application offers to create one** by
  scanning the complete file. This can take several minutes for large 8K
  recordings; a cancellable progress dialog is shown. The result is the same
  as `dantto4k --write-mmtsmap-only`.
- When loading finishes, the first frame appears in the preview and the
  timeline shows RAP (blue), seek (green), and MPT (orange) markers.

### 2. Control the preview

- Drag the **seek bar**, or click or drag a **timeline lane**, to scrub.
- Use **`-1f` / `+1f`** to move backward or forward by one frame. The position
  label shows `HH:MM:SS.mmm (frame number)`.
- Forward stepping is fast, and moving back and forth near a position is
  immediate because recently decoded frames are cached.

### 3. Create a cut list (Segments tab)

The edited program is the **ordered concatenation of its segments**.

- **Add** — Adds a segment from the current playhead to the end.
- **Split @ playhead** — Splits the segment at the playhead.
- **Delete** — Deletes the selected segment.
- Select a segment in the table or by clicking its timeline block. Set its
  boundaries with **`Set In` / `Set Out`** (using the current preview
  position), the **In/Out spin boxes**, or by dragging the red/orange
  **grips** on the timeline.
- **Snap to RAP** (enabled by default in the toolbar) snaps In/Out dragging to
  the nearest random-access point for clean GOP boundaries.

### 4. Save or load an edit

- Use `File > Save Edit...` to save the `.mmtsedit` EDL.
- Use `File > Open Edit...` to restore it. The referenced map **and source
  media** are loaded, so the preview is available immediately.

### 5. Export TS

- Use `File > Export Edited TS...` to create an **MPEG-2 TS (`.ts`)** made by
  concatenating only the selected segments. Each segment is demultiplexed and
  remultiplexed to TS (HEVC + AAC, without re-encoding), with continuous
  PTS/DTS. A cancellable progress dialog is shown.
- Joins have continuous timestamps, so the result plays smoothly in
  general-purpose players. Both **In and Out are frame-accurate**: if a
  boundary is not a RAP, its lead-in or tail is re-encoded and joined. ARIB
  subtitles are included. Disable Snap to RAP to cut at any frame; exporting
  such ranges takes longer because they require re-encoding.

### 6. Export MMTS

- Use `File > Export Edited MMTS...` to create an **MMT/TLV (`.mmts`)** file
  made by concatenating only the selected segments. Copied ranges pass the
  original media packets through unchanged, with no quality loss; only partial
  boundary GOPs are re-encoded with hevc_nvenc and repacked as MMT packets.
  Both **In and Out are frame-accurate**.
- **Timeline and sequence numbers are continuous**: timestamps are adjusted
  through MPT signalling and NTP packets, while MMTP packet and MPU sequence
  numbers are made continuous by rewriting header fields. Media payloads stay
  unchanged. Seeking continues to work across segments, and players that
  check sequence continuity can consume the output directly. Audio and
  subtitles are gated to `[IN, OUT)` at MPU granularity (approximately 0.1 s).

### 7. Command-line version (`mmts-edit-cli`)

A headless CLI that uses the same exporters as the GUI Export menu
(`MmtsSmartExporter` / `MmtsTsExporter`) without opening the GUI. The intended
workflow is to cut interactively in the GUI, save an `.mmtsedit`, and batch
export it with the CLI without reopening large 8K recordings in the GUI.

```text
mmts-edit-cli --edit <project.mmtsedit> --output <out.ts|out.mmts> [options]
mmts-edit-cli --input <src.mmts> --output <out.ts|out.mmts> --cut <start>-<end> [--cut ...] [options]
```

- The output format is determined by the `--output` extension: `.ts` for
  MPEG-2 TS with continuous PTS (requires a tsduck-enabled build), or `.mmts`
  for frame-accurate MMT/TLV.
- If a `.mmtsmap` sidecar is missing, it is scanned and created automatically;
  use `--map` to specify one explicitly.
- `--cut` timestamps may be milliseconds or timecodes in
  `[HH:]MM:SS[.mmm]` format.
- `--no-frame-accurate` falls back to RAP boundaries instead of failing when
  boundary re-encoding is unavailable, such as on systems without a supported
  GPU. By default, frame accuracy is required and such an export fails.
- `--quiet` suppresses progress output.

## Dependencies

- Qt 6 (Widgets).
- dantto4k sources, pinned as a submodule at `thirdparty/dantto4k`; the
  project compiles a CAS-independent subset directly. Clone with
  `--recurse-submodules`, or run `git submodule update --init --recursive`
  afterwards. Point `-DDANTTO4K_DIR=...` at a working copy (for example
  `../dantto4k`) to build against that instead of the pinned revision.
- Prebuilt LGPL shared ffmpeg development libraries in `thirdparty/ffmpeg`
  (`include/lib/bin`). Override with `-DFFMPEG_DIR=...`. Releases ship FFmpeg
  9.0.1 (BtbN `ffmpeg-n9.0.1-11-ge47273f4d9`).
- Frame-accurate export re-encodes boundary GOPs with `hevc_nvenc`, which with
  FFmpeg 9.0 requires an **NVIDIA driver 610.00 or newer** (NVENC API 13.1).
  With an older driver the encoder fails to open; use `--no-frame-accurate`
  or Snap to RAP to cut at RAP boundaries instead.
- tsduck for TS export. dantto4k's `thirdparty/tsduck` must contain built
  static libraries (`bin/<Config>-x64/tsducklib.lib`, `tscorelib.lib`) and
  `bin/include`. Building dantto4k once creates them.

`.mmtsedit` format (`timeline` is an ordered list of source cut ranges; the
edited program is their concatenation):

```json
{
  "version": 1,
  "source": "sample.mmts",
  "sourceSize": 123456789,
  "map": "sample.mmtsmap",
  "timeline": [
    { "sourceStartMs": 25000,   "sourceEndMs": 60000 },
    { "sourceStartMs": 90000,   "sourceEndMs": 1800000 }
  ]
}
```

## Directory layout

```text
src/
  main.cpp
  model/    MmtsMap, MmtsEdit          (.mmtsmap / .mmtsedit parsing, EDL model)
            MmtsCutPlan                (shared EDL-to-RAP-boundary/copy-range planner)
            MmtsExporter               (.mmts output with byte copies at RAP boundaries)
  decode/   MmtsVideoDecoder           (frame-accurate HEVC decoding)
            MmtsDecodeController       (worker thread, read-ahead, frame cache)
            MmtsMapBuilder             (.mmtsmap generation through dantto4k scanning)
            MmtsTsExporter             (remultiplexes an EDL to MPEG-2 TS with continuous PTS)
            MmtsSmartExporter          (frame-accurate .mmts output: pass-through copy + boundary splice)
  ui/       MainWindow, PreviewWidget, TimelineWidget
tests/      decode_test, controller_test, edit_test, mapbuild_test,
            export_test, ts_export_test, smart_export_test  (headless smoke tests)
            mmts_edit_cli  (CLI implementation; built as the `mmts-edit-cli` executable)
```

## Build

```powershell
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build-msvc --config Release
```

ffmpeg development libraries are not included in the repository. Download the
Windows LGPL **shared** build that CI uses,
`ffmpeg-n9.0.1-11-ge47273f4d9-win64-lgpl-shared-9.0.zip` from the BtbN/FFmpeg-Builds
release `autobuild-2026-08-31-13-27`, and extract it into `thirdparty/ffmpeg` so
that `thirdparty/ffmpeg/{include,lib,bin}` exists. Alternatively, point
`-DFFMPEG_DIR=...` to an existing installation.

## License

MIT; see `LICENSE`. This project links dantto4k sources and uses ffmpeg (LGPL)
at runtime. Those components remain subject to their respective licenses.
