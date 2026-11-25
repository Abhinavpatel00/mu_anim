# `mu_anim` — Production C11 + Vulkan + Slang Mathematical Animation Engine & `.anim` DSL

`mu_anim` is a high-performance, beyond-Manim mathematical & technical animation engine written in strict C11 (`-std=c11 -Wall -Wextra -Werror -fno-strict-aliasing`) on top of `mu_new` (`vk.h`/`vk.c`, `platform.h`/`platform.c`) and `mu_lib`, with a unified Slang GPU shader (`shaders/anim.slang`), 11 precompiled SPIR-V binaries, a standalone embeddable `.anim` scripting DSL (`anim_script.h`/`anim_script.c` + `anim_dsl`), and a shippable static library (`libmu_anim.a` + `pkg-config`).

## Architecture & Capabilities

- **`anim.h` / `anim_internal.h` / `anim.c`**:
  - Handle-based scene graph (`AnimId` generational IDs backed by `mu_id_pool`, ZII slot 0) with hot/cold DOD separation (`VMobHot` / `VMobCold`) and error diagnostics (`anim_last_error()` / `anim_clear_error()`).
  - Arbitrary-size dynamic subtree & morph buffers (no fixed 512-element subtree/morph caps), 3D cubic Bézier `VMobject`s (`anim_vmob_*`, De Casteljau subpath trimming, nonlinear `anim_vmob_apply_warp`), and 3D perspective/ortho-blend camera.
  - Rate functions (`ANIM_RATE_*`, damped `ANIM_RATE_SPRING`, custom `AnimRateFn` callbacks, cubic Bézier) and perceptual **Oklab** color interpolation.
  - Rich styling: linear/radial **GPU gradients**, cumulative arc-length **animated dashes** (`anim_set_dash` + `anim_anim_dash_flow`), **soft drop shadows** (`anim_set_shadow`), and **analytic clip boxes** (`anim_set_clip_rect`).
  - **True Z-buffered 3D parametric surfaces & curves** (`anim_surface3d`, `anim_sphere`, `anim_torus`, `anim_axes3d`, `anim_parametric_curve`) with **Cook-Torrance GGX PBR specular** (`D`, Smith-Schlick `G`, Fresnel-Schlick `F`), sky/ground hemisphere bounce, environment reflection, `fwidth`-antialiased UV wireframe grids, and **3D camera-facing billboards** (`anim_set_billboard`).
  - **Technical & UI mobjects**: `anim_paragraph` (aligned multi-line text), `anim_code` + `anim_code_highlight_line` (syntax-highlighted monospace code blocks), `anim_table` + `anim_table_cell`, and `anim_graph_network` (directed/undirected graphs with labeled nodes).
  - `ValueTracker` + **topologically sorted updater DAG** (`mu_toposort`).
  - Declarative `AnimTimeline` with snapshot-backed seeking, **overlapping clip scheduling** (`anim_play_at`), **`anim_play_lagged`** (`LaggedStart`), **`anim_play_sequence`** (`Succession`), `anim_anim_follow_path`, `anim_anim_dash_flow`, `anim_anim_camera_orbit`, **timeline-animatable PIP zoom inset** (`anim_scene_set_zoom_inset`, `anim_anim_zoom_inset_to`), and hierarchical `Transform` / `TransformMatchingTex` / `TransformFromCopy`.
  - **Full Manim Animation Parity + Custom Animations**: `Unwrite`, `GrowFromEdge`, `SpinInFromNothing`, `ShrinkToCenter`, `SpiralIn`, `ShowIncreasingSubsets`, `TypeWithCursor`, `Flash`, `FocusOn`, `Broadcast`, `ApplyWave`, `ApplyMatrix` / `ApplyMatrix3`, `ApplyPointwiseFunction` (`anim_anim_apply_warp`), `Homotopy` (`anim_anim_homotopy`), and arbitrary `anim_anim_custom` alpha callbacks.
