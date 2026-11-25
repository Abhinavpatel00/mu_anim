# External Dependencies & Modifications

This file documents all vendored third-party code in `/home/user/mu_anim/third_party/` and any updates made relative to upstream commits.

## 1. `third_party/mu_new`

- **Upstream repository:** `https://github.com/Abhinavpatel00/mu_new`
- **Base commit:** `a79cc93` (`rm unnecessary files`)
- **Modifications inside `third_party/mu_new`:**
  - `external/mu_lib/` was updated to match `https://github.com/Abhinavpatel00/mu_lib-` at commit `2c3d175`, bringing in the full header-only C11 library (`mu_id_pool.h`, `mu_toposort.h`, `mu_save.h`, `mu_spline.h`, `mu_morph2d.h`, `mu_files.h`, `mu_thread.h`, `mu_shapes.h`, `mu_color.h`, `mu_math.h`, etc.) and `mu.c`.
  - No edits were made to `third_party/mu_new/vk.h`, `third_party/mu_new/vk.c`, `third_party/mu_new/platform.h`, `third_party/mu_new/platform.c`, or `third_party/mu_new/external/mu_lib/`. `mu_anim` links `vk.c`, `platform.c`, `mu.c`, and GLFW directly from `third_party/mu_new`.

## 2. `third_party/stb/`

- **`stb_truetype.h`** (`https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h`, v1.28, public domain / MIT): Included once with `#define STB_TRUETYPE_IMPLEMENTATION` inside `anim_svg.c` to extract exact TrueType quadratic Bézier glyph outlines and kerning/metrics for UTF-8 strings.
- **`stb_image.h`** (`https://raw.githubusercontent.com/nothings/stb/master/stb_image.h`, v2.34, public domain / MIT): Included once with `#define STB_IMAGE_IMPLEMENTATION` inside `anim_svg.c` to decode PNG/JPEG/BMP/TGA textures into RGBA8 for `anim_image`, `anim_image_from_memory`, and `anim_surface_set_texture`.

## 3. System / CLI Tools (Runtime & Shader Build)

- **Shader compiler:** `slangc` (`2026.22`) compiles `shaders/anim.slang` into `shaders/anim.<entry>.spv`. Precompiled `.spv` binaries are checked into `shaders/` so `make all` runs without requiring `slangc` at build time.
- **Math typesetting backends (optional at runtime, cached with `mu_save` CRC32C):**
  - `typst` (`0.15.1`): Fast default backend for `anim_typst` / `anim_math_tex(..., ANIM_MATH_TYPST)`.
  - `latex` + `dvisvgm` (`3.4.4`): Classic LaTeX backend for `anim_latex` / `anim_math_tex(..., ANIM_MATH_LATEX)`.
- **Video export (optional at runtime):** `ffmpeg` (`7.1.5`) invoked as a streaming child pipe for H.264/AAC `.mp4` export (`--video`).
