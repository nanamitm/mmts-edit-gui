# MMTS Edit GUI

Qt6/C++ prototype for non-destructive MMTS editing.

Current scope:

- Load text `MMTSMAP 1` and binary `MMTSMAP2` sidecars.
- Show track, MPT, RAP and seek counts.
- Render a lightweight timeline with seek, RAP and MPT markers.
- Save a basic `.mmtsedit` sidecar with one non-destructive source range.

Current `.mmtsedit` shape:

```json
{
  "version": 1,
  "source": "sample.mmts",
  "sourceSize": 123456789,
  "map": "sample.mmtsmap",
  "timeline": [
    {
      "sourceStartMs": 25000,
      "sourceEndMs": 1800000
    }
  ]
}
```

Build example:

```powershell
C:\Qt\Tools\CMake_64\bin\cmake.exe -S . -B build-msvc -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\msvc2022_64
C:\Qt\Tools\CMake_64\bin\cmake.exe --build build-msvc --config Release
```
