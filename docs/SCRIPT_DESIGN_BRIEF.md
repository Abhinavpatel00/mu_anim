# `mu_anim` Scripting Language Design Brief

A concise overview of the `mu_anim` C11/Vulkan/Slang animation engine (`anim.h`) for gathering feedback on designing an ergonomic, minimal scripting language on top of it.

---

## 1. What `mu_anim` Does Under the Hood

`mu_anim` is a GPU-accelerated mathematical & technical animation engine (C11 + Vulkan + Slang) with:
- **Band-accelerated quadratic Bézier vector rendering** (resolution-independent fills, strokes, dashes, gradients, drop shadows, analytic clip rects, even-odd CSG holes).
- **Z-buffered 3D PBR surfaces & solids** (Cook-Torrance GGX specular + `fwidth` UV wireframe + 3D camera-facing billboards + perspective frustums).
- **Post-FX & camera**: 4-mip HDR bloom, Soft/ACES/AgX tone mapping, chromatic aberration, film grain, temporal motion blur (`--blur N`), and timeline-animatable Picture-in-Picture (PIP) zoom insets.
- **Typst & LaTeX (`anim_tex`)** SVG equation compilation with token isolation (`TransformMatchingTex`) + `stb_truetype` UTF-8 text.
- **Execution model**:
  1. `AnimScene` holds generational `AnimId` mobjects, `AnimTrackerId` scalar `ValueTracker`s, and a **topologically sorted updater DAG** (`anim_add_updater`).
  2. `AnimTimeline` records snapshots around `anim_play` / `anim_play_at` / `anim_play_lagged` / `anim_play_sequence` / `anim_wait`, supporting O(log N) timeline scrubbing (`anim_seek`).

---

## 2. Why C Is Too Verbose (The Problem to Solve)

Writing animations directly against `anim.h` currently takes ~350–700 lines of C per video because of:
1. **Manual `AnimSpec[]` arrays** for every parallel beat.
2. **Boilerplate updater structs + callbacks** just to tie a dot/arrow to a `ValueTracker`.
3. **Manual `anim_add` / `anim_remove` timing** so future objects don't appear early or linger invisibly.
4. **Separate constructor + setter calls** (`anim_move_to`, `anim_set_fill`, `anim_set_stroke`, `anim_set_glow`).
5. **Double-escaped LaTeX strings** in C (`"\\mathbf{v}(t) = \\frac{d\\mathbf{r}}{dt}"`).

---

## 3. Engine Surface the Script Language Must Expose (`anim.h`)

| Domain | Core `anim.h` Primitives & Operations |
| :--- | :--- |
| **2D Geometry & CSG** | `circle`, `arc`, `annulus`, `sector`, `line`, `dashed_line`, `arrow`, `double_arrow`, `rect`, `rounded_rect`, `polygon`, `regular_poly`, `dot`, `angle`, `right_angle`, `surrounding_rect`, `background_rect`, `underline`, `cross`, `traced_path`, `bool_op` (Union/Intersect/Diff/XOR), `convex_hull`, `voronoi`, `pixel_grid` |
| **Graphing & Fields** | `number_line`, `axes` (`c2p`/`p2c`), `polar_plane`, `plot`, `plot_polar`, `area_under_curve`, `riemann_rectangles`, `parametric_curve`, `implicit_plot`, `contour_plot`, `vector_field`, `stream_lines`, `particle_system` |
| **3D PBR & Camera** | `surface3d`, `sphere`, `torus`, `cube`, `prism`, `cylinder`, `cone`, `polyhedron`, `line3d`, `arrow3d`, `dot3d`, `frustum3d`, `axes3d`, `camera_to`, `camera_orbit`, `zoom_inset_to` (PIP), `set_billboard` |
| **Text, Math & UI** | `text`, `paragraph`, `bulleted_list`, `code` (+ line highlight), `table`, `graph_network`, `tex` (Typst/LaTeX), `svg`, `decimal_number`, `variable` |
| **Physics & CS Toolkits** | `phys_step_rk4`, `phys_trajectory`, `phys_spring`, `phys_pendulum`, `phys_free_body`, `phys_wave`, `phys_charge`, `phys_field_lines`, `phys_lens`, `phys_ray_bundle`; `cs_array`, `cs_bar_chart`, `cs_swap_mobs`, `cs_linked_list`, `cs_stack_queue`, `cs_binary_tree`, `cs_fsm`, `cs_logic_gate`, `cs_bit_register`, `cs_neural_net` |
| **Transforms & Styling** | `move_to`, `shift`, `next_to`, `align_to`, `arrange`, `scale`, `rotate`, `set_fill`, `set_stroke`, `set_gradient`, `set_dash`, `set_glow`, `set_shadow`, `set_clip_rect`, `set_opacity` |
| **Animations** | `create`, `uncreate`, `write`, `unwrite`, `draw_border_then_fill`, `fade_in`, `fade_out`, `grow_from_center`, `grow_from_edge`, `grow_arrow`, `spin_in`, `shrink_to_center`, `indicate`, `circumscribe`, `passing_flash`, `wiggle`, `flash`, `focus_on`, `broadcast`, `transform`, `replacement_transform`, `transform_matching_tex`, `follow_path`, `move_arc`, `apply_warp`, `homotopy`, `apply_wave`, `type_with_cursor`, `show_increasing_subsets` |
| **Scheduling & Post-FX** | `play`, `play_at`, `play_lagged`, `play_sequence`, `wait`, 14 rate functions (including `spring` & cubic Bézier), `tonemap` (`soft`/`aces`/`agx`), `bloom`, `aberration_px`, `grain_amount`, `motion_blur_samples` |

