# dr_mp3 (vendored)

Single-header MP3 decoder by David Reid, based on minimp3 (lieff/minimp3).

- Upstream: https://github.com/mackron/dr_libs
- Version: dr_mp3 v0.7.4 (2026-10-31), commit `aec2dd20667f54acfdb66031217df6d240f56c52`
  ("dr_mp3: Version 0.7.4")
- `dr_mp3.h`: the upstream file unchanged,
  `https://raw.githubusercontent.com/mackron/dr_libs/aec2dd20667f54acfdb66031217df6d240f56c52/dr_mp3.h`,
  SHA-256 `f5c8020eeebb79877184432efdd2651f24869f5bb8f379d45450fd0aed1ee915`
- `LICENSE`: the upstream `LICENSE` of the same commit, SHA-256
  `dd1c647e6f767f8ff4b2dfae0fed314726600a01e0cf1ef556afddd5fa96ff15`.
  Public domain (Unlicense) or MIT No Attribution, at the user's choice; the
  minimp3 part is CC0 (the note at the end of `dr_mp3.h`).
- `dr_mp3.c`: this project's translation unit with the implementation
  (`DR_MP3_IMPLEMENTATION`, `DR_MP3_NO_STDIO`).

Used off Windows only: the .mp3 BGM of `src/engine/pdi/T521_pdibgm.cpp`
(Windows decodes it with Media Foundation).

To update: replace `dr_mp3.h` and `LICENSE` with the files of a newer
release commit and record its version, commit and checksums here.
