# MMTS Edit GUI

Qt6/C++ prototype for non-destructive MMTS editing.

Current scope:

- Load text `MMTSMAP 1` and binary `MMTSMAP2` sidecars.
- Show track, MPT, RAP and seek counts.
- Render a lightweight timeline with seek, RAP and MPT markers.
- **Frame-accurate video preview** of a (descrambled) `.mmts` file: decode via the
  dantto4k MMT/TLV demuxer + libavcodec (HEVC), with a seek bar and ±1 frame
  stepping. Seeking jumps to the nearest preceding RAP (from `.mmtsmap`) and
  decodes forward to the exact target. Decoding runs on a worker thread (the UI
  never blocks); scrub requests are coalesced (latest wins) and recent frames are
  cached, so forward stepping is fast and back/forth stepping is instant.
- Set In/Out from the current preview position.
- Interactive timeline: a playhead tracks the preview; click or drag the lane to
  seek and select a segment; drag the selected segment's In/Out grips to trim
  (with optional "Snap to RAP").
- Multi-segment EDL: the Segments panel lists the cuts; Add / Split @ playhead /
  Delete build a non-destructive cut list. The edited program is the
  concatenation of the segments in order.
- Save / load a `.mmtsedit` sidecar (version 1, `timeline` = ordered segments).
- Export a new `.mmts` from the edit (`File > Export Edited Media...`): copies the
  RAP-aligned source byte ranges for each segment (no re-encode). A single
  segment yields a clean contiguous clip; multiple segments keep their original
  MMT timestamps, so seams carry a PTS discontinuity (players resync at each
  segment's RAP). Contiguous-timestamp / frame-accurate export is future work.

## Usage

The editor is non-destructive: it never modifies the source `.mmts`. An edit is
just a list of source time ranges (an EDL) stored in a small `.mmtsedit` file,
and `Export` writes a new `.mmts` from that list.

### 1. Open the media

- `File > Open Media...` and pick a (descrambled) `.mmts`.
- It loads the sidecar index `<name>.mmtsmap` next to it. **If the map is
  missing, the app offers to create it** by scanning the whole file (this can
  take a few minutes for large 8K recordings; a progress dialog with Cancel is
  shown). The result is identical to `dantto4k --write-mmtsmap-only`.
- Once loaded, the first frame appears in the preview and the timeline shows
  RAP (blue) / seek (green) / MPT (amber) markers.

### 2. Navigate the preview

- Drag the **seek bar**, or click/drag on the **timeline lane**, to scrub.
- **`-1f` / `+1f`** step exactly one frame back/forward (frame-accurate). The
  position label shows `HH:MM:SS.mmm (#frame)`.
- Forward stepping is fast; stepping back and forth around a point is instant
  (recently decoded frames are cached).

### 3. Build the cut list (Segments tab)

The edited program is the **concatenation of the segments, in order**.

- **Add** – append a segment from the current playhead to the end.
- **Split @ playhead** – split the segment under the playhead into two.
- **Delete** – remove the selected segment.
- Select a segment (in the table or by clicking its block on the timeline) and
  set its bounds with **`Set In` / `Set Out`** (uses the current preview
  position), the **In/Out spin boxes**, or by dragging the segment's red/orange
  **grips** on the timeline.
- **Snap to RAP** (toolbar, on by default) snaps In/Out drags to the nearest
  random-access point, so cuts land on clean GOP boundaries.

### 4. Save / load the edit

- `File > Save Edit...` writes a `.mmtsedit` (the EDL).
- `File > Open Edit...` restores it: it loads the referenced map **and** the
  source media, so the preview works immediately.

### 5. Export a new `.mmts`

- `File > Export Edited Media...` writes a new `.mmts` containing only the
  segments, by copying the RAP-aligned source byte ranges (no re-encode, so it
  is fast and lossless). A progress dialog with Cancel is shown.
- A single segment produces a clean contiguous clip. With multiple segments the
  original MMT timestamps are kept, so each seam carries a PTS discontinuity
  (players resync at the next segment's RAP).

## Dependencies

- Qt 6 (Widgets).
- dantto4k sources, expected at `../dantto4k` (compiled directly, CAS-free subset).
- Prebuilt ffmpeg dev libs (LGPL shared) at `thirdparty/ffmpeg` (`include/lib/bin`).
  Override locations with `-DDANTTO4K_DIR=...` / `-DFFMPEG_DIR=...`.

`.mmtsedit` shape (`timeline` is an ordered list of source cut segments; the
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

## Project layout

```text
src/
  main.cpp
  model/    MmtsMap, MmtsEdit          (.mmtsmap / .mmtsedit parsing, EDL model)
            MmtsExporter               (RAP-aligned .mmts export)
  decode/   MmtsVideoDecoder           (frame-accurate HEVC decode)
            MmtsDecodeController        (worker thread + coalescing + frame cache)
            MmtsMapBuilder             (generate .mmtsmap via dantto4k scan)
  ui/       MainWindow, PreviewWidget, TimelineWidget
tests/      decode_test, controller_test, edit_test,
            export_test, mapbuild_test  (headless smoke tests)
```

## Build

```powershell
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build-msvc --config Release
```

ffmpeg dev libraries are not committed. Download a Windows LGPL **shared** build
(e.g. from BtbN/FFmpeg-Builds) and extract it to `thirdparty/ffmpeg` so that
`thirdparty/ffmpeg/{include,lib,bin}` exist, or point `-DFFMPEG_DIR=...` at an
existing install.

## License

MIT (see `LICENSE`). Note: this project links the dantto4k sources and uses
ffmpeg (LGPL) at runtime; those carry their own licenses.