---

## 4. Current Strawman Syntax Direction (Work in Progress)

**Design goals chosen so far:**
- **Zero Python-style indentation rules**: curly braces `{ ... }` delimit blocks; newlines or `;` separate statements.
- **Fluent pipe-chaining (`|>`)** for building & styling objects left-to-right, plus `{ key: val }` config bags for multi-option builders (`scene`, `axes`).
- **Reactive `:=` math bindings** that auto-compile into `ValueTracker` + topological updaters, plus an imperative `on update(dt) { ... }` escape hatch.
- **Raw backtick strings** `` `...` `` for LaTeX/Typst so backslashes never need double-escaping.
- **Lazy scene lifecycle**: objects enter the scene automatically on their first `write`/`fade_in`/`grow`/`show` and leave on `fade_out`/`unwrite`/`replace`.

### Example: Uniform Circular Motion (~515 lines of C → ~42 lines of script)

```text
scene { bg: #0B0F18 -> #141D30, tonemap: agx, blur: 2, fps: 60 }

C = [-3.35, -0.22]; R = 1.92
th = tracker(0.55)

// Reactive kinematics (auto-wired into mu_anim's topological updater DAG)
u  := [cos(th), sin(th)]
t  := [-sin(th), cos(th)]
p  := C + R * u
vt := p + 1.46 * t
at := p - 1.12 * u

// Objects & styles
title = text("Uniform Circular Motion: Calculus Derivation", 0.44) |> at([0, 3.05]) |> glow(0.22)
ax    = axes { x: [-2.5, 2.5], y: [-2.5, 2.5], size: [4.7, 4.7] } |> at(C) |> stroke(#738CB88C, 1.6)
circ  = circle(R) |> at(C) |> stroke(#8CA6CC66, 1.8) |> dash(0.14, 0.10)
card  = round_rect(5.9, 5.3, 0.2) |> at([3.05, -0.22, -0.1]) |> fill(#04060EE6) |> stroke(#5280C7CC, 2)

p_dot = dot(0.11) |> at(p) |> fill(#F9E26B) |> glow(0.95)
trail = traced_path(p_dot, #F9E26B, 3.0)
r_vec = arrow(C, p)  |> color(#38BDF8) |> stroke(3.4)
v_vec = arrow(p, vt) |> color(#4ADE80) |> stroke(3.4)
a_vec = arrow(p, at) |> color(#FB7185) |> stroke(4.2)
arc_t = arc(0.46, 0, th) |> at(C) |> stroke(#F59E0B, 2.2)
perp  = right_angle(p - 0.22*u, p, p + 0.22*t) |> stroke(#F59E0B, 2.0)

lbl_r = tex(`\mathbf{r}(t)`, 0.34) |> at(C + 0.42*R*u - 0.30*t) |> color(#38BDF8)
lbl_v = tex(`\mathbf{v}(t)`, 0.34) |> at(vt + 0.28*t + 0.24*u)  |> color(#4ADE80)
lbl_a = tex(`\mathbf{a}(t)`, 0.34) |> at(p - 0.56*1.12*u + 0.32*t) |> color(#FB7185)

eq1 = tex(`\mathbf{r}(t) = R\begin{bmatrix}\cos(\omega t)\\ \sin(\omega t)\end{bmatrix}`, 0.78) |> at([3.05, 1.55]) |> color(#38BDF8)
eq2 = tex(`\mathbf{v}(t) = \frac{d\mathbf{r}}{dt} = R\omega\begin{bmatrix}-\sin(\omega t)\\ \cos(\omega t)\end{bmatrix}`, 0.78) |> at([3.05, -0.10]) |> color(#4ADE80)
eq3 = tex(`\mathbf{a}(t) = \frac{d\mathbf{v}}{dt} = -R\omega^2\begin{bmatrix}\cos(\omega t)\\ \sin(\omega t)\end{bmatrix} = -\omega^2\mathbf{r}(t)`, 0.78) |> at([3.05, -1.75]) |> color(#FB7185)

hl_v  = tex(`v = \|\mathbf{v}(t)\| = \omega R`, 0.54) |> at([3.05, 0.90]) |> color(#4ADE80)
hl_ac = tex(`a_c = \|\mathbf{a}(t)\| = \omega^2 R = \frac{v^2}{R}`, 0.66) |> at([3.05, -0.52]) |> color(#FB7185)

// Synchronized choreography
play(1.0s)         { write(title); fade_in(ax, card); create(circ) }
play(2.6s)         { grow(p_dot); show(trail); grow_arrow(r_vec); create(arc_t); fade_in(lbl_r); write(eq1); th -> 1.45 }
play(3.2s)         { grow_arrow(v_vec); create(perp); fade_in(lbl_v); write(eq2); camera -> [-0.15, 0.05, 8.75]; th -> 2.65 }
play(1.5s, linear) { circumscribe(eq2, #4ADE80); th -> 3.45 }
play(3.3s)         { camera -> default; grow_arrow(a_vec); fade_in(lbl_a); write(eq3); th -> 5.20 }
play(2.3s, linear) { circumscribe(eq3, #FB7185); th -> 7.10 }
play(0.6s)         { fade_out(eq1, eq2, eq3) }
play(2.2s)         { write(hl_v, hl_ac); camera -> [0.20, -0.05, 9.35]; th -> 8.40 }
play(1.8s)         { frame(hl_v, #4ADE80); frame(hl_ac, #F9E26B); th -> 9.65 }
play(1.4s, linear) { th -> 10.45 }
```

---

## 5. Open Questions for Reviewers

1. **Syntax & Readability:** Does `|>`, `{ key: val }` config bags, and `{ ... }` timeline blocks hit the right balance, or would you change anything (e.g., `@` for positioning, `.method()` instead of `|>`, or explicit `let`)?
2. **Reactivity Model:** Is automatic dependency propagation from `:=` formulas into `arrow(C, p)` / `at(p)` intuitive, or could implicit reactivity surprise users compared to marking reactive calls explicitly?
3. **Timeline Syntax:** Inside `play(duration, rate) { ... }`, is verb-first (`write(eq1); th -> 1.45`) or pipe-first (`eq1 |> write(); th |> to(1.45)`) cleaner—and how should `lagged` / `sequence` / overlapping `at` cues look?
4. **Abstraction & Reuse:** What minimal control flow (`for`, `if`, `fn`/templates, arrays/lists) is essential without turning a compact animation DSL into a bloated general-purpose language?
5. **Implementation Architecture:** For embedding in a C11 codebase (`anim_script.c`), would you prefer a tiny single-pass AST/bytecode interpreter or compiling down to a quick C11 / Lua / Wren / custom VM?
