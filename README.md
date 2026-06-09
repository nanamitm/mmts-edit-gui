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

`File > Open Media...` opens `sample.mmts` and auto-loads `sample.mmtsmap`
(generate the map with `dantto4k --write-mmtsmap-only sample.mmts`).

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
  decode/   MmtsVideoDecoder,          (frame-accurate HEVC decode)
            MmtsDecodeController        (worker thread + coalescing + frame cache)
  ui/       MainWindow, PreviewWidget, TimelineWidget
tests/      decode_test, controller_test, edit_test   (headless smoke tests)
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