- **`anim_extra.c` (Geometry Matchers, 2D CSG, Computational Geometry, Graphing, Tracers & 3D Solids)**:
  - **Shape Matchers & Geometry**: `anim_cubic_bezier`, `anim_arc_between_points`, `anim_sector`, `anim_angle`, `anim_right_angle`, `anim_surrounding_rect`, `anim_background_rect`, `anim_underline`, `anim_cross`, and seek-accurate **`anim_traced_path`** (`TracedPath`).
  - **2D Polygon Boolean CSG & Computational Geometry**: `anim_bool_op` (`ANIM_BOOL_UNION`, `ANIM_BOOL_INTERSECT`, `ANIM_BOOL_DIFFERENCE`, `ANIM_BOOL_XOR`) producing even-odd-filled `VMobject`s with exact holes/cutouts, **`anim_convex_hull`** (Andrew's Monotone Chain), and **`anim_voronoi`** (half-plane clipped cells).
  - **Graphing, Vector Fields & Raster/Particle Tooling**: `anim_polar_plane`, `anim_plot_polar`, **`anim_implicit_plot`** (Marching Squares `f(x, y) = 0` contour extractor), **`anim_contour_plot`** (multi-level Marching Squares), **`anim_stream_lines`** (RK2 continuous vector-field streamlines), **`anim_pixel_grid`** (rasterizer pixel grid with sample centers), **`anim_particle_system`** (seek-accurate analytical physics emitter), `anim_bulleted_list`, and live tracker-synced `anim_variable`.
  - **True Z-Buffered 3D Solids & Camera Frustums**: `anim_cube`, `anim_prism`, `anim_cylinder`, `anim_cone`, `anim_polyhedron` (`TETRAHEDRON`, `CUBE`, `OCTAHEDRON`, `DODECAHEDRON`, `ICOSAHEDRON`), `anim_line3d`, `anim_arrow3d`, `anim_dot3d`, and **`anim_frustum3d`** (perspective frustum wireframe + near/far clip planes).
- **`anim_pcs.c` (Physics & Computer Science Toolkit)**:
  - **Physics (`anim_phys_*`)**: built-in 4th-order Runge-Kutta integrator (`anim_phys_step_rk4`) & ODE trajectory curves (`anim_phys_trajectory`), live-updatable coiled springs (`anim_phys_spring` / `_set`), pendulums (`anim_phys_pendulum` / `_set`), free-body diagrams (`anim_phys_free_body`), waves (`anim_phys_wave` / `_set`), electrostatic point charges (`anim_phys_charge`) + RK2 electric field streamlines (`anim_phys_field_lines`), and thin-lens optics (`anim_phys_lens`, `anim_phys_ray_bundle`).
  - **Computer Science (`anim_cs_*`)**: index-labeled arrays (`anim_cs_array`), sorting bar charts (`anim_cs_bar_chart`), one-call arced swap animations (`anim_cs_swap_mobs`, `anim_cs_bar_swap`, `anim_anim_move_arc`), linked lists (`anim_cs_linked_list`), stacks & queues (`anim_cs_stack_queue`), heap-indexed binary trees (`anim_cs_binary_tree`), finite state machines with double-ring accepting states & curved/self-loop transitions (`anim_cs_fsm`), IEEE digital logic gates (`anim_cs_logic_gate`: `AND`/`OR`/`XOR`/`NOT`/`NAND`/`NOR`), color-coded binary bit registers (`anim_cs_bit_register`), and multi-layer neural networks (`anim_cs_neural_net`).
- **`anim_svg.c`**:
  - Cached `stb_truetype` UTF-8 text outlining (16-slot cache with graceful fallback and error diagnostics).
  - Two-pass SVG parser (`<defs>`/`<symbol>`/`<path>`/`<rect>`/`<circle>`/`<polygon>`/`<use>`, SVG elliptical arcs, transforms, `#0100XX` token group tags).
  - Typst (`typst compile --format svg`) and LaTeX (`latex` + `dvisvgm`) equation compilers with `{{token}}` isolation and CRC32C-checked `mu_save` disk caching under `.cache/mu_anim_tex/`.
- **`anim_render.c` + `shaders/anim.slang`**:
  - Resolution-independent **GPU band-accelerated quadratic Bézier rasterizer** (64 bands/shape max, `mu_vm_arena`-backed per-frame/per-shape scratch, screen-space frustum culling, global caps up to `32,768` shapes / `131,072` bands / `524,288` segments).
  - Z-buffered (`FMT_D32F`) lit 3D surface pass (`vs_surface3d`/`fs_surface3d`) composited seamlessly beneath 2D/3D vector bands.
  - **Temporal motion blur** (`--blur N` sub-frame HDR accumulation via `fs_accum_blit`), 4-mip Karis-averaged HDR bloom, selectable **Soft / ACES / AgX** tone mapping (`AnimRenderOpts.tonemap`), radial **chromatic aberration** (`aberration_px`), **film grain** (`grain_amount`), **GPU Picture-in-Picture Zoomed Camera Inset** (`AnimRenderOpts.zoom_inset_enable` or timeline `anim_anim_zoom_inset_to`), resolution presets (`--preset 720p|1080p|4k|vertical|square`), and transparent alpha PNG export (`--alpha 1`).
  - Interactive GLFW/Vulkan live preview (`Space` play/pause, `Left`/`Right` scrub, mouse drag on scrub bar, right/middle-drag 3D orbit, scroll wheel zoom, `C` camera reset, `B` bloom, `H` HUD, `F2` screenshot, `Home`/`End`, `1`/`2`/`4` speed) and triple-buffered async `ffmpeg` MP4 + WAV audio muxing.

## Building, Installing & Linking as a Library

```bash
# Build libmu_anim.a, mu_anim, anim_dsl, and all C showcase binaries
make -j4 all examples

# Run automated unit, >5,000-cubic stress, 3D surface, Physics+CS, .anim DSL, and Vulkan validation tests
make check

# Install static library, public headers (anim.h, anim_script.h), precompiled SPIR-V shaders, anim_dsl, and mu_anim.pc
sudo make install PREFIX=/usr/local
```

External C11 projects need only `anim.h` (and optionally `anim_script.h`) and can manage Vulkan/GLFW lifecycle in one call via `AnimContext`:

```c
#include <mu_anim/anim.h>
#include <mu_anim/anim_script.h>

int main(void) {
    AnimContext  *ctx = anim_context_create(1280, 720, "app", /*headless=*/true, /*validate=*/false);
    AnimScene    *sc  = anim_scene_create();
    AnimTimeline *tl  = anim_timeline_create();
    AnimScript   *dsl = anim_script_create();
    AnimScriptOpts opts;

    anim_script_exec_file(dsl, sc, tl, "examples/calculus_tangent.anim", &opts);
    anim_export_mp4(anim_context_renderer(ctx), tl, sc, &opts.render, "out.mp4");

    anim_script_destroy(dsl);
    anim_timeline_destroy(tl);
    anim_scene_destroy(sc);
    anim_context_destroy(ctx);
    return 0;
}
```

Compile and link cleanly outside the repo via `pkg-config`:
```bash
cc -std=c11 app.c $(pkg-config --cflags --libs mu_anim) -o app
```
Shaders are resolved automatically from `$MU_ANIM_SHADER_DIR`, `./shaders/`, or `$(PREFIX)/share/mu_anim/shaders/`.

## Running the `.anim` DSL & Showcase Binaries

```bash
# Render any .anim script to MP4, PNG, or interactive preview window
./anim_dsl examples/calculus_tangent.anim --video calculus_tangent.mp4
./anim_dsl examples/uniform_circular_motion.anim --shot preview.png --time 5.0
./anim_dsl examples/text_animations_showcase.anim   # interactive preview window

# Export the 24.45s 5-act Real-Time Graphics Pipeline explainer MP4 (1280x720@60fps, AgX + 2x motion blur)
./graphics_pipeline_demo --video graphics_pipeline.mp4 --fps 60 --blur 2

# Export the 12.7s Physics & CS toolkit showcase MP4 (1280x720@60fps, 2x motion blur)
./physics_cs_demo --video physics_cs_demo.mp4 --fps 60 --blur 2

# Export a single PNG frame (headless Vulkan, with validation layers)
./mu_anim --shot frame.png --time 11.0 --validate 1 --stats 1

# Export the 15.6s 4-act 1280x720@60fps showcase MP4 with 2x temporal motion blur
./mu_anim --video demo.mp4 --fps 60 --width 1280 --height 720 --blur 2

# Export 1080p60 / 4K / vertical social preset
./mu_anim --video out_1080p.mp4 --preset 1080p --blur 2

# Launch interactive preview window
./mu_anim
```

*(Note: In headless environments without a hardware GPU, set `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json` to use Mesa Lavapipe.)*

## Recompiling Shaders (Optional)

All 11 precompiled SPIR-V binaries live in `shaders/anim.*.spv`. If `shaders/anim.slang` is edited, run `make shaders` (requires `slangc` on `PATH`).
# mu_anim
# mu_anim
