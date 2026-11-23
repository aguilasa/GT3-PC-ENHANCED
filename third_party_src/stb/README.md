# stb (vendored)

Single-header libraries by Sean Barrett and contributors.

- Upstream: https://github.com/nothings/stb
- Commit `6b5799bae8a1fe3792691ef4b72ae26434c3e799` (2026-11-19, "update
  stb_image_write version"); the files unchanged from it:
  - `stb_truetype.h` v1.28,
    `https://raw.githubusercontent.com/nothings/stb/6b5799bae8a1fe3792691ef4b72ae26434c3e799/stb_truetype.h`,
    SHA-256 `428cfe93520f2aed5e413a220b0db5bba5b17e2ebe5a64e4a8efa9eeeb67ec5f`
  - `stb_image.h` v2.34,
    `https://raw.githubusercontent.com/nothings/stb/6b5799bae8a1fe3792691ef4b72ae26434c3e799/stb_image.h`,
    SHA-256 `dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020`
  - `LICENSE`, SHA-256
    `eb36e45c704dd0b59730115523e089b051852625b4db0a6fe76c991900af20e7`:
    MIT or public domain (Unlicense), at the user's choice.

Used off Windows only, each implementation compiled in the one file that
uses it (`STB_*_IMPLEMENTATION` with `STBTT_STATIC` / `STB_IMAGE_STATIC`):

- `stb_truetype.h`: the vector fonts of `src/engine/pgl/native/pgl_vfont.cpp`
  (Windows reads them with DirectWrite).
- `stb_image.h` (`STBI_ONLY_JPEG`): the `.jpg` replacement textures of
  `src/engine/pgl/native/pgl_texrepl.cpp` (Windows decodes them with WIC;
  a `.png` goes through `platform/png_image.h` on both).

To update: replace the files with those of a newer commit and record its
commit, versions and checksums here.
