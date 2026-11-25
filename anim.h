#ifndef MU_ANIM_H
#define MU_ANIM_H

/*
 * anim.h — Manim-grade vector & math animation engine on mu_lib + mu_new.
 * ============================================================================
 *  Follows mu_lib DOD conventions:
 *    - Generational handles (AnimId) with zero-initialized slot 0 sentinel.
 *    - Flat SoA/hot-cold tables, explicit state passing (AnimScene/AnimTimeline).
 *    - Analytical quadratic Bezier fill (band-accelerated winding + exact SDF AA)
 *      and variable-width SDF stroke rendered on Vulkan via vk.h + anim.slang.
 *    - SVG, stb_truetype plain text, and Typst / LaTeX (dvisvgm) math backends
 *      with semantic {{token}} part extraction and mu_save CRC32C disk caching.
 *    - Eager snapshot-backed seekable timeline + mu_toposort ValueTracker DAG.
 * ==========================================================================*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations from mu_new/vk.h and platform.h so anim.h stays light. */
typedef struct Gpu      Gpu;
typedef struct Image    Image;
typedef struct Platform Platform;

enum {
    ANIM_MAX_DEPS        = 8,
    ANIM_MAX_PARTS       = 128,
    ANIM_MAX_VIEWPORTS   = 8,
    ANIM_TAG_LEN         = 48,
    ANIM_PATH_LEN        = 512,
    ANIM_FRAME_PX_W      = 1280,
    ANIM_FRAME_PX_H      = 720,
    ANIM_DEFAULT_FPS     = 30
};

#define ANIM_WORLD_H          8.0f
#define ANIM_WORLD_W_16_9     (14.222222f)
#define ANIM_PI               (3.14159265358979323846f)
#define ANIM_TAU              (6.28318530717958647692f)
#define ANIM_DEFAULT_STROKE_W 4.0f

/* ------------------------------------------------------------------ handles */

typedef struct AnimId {
    uint32_t index;
    uint32_t gen;
} AnimId;

#define ANIM_ID_NIL       ((AnimId){ 0u, 0u })
#define ANIM_ID_VALID(id) ((id).index != 0u)

typedef struct AnimTrackerId {
    uint32_t index;
    uint32_t gen;
} AnimTrackerId;

#define ANIM_TRACKER_NIL       ((AnimTrackerId){ 0u, 0u })
#define ANIM_TRACKER_VALID(id) ((id).index != 0u)

/* ---------------------------------------------------------- hot value types */

typedef struct AnimVec3 {
    float x, y, z;
} AnimVec3;

typedef struct AnimColor {
    float r, g, b, a; /* linear RGBA */
} AnimColor;

typedef struct AnimCubic {
    AnimVec3 p0, p1, p2, p3;
} AnimCubic;

#define ANIM_V3(x, y, z) ((AnimVec3){ (float)(x), (float)(y), (float)(z) })
#define ANIM_ORIGIN      ANIM_V3(0.0f, 0.0f, 0.0f)
#define ANIM_UP          ANIM_V3(0.0f, 1.0f, 0.0f)
#define ANIM_DOWN        ANIM_V3(0.0f, -1.0f, 0.0f)
#define ANIM_LEFT        ANIM_V3(-1.0f, 0.0f, 0.0f)
#define ANIM_RIGHT       ANIM_V3(1.0f, 0.0f, 0.0f)
#define ANIM_OUT         ANIM_V3(0.0f, 0.0f, 1.0f)
#define ANIM_IN          ANIM_V3(0.0f, 0.0f, -1.0f)
#define ANIM_UL          ANIM_V3(-1.0f, 1.0f, 0.0f)
#define ANIM_UR          ANIM_V3(1.0f, 1.0f, 0.0f)
#define ANIM_DL          ANIM_V3(-1.0f, -1.0f, 0.0f)
#define ANIM_DR          ANIM_V3(1.0f, -1.0f, 0.0f)

/* --------------------------------------------------------- colors & easing */

AnimColor anim_rgba(float r, float g, float b, float a);
AnimColor anim_srgb_to_linear(float r, float g, float b, float a);
AnimColor anim_hex(uint32_t rgb_hex);
AnimColor anim_hex_a(uint32_t rgb_hex, float alpha);
AnimColor anim_color_lerp_oklab(AnimColor a, AnimColor b, float t);

#define ANIM_BG_3B1B  anim_hex(0x0F111A)
#define ANIM_BLACK    anim_hex(0x000000)
#define ANIM_WHITE    anim_hex(0xFFFFFF)
#define ANIM_GREY_A   anim_hex(0xDDDDDD)
#define ANIM_GREY_B   anim_hex(0xBBBBBB)
#define ANIM_GREY_C   anim_hex(0x888888)
#define ANIM_GREY_D   anim_hex(0x444444)
#define ANIM_BLUE_A   anim_hex(0xC7E9F1)
#define ANIM_BLUE_B   anim_hex(0x9CDCEB)
#define ANIM_BLUE_C   anim_hex(0x58C4DD)
#define ANIM_BLUE_D   anim_hex(0x29ABCA)
#define ANIM_BLUE_E   anim_hex(0x236B8E)
#define ANIM_BLUE     ANIM_BLUE_C
#define ANIM_TEAL     anim_hex(0x5CD0B3)
#define ANIM_GREEN_A  anim_hex(0xC9E2AE)
#define ANIM_GREEN_C  anim_hex(0x83C167)
#define ANIM_GREEN_E  anim_hex(0x699C52)
#define ANIM_GREEN    ANIM_GREEN_C
#define ANIM_YELLOW_A anim_hex(0xFFF1B6)
#define ANIM_YELLOW_C anim_hex(0xFFFF00)
#define ANIM_YELLOW   anim_hex(0xF9E26B)
#define ANIM_GOLD     anim_hex(0xF0AC5F)
#define ANIM_RED_A    anim_hex(0xF7A1A3)
#define ANIM_RED_C    anim_hex(0xFC6255)
#define ANIM_RED_E    anim_hex(0xCF5044)
#define ANIM_RED      ANIM_RED_C
#define ANIM_MAROON   anim_hex(0xC55F73)
#define ANIM_PURPLE   anim_hex(0x9A72AC)
#define ANIM_PINK     anim_hex(0xD147BD)

typedef enum AnimRateKind {
    ANIM_RATE_LINEAR = 0,
    ANIM_RATE_SMOOTH,
    ANIM_RATE_DOUBLE_SMOOTH,
    ANIM_RATE_RUSH_INTO,
    ANIM_RATE_RUSH_FROM,
    ANIM_RATE_THERE_AND_BACK,
    ANIM_RATE_THERE_AND_BACK_PAUSE,
    ANIM_RATE_WIGGLE,
    ANIM_RATE_LINGERING,
    ANIM_RATE_RUNNING_START,
    ANIM_RATE_EASE_OUT_BACK,
    ANIM_RATE_EASE_IN_OUT_CUBIC,
    ANIM_RATE_SPRING
} AnimRateKind;

typedef float (*AnimRateFn)(float t, void *user);

float anim_rate_eval(AnimRateKind kind, float t);
float anim_rate_eval_spring(float t, float damping, float freq);

/* Thread-local / global diagnostic string for production error inspection. */
const char *anim_last_error(void);
void        anim_clear_error(void);

/* ----------------------------------------------------------- style & camera */

typedef enum AnimCapStyle {
    ANIM_CAP_ROUND = 0,
    ANIM_CAP_BUTT  = 1,
    ANIM_CAP_SQUARE = 2
} AnimCapStyle;

typedef enum AnimJoinStyle {
    ANIM_JOIN_ROUND = 0,
    ANIM_JOIN_MITER = 1,
    ANIM_JOIN_BEVEL = 2
} AnimJoinStyle;

typedef enum AnimFillRule {
    ANIM_FILL_NONZERO  = 0,
    ANIM_FILL_EVEN_ODD = 1
} AnimFillRule;

typedef struct AnimCamera {
    AnimVec3 pos;
    AnimVec3 target;
    AnimVec3 up;
    float    fov_deg;     /* vertical FOV in degrees (default 45)          */
    float    frame_height;/* world units matching viewport height (8.0)    */
    float    zoom;        /* multiplier (1.0 = default)                    */
    float    ortho_blend; /* 0.0 = perspective, 1.0 = orthographic (0.0)   */
} AnimCamera;

AnimCamera anim_camera_default(void);
void       anim_camera_view_proj(const AnimCamera *cam, float aspect, float out_vp[16], float out_view[16]);

/* --------------------------------------------------------------- scene & VM */

typedef struct AnimScene    AnimScene;
typedef struct AnimTimeline AnimTimeline;
typedef struct AnimRenderer AnimRenderer;

typedef struct AnimComplex {
    float re, im;
} AnimComplex;

typedef AnimVec3    (*AnimWarpFn)(AnimVec3 p, void *user);
typedef AnimComplex (*AnimComplexFn)(AnimComplex z, void *user);
typedef AnimVec3    (*AnimHomotopyFn)(AnimVec3 p, float t, void *user);
typedef float    (*AnimImplicitFn)(float x, float y, void *user);
typedef void     (*AnimCustomAnimFn)(AnimScene *scene, AnimId target, float alpha, void *user);
typedef void     (*AnimUpdaterFn)(AnimScene *scene, AnimId target, float dt, void *user);
typedef float    (*AnimScalarFn)(float x, void *user);
typedef AnimVec3 (*AnimFieldFn)(AnimVec3 p, void *user);
typedef AnimVec3 (*AnimParamFn)(float t, void *user);
typedef AnimVec3 (*AnimSurfaceFn)(float u, float v, void *user);
typedef AnimColor (*AnimSurfaceColorFn)(float u, float v, AnimVec3 p, AnimVec3 n, void *user);

AnimScene *anim_scene_create(void);
void       anim_scene_destroy(AnimScene *scene);
void       anim_scene_clear(AnimScene *scene);

void       anim_scene_set_camera(AnimScene *scene, AnimCamera cam);
AnimCamera anim_scene_camera(const AnimScene *scene);
void       anim_scene_begin_ambient_camera_rotation(AnimScene *scene, float d_azimuth_rad_per_sec, float d_elevation_rad_per_sec);
void       anim_scene_stop_ambient_camera_rotation(AnimScene *scene);
void       anim_scene_begin_illusion_camera_rotation(AnimScene *scene, float rate_rad_per_sec, float wobble_rad);
void       anim_scene_set_zoom_inset(AnimScene *scene, bool enable, AnimCamera cam, const float rect[4], AnimColor border);

typedef struct AnimViewport {
    bool       active;
    AnimCamera cam;
    float      rect[4];      /* normalized [x0, y0, x1, y1] in [0..1]       */
    AnimColor  border;       /* border color (alpha == 0 disables border)   */
    float      border_width; /* border thickness in px (default 2.0)        */
    bool       draw_bg;      /* clear/draw scene bg gradient inside rect    */
} AnimViewport;

uint32_t anim_scene_add_viewport(AnimScene *scene, AnimCamera cam, const float rect[4], AnimColor border, float border_width);
void     anim_scene_set_viewport(AnimScene *scene, uint32_t idx, bool active, AnimCamera cam, const float rect[4], AnimColor border, float border_width);
void     anim_scene_clear_viewports(AnimScene *scene);
uint32_t anim_scene_viewport_count(const AnimScene *scene);

void       anim_scene_set_bg(AnimScene *scene, AnimColor top, AnimColor bottom);
void       anim_scene_set_cache_dir(AnimScene *scene, const char *dir_path);

typedef enum AnimLightKind {
    ANIM_LIGHT_DIRECTIONAL = 0,
    ANIM_LIGHT_POINT
} AnimLightKind;

typedef struct AnimLight {
    AnimLightKind kind;
    AnimVec3      pos_or_dir;
    AnimColor     color;
    float         intensity;
} AnimLight;

typedef struct AnimFogDesc {
    bool      enabled;
    bool      exponential;
    AnimColor color;
    float     start;
    float     end;
    float     density;
} AnimFogDesc;

void       anim_scene_clear_lights(AnimScene *scene);
void       anim_scene_add_light(AnimScene *scene, AnimLightKind kind, AnimVec3 pos_or_dir, AnimColor color, float intensity);
void       anim_scene_set_ambient_light(AnimScene *scene, AnimColor color, float intensity);
void       anim_scene_set_fog(AnimScene *scene, const AnimFogDesc *fog);

AnimCamera anim_camera_frame_mob(const AnimScene *scene, AnimId target, float padding, float aspect);
void       anim_camera_move_to(AnimCamera *cam, AnimVec3 target);
void       anim_scene_camera_save(AnimScene *scene);
void       anim_scene_camera_restore(AnimScene *scene);
void       anim_scene_camera_set_spherical(AnimScene *scene, AnimVec3 target, float distance, float phi_rad, float theta_rad);
float      anim_scene_time(const AnimScene *scene);

/* VMobject lifecycle & path editing */
AnimId anim_vmob_create(AnimScene *scene);
void   anim_vmob_destroy(AnimScene *scene, AnimId id);
bool   anim_vmob_alive(const AnimScene *scene, AnimId id);
AnimId anim_vmob_clone(AnimScene *scene, AnimId id);

void   anim_vmob_clear_paths(AnimScene *scene, AnimId id);
void   anim_vmob_begin_subpath(AnimScene *scene, AnimId id, AnimVec3 start);
void   anim_vmob_line_to(AnimScene *scene, AnimId id, AnimVec3 end);
void   anim_vmob_quad_to(AnimScene *scene, AnimId id, AnimVec3 ctrl, AnimVec3 end);
void   anim_vmob_cubic_to(AnimScene *scene, AnimId id, AnimVec3 c1, AnimVec3 c2, AnimVec3 end);
void   anim_vmob_close_subpath(AnimScene *scene, AnimId id);
void   anim_vmob_add_cubic(AnimScene *scene, AnimId id, AnimCubic c, bool new_subpath);

uint32_t anim_vmob_subpath_count(const AnimScene *scene, AnimId id);
uint32_t anim_vmob_cubic_count(const AnimScene *scene, AnimId id);
AnimVec3 anim_vmob_point_at(const AnimScene *scene, AnimId id, float t);
AnimVec3 anim_vmob_tangent_at(const AnimScene *scene, AnimId id, float t);
float    anim_vmob_arc_length(const AnimScene *scene, AnimId id);
void     anim_vmob_subdivide(AnimScene *scene, AnimId id, uint32_t cuts_per_cubic);
void     anim_vmob_apply_warp(AnimScene *scene, AnimId id, AnimWarpFn fn, void *user, uint32_t subdiv_cuts);
void     anim_vmob_apply_complex_fn(AnimScene *scene, AnimId id, AnimComplexFn fn, void *user, uint32_t subdiv_cuts);
void     anim_save_state(AnimScene *scene, AnimId id);
void     anim_vmob_set_stroke_gradient(AnimScene *scene, AnimId id, AnimColor c_start, AnimColor c_end);

/* Styling */
void anim_set_fill(AnimScene *scene, AnimId id, AnimColor color, bool recursive);
void anim_set_fill_gradient_linear(AnimScene *scene, AnimId id, AnimColor c0, AnimColor c1, AnimVec3 world_p0, AnimVec3 world_p1, bool recursive);
void anim_set_fill_gradient_radial(AnimScene *scene, AnimId id, AnimColor c_inner, AnimColor c_outer, AnimVec3 world_center, float world_radius, bool recursive);
void anim_set_fill_gradient_stops(AnimScene *scene, AnimId id, const AnimColor *stops, uint32_t stop_count, AnimVec3 world_p0, AnimVec3 world_p1, bool radial, float world_radius, bool recursive);
void anim_clear_fill_gradient(AnimScene *scene, AnimId id, bool recursive);
void anim_set_stroke(AnimScene *scene, AnimId id, AnimColor color, float width_pt, bool recursive);
void anim_set_background_stroke(AnimScene *scene, AnimId id, AnimColor color, float width_pt, bool recursive);
void anim_set_dash(AnimScene *scene, AnimId id, float dash_px, float gap_px, float offset_px, bool recursive);
void anim_set_shadow(AnimScene *scene, AnimId id, float shadow_px, bool recursive);
void anim_set_clip_rect(AnimScene *scene, AnimId id, AnimVec3 world_min, AnimVec3 world_max, bool recursive);
void anim_clear_clip(AnimScene *scene, AnimId id, bool recursive);
void anim_set_color(AnimScene *scene, AnimId id, AnimColor color, bool recursive);
void anim_set_opacity(AnimScene *scene, AnimId id, float opacity, bool recursive);
void anim_set_glow(AnimScene *scene, AnimId id, float glow, bool recursive);
void anim_set_sheen(AnimScene *scene, AnimId id, float sheen, float angle_rad, bool recursive);
void anim_set_sheen_color(AnimScene *scene, AnimId id, float sheen, float angle_rad, AnimColor color, bool recursive);
void anim_set_z_index(AnimScene *scene, AnimId id, float z_index, bool recursive);
void anim_set_billboard(AnimScene *scene, AnimId id, bool billboard, bool recursive);
void anim_set_depth_test(AnimScene *scene, AnimId id, bool depth_test, bool recursive);
void anim_set_fixed_in_frame(AnimScene *scene, AnimId id, bool fixed_in_frame, bool recursive);
void anim_set_trim(AnimScene *scene, AnimId id, float start, float end, bool recursive);
void anim_set_visible(AnimScene *scene, AnimId id, bool visible, bool recursive);
void anim_set_cap_join(AnimScene *scene, AnimId id, AnimCapStyle cap, AnimJoinStyle join, bool recursive);
void anim_set_fill_rule(AnimScene *scene, AnimId id, AnimFillRule rule, bool recursive);
void anim_set_tag(AnimScene *scene, AnimId id, const char *tag);

/* Hierarchy */
void     anim_add(AnimScene *scene, AnimId id);
void     anim_add_child(AnimScene *scene, AnimId parent, AnimId child);
void     anim_remove(AnimScene *scene, AnimId id);
uint32_t anim_child_count(const AnimScene *scene, AnimId id);
AnimId   anim_child_at(const AnimScene *scene, AnimId id, uint32_t idx);
AnimId   anim_find_by_tag(const AnimScene *scene, AnimId root, const char *tag);

/* Transforms & Manim-style layout */
AnimVec3 anim_pos(const AnimScene *scene, AnimId id);
void     anim_set_pos(AnimScene *scene, AnimId id, AnimVec3 pos);
void     anim_bbox_world(const AnimScene *scene, AnimId id, AnimVec3 *out_min, AnimVec3 *out_max);
AnimVec3 anim_center(const AnimScene *scene, AnimId id);
float    anim_width(const AnimScene *scene, AnimId id);
float    anim_height(const AnimScene *scene, AnimId id);

void anim_move_to(AnimScene *scene, AnimId id, AnimVec3 target);
void anim_shift(AnimScene *scene, AnimId id, AnimVec3 delta);
void anim_scale(AnimScene *scene, AnimId id, float factor);
void anim_scale_vec(AnimScene *scene, AnimId id, AnimVec3 factors);
void anim_scale_to_width(AnimScene *scene, AnimId id, float width);
void anim_scale_to_height(AnimScene *scene, AnimId id, float height);
void anim_rotate(AnimScene *scene, AnimId id, float angle_rad, AnimVec3 axis);
void anim_next_to(AnimScene *scene, AnimId id, AnimId ref, AnimVec3 dir, float buff);
void anim_align_to(AnimScene *scene, AnimId id, AnimId ref, AnimVec3 dir);
void anim_arrange(AnimScene *scene, const AnimId *ids, uint32_t count, AnimVec3 dir, float buff);
void anim_arrange_in_grid(AnimScene *scene, const AnimId *ids, uint32_t count, uint32_t rows, uint32_t cols, float h_buff, float v_buff);
void anim_to_edge(AnimScene *scene, AnimId id, AnimVec3 dir, float buff);
void anim_to_corner(AnimScene *scene, AnimId id, AnimVec3 corner, float buff);

/* --------------------------------------------------------- standard library */

AnimId anim_group(AnimScene *scene, const AnimId *children, uint32_t count);
AnimId anim_line(AnimScene *scene, AnimVec3 a, AnimVec3 b);
AnimId anim_dashed_line(AnimScene *scene, AnimVec3 a, AnimVec3 b, float dash_len, float gap_len);
AnimId anim_arrow(AnimScene *scene, AnimVec3 a, AnimVec3 b, float tip_len, float tip_width);
AnimId anim_double_arrow(AnimScene *scene, AnimVec3 a, AnimVec3 b, float tip_len, float tip_width);
AnimId anim_vector(AnimScene *scene, AnimVec3 dir);
AnimId anim_arc(AnimScene *scene, float radius, float start_angle, float angle);
AnimId anim_circle(AnimScene *scene, float radius);
AnimId anim_annulus(AnimScene *scene, float r_inner, float r_outer);
AnimId anim_ellipse(AnimScene *scene, float width, float height);
AnimId anim_rect(AnimScene *scene, float width, float height);
AnimId anim_rounded_rect(AnimScene *scene, float width, float height, float corner_radius);
AnimId anim_square(AnimScene *scene, float side);
AnimId anim_polygon(AnimScene *scene, const AnimVec3 *pts, uint32_t count);
AnimId anim_regular_polygon(AnimScene *scene, uint32_t sides, float radius);
AnimId anim_star(AnimScene *scene, uint32_t points, float r_outer, float r_inner);
AnimId anim_dot(AnimScene *scene, AnimVec3 pos, float radius);
AnimId anim_brace(AnimScene *scene, AnimId target, AnimVec3 dir, float buff);
AnimId anim_cubic_bezier(AnimScene *scene, AnimVec3 p0, AnimVec3 p1, AnimVec3 p2, AnimVec3 p3);
AnimId anim_arc_between_points(AnimScene *scene, AnimVec3 a, AnimVec3 b, float angle_rad);
AnimId anim_sector(AnimScene *scene, float r_inner, float r_outer, float start_angle, float angle);
AnimId anim_angle(AnimScene *scene, AnimVec3 a_start, AnimVec3 vertex, AnimVec3 b_end, float radius);
AnimId anim_right_angle(AnimScene *scene, AnimVec3 a_start, AnimVec3 vertex, AnimVec3 b_end, float length);
AnimId anim_surrounding_rect(AnimScene *scene, AnimId target, float buff, float corner_radius);
AnimId anim_background_rect(AnimScene *scene, AnimId target, float buff, AnimColor color);
AnimId anim_underline(AnimScene *scene, AnimId target, float buff);
AnimId anim_cross(AnimScene *scene, AnimId target, float stroke_width);
AnimId anim_traced_path(AnimScene *scene, AnimId target, AnimColor color, float stroke_width, uint32_t max_points);
AnimId anim_cutout(AnimScene *scene, AnimId outer, const AnimId *holes, uint32_t hole_count);
AnimId anim_rounded_polygon(AnimScene *scene, const AnimVec3 *pts, uint32_t count, float corner_radius);
AnimId anim_elbow(AnimScene *scene, float width, float angle_rad);
AnimId anim_arc_polygon(AnimScene *scene, const AnimVec3 *pts, const float *arc_angles, uint32_t count);
AnimId anim_dashed_vmobject(AnimScene *scene, AnimId src_vmob, uint32_t num_dashes, float dashed_ratio);
AnimId anim_labeled_dot(AnimScene *scene, const char *label, const char *font_path, float radius, AnimColor bg_col, AnimColor fg_col);
AnimId anim_curved_arrow(AnimScene *scene, AnimVec3 start, AnimVec3 end, float angle_rad, float tip_len);
AnimId anim_curved_double_arrow(AnimScene *scene, AnimVec3 start, AnimVec3 end, float angle_rad, float tip_len);
AnimId anim_integer(AnimScene *scene, int value, const char *font_path, float height);
AnimId anim_callout(AnimScene *scene, AnimVec3 target_pt, AnimVec3 box_center, const char *label, const char *font_path, AnimColor accent_color);
AnimId anim_dimension_line(AnimScene *scene, AnimVec3 a, AnimVec3 b, float offset, const char *label, const char *font_path, AnimColor color);

typedef enum AnimBoolOp {
    ANIM_BOOL_UNION = 0,
    ANIM_BOOL_INTERSECT,
    ANIM_BOOL_DIFFERENCE,
    ANIM_BOOL_XOR
} AnimBoolOp;

AnimId anim_bool_op(AnimScene *scene, AnimId a, AnimId b, AnimBoolOp op);
AnimId anim_convex_hull(AnimScene *scene, const AnimVec3 *pts, uint32_t count);
AnimId anim_voronoi(AnimScene *scene, const AnimVec3 *sites, uint32_t count, AnimVec3 bmin, AnimVec3 bmax, const AnimColor *colors);

typedef struct AnimAxesDesc {
    float    x_min, x_max, x_step;
    float    y_min, y_max, y_step;
    float    x_length;
    float    y_length;
    bool     include_ticks;
    bool     include_tips;
    bool     include_grid;
    bool     include_numbers;
    const char *font_path;
} AnimAxesDesc;

AnimId   anim_number_line(AnimScene *scene, float min_v, float max_v, float step, float length, bool include_ticks);
AnimId   anim_axes(AnimScene *scene, const AnimAxesDesc *desc);
AnimVec3 anim_axes_c2p(const AnimScene *scene, AnimId axes_id, float x, float y);
void     anim_axes_p2c(const AnimScene *scene, AnimId axes_id, AnimVec3 world_pt, float *out_x, float *out_y);
AnimId   anim_number_plane(AnimScene *scene, float x_min, float x_max, float x_step, float y_min, float y_max, float y_step,
                           float x_len, float y_len, bool faded_subgrid);
AnimId   anim_complex_plane(AnimScene *scene, float re_min, float re_max, float re_step, float im_min, float im_max, float im_step,
                            float x_len, float y_len, bool include_labels, const char *font_path);
AnimVec3 anim_complex_c2p(const AnimScene *scene, AnimId plane_id, float re, float im);
void     anim_complex_p2c(const AnimScene *scene, AnimId plane_id, AnimVec3 world_pt, float *out_re, float *out_im);
void     anim_number_plane_prepare_nonlinear(AnimScene *scene, AnimId plane_id, uint32_t cuts_per_cubic);

AnimId anim_plot(AnimScene *scene, AnimId axes_id, AnimScalarFn fn, float x_min, float x_max, uint32_t samples, void *user);
AnimId anim_tangent_line(AnimScene *scene, AnimId vmob_id, float alpha, float length);
AnimId anim_secant_slope_group(AnimScene *scene, AnimId axes_id, AnimScalarFn fn, float x, float dx, float line_length, void *user, AnimColor secant_col, AnimColor dx_col, AnimColor dy_col);
AnimId anim_area_under_curve(AnimScene *scene, AnimId axes_id, AnimScalarFn fn, float x_min, float x_max, uint32_t samples, void *user);
AnimId anim_riemann_rectangles(AnimScene *scene, AnimId axes_id, AnimScalarFn fn, float x_min, float x_max, float dx, void *user, AnimColor c_left, AnimColor c_right);
AnimId anim_vector_field(AnimScene *scene, AnimId axes_id, AnimFieldFn fn, float x_step, float y_step, float max_arrow_len, void *user);
AnimId anim_polar_plane(AnimScene *scene, float max_radius, float radius_step, uint32_t azimuth_divs, float size);
AnimId anim_plot_polar(AnimScene *scene, AnimId plane_id, AnimScalarFn r_fn, float theta_min, float theta_max, uint32_t samples, void *user);
AnimId anim_implicit_plot(AnimScene *scene, AnimId axes_id, AnimImplicitFn fn, float x_min, float x_max, float y_min, float y_max, uint32_t nx, uint32_t ny, void *user);
AnimId anim_contour_plot(AnimScene *scene, AnimId axes_id, AnimImplicitFn fn, float x_min, float x_max, float y_min, float y_max,
                         uint32_t nx, uint32_t ny, const float *levels, uint32_t num_levels, AnimColor c_low, AnimColor c_high, void *user);
AnimId anim_stream_lines(AnimScene *scene, AnimId axes_id, AnimFieldFn fn, float x_min, float x_max, float y_min, float y_max, float x_step, float y_step, uint32_t max_steps, void *user);
AnimId anim_animated_stream_lines(AnimScene *scene, AnimId axes_id, AnimFieldFn fn, float x_min, float x_max, float y_min, float y_max, float x_step, float y_step, uint32_t max_steps, float flow_speed, void *user);

typedef struct AnimBoxStats {
    float min_v;
    float q1;
    float median;
    float q3;
    float max_v;
} AnimBoxStats;

AnimId anim_scatter_plot(AnimScene *scene, AnimId axes_id, const float *xs, const float *ys, const float *radii, const AnimColor *colors, uint32_t count, float default_radius);
AnimId anim_bar_plot(AnimScene *scene, AnimId axes_id, const float *values, uint32_t categories, uint32_t series, bool stacked, const AnimColor *series_colors, float bar_group_width);
AnimId anim_box_plot(AnimScene *scene, AnimId axes_id, const AnimBoxStats *boxes, uint32_t count, float box_width, const AnimColor *colors);
AnimId anim_heatmap(AnimScene *scene, AnimId axes_id, const float *values, uint32_t cols, uint32_t rows, float v_min, float v_max, AnimColor c_low, AnimColor c_mid, AnimColor c_high);
void   anim_heatmap_set(AnimScene *scene, AnimId heatmap_id, const float *values, uint32_t cols, uint32_t rows, float v_min, float v_max, AnimColor c_low, AnimColor c_mid, AnimColor c_high);

AnimId anim_decimal_number(AnimScene *scene, float value, uint32_t num_decimals, const char *font_path, float height);
void   anim_decimal_number_set(AnimScene *scene, AnimId num_id, float value);
AnimId anim_variable(AnimScene *scene, const char *label, AnimTrackerId tracker, uint32_t decimals, const char *font_path, float height);
AnimId anim_matrix(AnimScene *scene, const float *values, uint32_t rows, uint32_t cols, uint32_t decimals, const char *font_path, float cell_height);

/* 3D Parametric Curves, Lit Z-Buffered Surfaces, Solids & Technical Widgets */
typedef enum AnimPolyhedronKind {
    ANIM_POLY_TETRAHEDRON = 0,
    ANIM_POLY_CUBE,
    ANIM_POLY_OCTAHEDRON,
    ANIM_POLY_DODECAHEDRON,
    ANIM_POLY_ICOSAHEDRON
} AnimPolyhedronKind;

AnimId anim_parametric_curve(AnimScene *scene, AnimParamFn fn, float t_min, float t_max, uint32_t samples, void *user);
AnimId anim_surface3d(AnimScene *scene, AnimSurfaceFn fn, AnimSurfaceColorFn color_fn,
                      float u_min, float u_max, float v_min, float v_max,
                      uint32_t u_steps, uint32_t v_steps, void *user);
AnimId anim_sphere(AnimScene *scene, float radius, uint32_t u_steps, uint32_t v_steps);
AnimId anim_torus(AnimScene *scene, float r_major, float r_minor, uint32_t u_steps, uint32_t v_steps);
AnimId anim_cube(AnimScene *scene, float side);
AnimId anim_prism(AnimScene *scene, AnimVec3 dimensions);
AnimId anim_cylinder(AnimScene *scene, float radius, float height, uint32_t slices);
AnimId anim_cone(AnimScene *scene, float radius, float height, uint32_t slices);
AnimId anim_polyhedron(AnimScene *scene, AnimPolyhedronKind kind, float radius);
AnimId anim_surface_of_revolution(AnimScene *scene, AnimParamFn profile_fn, float t_min, float t_max,
                                  float theta_max, uint32_t t_steps, uint32_t theta_steps, void *user);
AnimId anim_extrude_vmob(AnimScene *scene, AnimId profile_2d, float depth, AnimColor side_col);
AnimId anim_line3d(AnimScene *scene, AnimVec3 a, AnimVec3 b, float radius, uint32_t slices);
AnimId anim_arrow3d(AnimScene *scene, AnimVec3 a, AnimVec3 b, float shaft_radius, float tip_radius, float tip_len, uint32_t slices);
AnimId anim_dot3d(AnimScene *scene, AnimVec3 pos, float radius);
AnimId anim_frustum3d(AnimScene *scene, float fov_deg, float aspect, float near_z, float far_z, AnimVec3 pos, AnimVec3 target);
AnimId anim_pixel_grid(AnimScene *scene, uint32_t cols, uint32_t rows, float cell_size, bool show_centers, AnimId *out_cells);
AnimId anim_particle_system(AnimScene *scene, uint32_t count, AnimVec3 origin, AnimVec3 vel_min, AnimVec3 vel_max,
                            AnimVec3 gravity, float life_max, AnimColor c_start, AnimColor c_end, float radius);
AnimId anim_axes3d(AnimScene *scene, float x_range, float y_range, float z_range);
AnimId anim_vector_field3d(AnimScene *scene, AnimFieldFn fn, AnimVec3 bmin, AnimVec3 bmax,
                           uint32_t nx, uint32_t ny, uint32_t nz, float max_arrow_len,
                           AnimColor c_low, AnimColor c_high, void *user);
AnimId anim_stream_lines3d(AnimScene *scene, AnimFieldFn fn, AnimVec3 bmin, AnimVec3 bmax,
                           uint32_t nx, uint32_t ny, uint32_t nz, float dt, uint32_t max_steps,
                           AnimColor c_start, AnimColor c_end, void *user);
void   anim_surface_set_wireframe(AnimScene *scene, AnimId id, float grid_density, float line_alpha, bool recursive);
void   anim_surface_set_roughness(AnimScene *scene, AnimId id, float roughness, bool recursive);
void   anim_surface_set_texture(AnimScene *scene, AnimId id, const char *path, bool lit, bool recursive);
void   anim_surface_set_checkerboard(AnimScene *scene, AnimId id, AnimColor c1, AnimColor c2, uint32_t u_divs, uint32_t v_divs, bool recursive);
void   anim_image_set_lit(AnimScene *scene, AnimId id, bool lit, bool recursive);

typedef enum AnimAlign {
    ANIM_ALIGN_LEFT = 0,
    ANIM_ALIGN_CENTER,
    ANIM_ALIGN_RIGHT
} AnimAlign;

AnimId anim_paragraph(AnimScene *scene, const char *font_path, const char *utf8, float line_height, float line_spacing, AnimAlign align);
AnimId anim_bulleted_list(AnimScene *scene, const char *font_path, const char *const *items, uint32_t count, float text_height, float line_spacing);
AnimId anim_code(AnimScene *scene, const char *font_path, const char *code_utf8, float font_height, bool line_numbers);
AnimId anim_code_highlight_line(AnimScene *scene, AnimId code_id, uint32_t line_1based, AnimColor highlight_color);
AnimId anim_table(AnimScene *scene, const char *const *cells, uint32_t rows, uint32_t cols, const char *font_path, float cell_height, bool header_row);
AnimId anim_table_cell(const AnimScene *scene, AnimId table_id, uint32_t row, uint32_t col);
typedef enum AnimGraphLayoutKind {
    ANIM_GRAPH_LAYOUT_CIRCULAR = 0,
    ANIM_GRAPH_LAYOUT_SPRING,
    ANIM_GRAPH_LAYOUT_GRID,
    ANIM_GRAPH_LAYOUT_TREE,
    ANIM_GRAPH_LAYOUT_BIPARTITE
} AnimGraphLayoutKind;

AnimId anim_graph_network(AnimScene *scene, uint32_t node_count, const AnimVec3 *positions, const char *const *labels,
                          uint32_t edge_count, const uint32_t *edge_from, const uint32_t *edge_to, bool directed, const char *font_path);
void   anim_graph_compute_layout(AnimGraphLayoutKind layout, uint32_t node_count,
                                 uint32_t edge_count, const uint32_t *edge_from, const uint32_t *edge_to,
                                 float width, float height, uint32_t seed, AnimVec3 *out_positions);
AnimId anim_graph_with_layout(AnimScene *scene, uint32_t node_count, const char *const *labels,
                              uint32_t edge_count, const uint32_t *edge_from, const uint32_t *edge_to,
                              bool directed, AnimGraphLayoutKind layout, float width, float height, const char *font_path);

typedef AnimColor (*AnimPixelFn)(uint32_t x, uint32_t y, uint32_t width, uint32_t height, void *user);

AnimId anim_pixel_array(AnimScene *scene, const uint8_t *rgba, uint32_t width, uint32_t height, float world_height);
AnimId anim_pixel_array_fn(AnimScene *scene, uint32_t width, uint32_t height, float world_height, AnimPixelFn fn, void *user);
void   anim_pixel_array_update(AnimScene *scene, AnimId id, const uint8_t *rgba, uint32_t width, uint32_t height);
AnimId anim_pixel_array_grid(AnimScene *scene, const AnimColor *pixels, uint32_t cols, uint32_t rows, float cell_size, float gap);

/* ------------------------------------------- SVG, Text & Typst/LaTeX math */

typedef enum AnimTexBackend {
    ANIM_TEX_AUTO = 0,
    ANIM_TEX_TYPST,
    ANIM_TEX_LATEX
} AnimTexBackend;

AnimId anim_svg_from_memory(AnimScene *scene, const char *svg, size_t len, float world_height);
AnimId anim_svg_from_file(AnimScene *scene, const char *path, float world_height);
AnimId anim_image(AnimScene *scene, const char *path, float world_height);
AnimId anim_image_from_memory(AnimScene *scene, const void *bytes, size_t len, float world_height);
AnimId anim_text(AnimScene *scene, const char *font_path, const char *utf8, float world_height);
AnimId anim_markup(AnimScene *scene, const char *font_path, const char *markup_utf8, float world_height);
void   anim_text_set_t2c(AnimScene *scene, AnimId text_id, const char *utf8_src, const char *substring, AnimColor color);
AnimId anim_tex(AnimScene *scene, AnimTexBackend backend, const char *expr, float world_height);
AnimId anim_tex_parts(AnimScene *scene, AnimTexBackend backend, const char *const *parts, uint32_t part_count, float world_height);
AnimId anim_tex_align(AnimScene *scene, AnimTexBackend backend, const char *const *lines, uint32_t line_count, float line_height, float line_spacing);
AnimId anim_tex_diff(AnimScene *scene, AnimTexBackend backend, const char *const *parts_a, uint32_t count_a,
                     const char *const *parts_b, uint32_t count_b, float world_height,
                     AnimColor removed_col, AnimColor added_col, AnimId *out_eq_a, AnimId *out_eq_b);

/* ---------------------------------------- ValueTracker & DAG updaters */

AnimTrackerId anim_tracker_create(AnimScene *scene, float initial_value);
float         anim_tracker_get(const AnimScene *scene, AnimTrackerId id);
void          anim_tracker_set(AnimScene *scene, AnimTrackerId id, float value);

void anim_add_updater(AnimScene *scene, AnimId target, AnimUpdaterFn fn, void *user,
                      const AnimId *dep_mobs, uint32_t dep_mob_count,
                      const AnimTrackerId *dep_trackers, uint32_t dep_tracker_count);
void anim_clear_updaters(AnimScene *scene, AnimId target);
void anim_eval_updaters(AnimScene *scene, float dt);
void anim_scene_step_updaters(AnimScene *scene, float total_dt, float max_substep);

/* --------------------------------------------------- Timeline & Animations */

typedef enum AnimKind {
    ANIM_KIND_NONE = 0,
    ANIM_KIND_CREATE,
    ANIM_KIND_UNCREATE,
    ANIM_KIND_WRITE,
    ANIM_KIND_DRAW_BORDER_THEN_FILL,
    ANIM_KIND_FADE_IN,
    ANIM_KIND_FADE_OUT,
    ANIM_KIND_GROW_FROM_CENTER,
    ANIM_KIND_GROW_FROM_POINT,
    ANIM_KIND_GROW_ARROW,
    ANIM_KIND_INDICATE,
    ANIM_KIND_CIRCUMSCRIBE,
    ANIM_KIND_SHOW_PASSING_FLASH,
    ANIM_KIND_WIGGLE,
    ANIM_KIND_TRANSFORM,
    ANIM_KIND_REPLACEMENT_TRANSFORM,
    ANIM_KIND_TRANSFORM_MATCHING_TEX,
    ANIM_KIND_TRANSFORM_MATCHING_SHAPES,
    ANIM_KIND_MOVE_TO,
    ANIM_KIND_SHIFT,
    ANIM_KIND_SCALE,
    ANIM_KIND_ROTATE,
    ANIM_KIND_SET_COLOR,
    ANIM_KIND_SET_OPACITY,
    ANIM_KIND_TRACKER_TO,
    ANIM_KIND_CAMERA_TO,
    ANIM_KIND_FOLLOW_PATH,
    ANIM_KIND_DASH_FLOW,
    ANIM_KIND_CAMERA_ORBIT,
    ANIM_KIND_MOVE_ARC,
    ANIM_KIND_UNWRITE,
    ANIM_KIND_GROW_FROM_EDGE,
    ANIM_KIND_SPIN_IN,
    ANIM_KIND_SHRINK_TO_CENTER,
    ANIM_KIND_TRANSFORM_FROM_COPY,
    ANIM_KIND_APPLY_WARP,
    ANIM_KIND_APPLY_MATRIX,
    ANIM_KIND_HOMOTOPY,
    ANIM_KIND_APPLY_WAVE,
    ANIM_KIND_FLASH,
    ANIM_KIND_FOCUS_ON,
    ANIM_KIND_BROADCAST,
    ANIM_KIND_SPIRAL_IN,
    ANIM_KIND_SHOW_SUBSETS,
    ANIM_KIND_SHOW_SUBMOBJECTS_ONE_BY_ONE,
    ANIM_KIND_ADD_TEXT_LETTER_BY_LETTER,
    ANIM_KIND_REMOVE_TEXT_LETTER_BY_LETTER,
    ANIM_KIND_TYPE_CURSOR,
    ANIM_KIND_ZOOM_INSET_TO,
    ANIM_KIND_CAMERA_FLY_TO,
    ANIM_KIND_CAMERA_ORBIT_AXIS,
    ANIM_KIND_CAMERA_DOLLY_ZOOM,
    ANIM_KIND_CAMERA_TRACK,
    ANIM_KIND_CAMERA_SHAKE,
    ANIM_KIND_CAMERA_PATH,
    ANIM_KIND_FLIP_3D,
    ANIM_KIND_FLIP_REPLACE_3D,
    ANIM_KIND_TUMBLE_IN,
    ANIM_KIND_TUMBLE_OUT,
    ANIM_KIND_EXPLODE_3D,
    ANIM_KIND_SWIRL_3D,
    ANIM_KIND_ROTATE_AROUND,
    ANIM_KIND_PULSE_GLOW,
    ANIM_KIND_SPRING_MOVE,
    ANIM_KIND_VIEWPORT_TO,
    ANIM_KIND_APPLY_COMPLEX_FN,
    ANIM_KIND_RESTORE,
    ANIM_KIND_ROTATING,
    ANIM_KIND_SWAP,
    ANIM_KIND_FADE_TRANSFORM,
    ANIM_KIND_COUNTERCLOCKWISE_TRANSFORM,
    ANIM_KIND_PHASE_FLOW,
    ANIM_KIND_BLINK,
    ANIM_KIND_CUSTOM
} AnimKind;

typedef struct AnimSpec {
    AnimKind      kind;
    AnimId        target;
    AnimId        other;        /* destination for Transform / path for Follow  */
    AnimTrackerId tracker;
    AnimVec3      vec;          /* shift / point / scale / axis / orbit deltas*/
    AnimColor     color;        /* color for Indicate / Circumscribe / SetCol */
    AnimCamera    camera;       /* destination camera                         */
    float         scalar;       /* angle / scale factor / opacity / tracker   */
    float         scalar2;      /* secondary scalar (radius / amplitude)      */
    float         lag_ratio;    /* [0..1], < 0 uses kind's sensible default   */
    bool          has_rate;
    AnimRateKind  rate;
    bool          flag;         /* rotate_along for FollowPath, etc.          */
    AnimRateFn    custom_rate;
    void         *custom_rate_user;
    void         *fn_ptr;       /* AnimWarpFn / AnimHomotopyFn / AnimCustomAnimFn */
    void         *fn_user;
    float         mat[16];      /* column-major 4x4 matrix for ApplyMatrix    */
} AnimSpec;

AnimSpec anim_spec_with_rate(AnimSpec sp, AnimRateKind rate);
AnimSpec anim_spec_with_lag(AnimSpec sp, float lag_ratio);
AnimSpec anim_spec_with_custom_rate(AnimSpec sp, AnimRateFn fn, void *user);

AnimSpec anim_anim_create(AnimId target);
AnimSpec anim_anim_uncreate(AnimId target);
AnimSpec anim_anim_write(AnimId target);
AnimSpec anim_anim_draw_border_then_fill(AnimId target);
AnimSpec anim_anim_fade_in(AnimId target, AnimVec3 shift, float scale);
AnimSpec anim_anim_fade_out(AnimId target, AnimVec3 shift, float scale);
AnimSpec anim_anim_grow_from_center(AnimId target);
AnimSpec anim_anim_grow_from_point(AnimId target, AnimVec3 point);
AnimSpec anim_anim_grow_arrow(AnimId target);
AnimSpec anim_anim_indicate(AnimId target, float scale_factor, AnimColor color);
AnimSpec anim_anim_circumscribe(AnimId target, AnimColor color, float buff);
AnimSpec anim_anim_show_passing_flash(AnimId target, float flash_width);
AnimSpec anim_anim_wiggle(AnimId target, float scale_factor, float angle_rad);
AnimSpec anim_anim_transform(AnimId src, AnimId dst);
AnimSpec anim_anim_replacement_transform(AnimId src, AnimId dst);
AnimSpec anim_anim_transform_matching_tex(AnimId src, AnimId dst);
AnimSpec anim_anim_transform_matching_shapes(AnimId src, AnimId dst);
AnimSpec anim_anim_move_to(AnimId target, AnimVec3 pos);
AnimSpec anim_anim_shift(AnimId target, AnimVec3 delta);
AnimSpec anim_anim_scale(AnimId target, float factor);
AnimSpec anim_anim_rotate(AnimId target, float angle_rad, AnimVec3 axis);
AnimSpec anim_anim_set_color(AnimId target, AnimColor color);
AnimSpec anim_anim_set_opacity(AnimId target, float opacity);
AnimSpec anim_anim_tracker_to(AnimTrackerId tracker, float target_value);
AnimSpec anim_anim_camera_to(AnimCamera cam);
AnimSpec anim_anim_camera_frame(const AnimScene *scene, AnimId target, float padding, float aspect);
AnimSpec anim_anim_follow_path(AnimId target, AnimId path_mob, bool rotate_along_tangent);
AnimSpec anim_anim_dash_flow(AnimId target, float delta_offset_px);
AnimSpec anim_anim_camera_orbit(float d_azimuth_rad, float d_elevation_rad, float zoom_mul);
AnimSpec anim_anim_move_arc(AnimId target, AnimVec3 dst_pos, float arc_height);
AnimSpec anim_anim_unwrite(AnimId target);
AnimSpec anim_anim_grow_from_edge(AnimId target, AnimVec3 edge_dir);
AnimSpec anim_anim_spin_in_from_nothing(AnimId target, float angle_rad);
AnimSpec anim_anim_shrink_to_center(AnimId target);
AnimSpec anim_anim_transform_from_copy(AnimId src, AnimId dst);
AnimSpec anim_anim_apply_warp(AnimId target, AnimWarpFn fn, void *user);
AnimSpec anim_anim_apply_matrix(AnimId target, const float mat4_col_major[16]);
AnimSpec anim_anim_apply_matrix3(AnimId target, const float mat3_row_major[9]);
AnimSpec anim_anim_homotopy(AnimId target, AnimHomotopyFn fn, void *user);
AnimSpec anim_anim_apply_wave(AnimId target, AnimVec3 dir, float amplitude);
AnimSpec anim_anim_flash(AnimVec3 point, AnimColor color, float line_len, uint32_t num_lines, float radius);
AnimSpec anim_anim_focus_on(AnimVec3 point, AnimColor color, float radius);
AnimSpec anim_anim_broadcast(AnimVec3 point, AnimColor color, uint32_t count, float max_radius);
AnimSpec anim_anim_spiral_in(AnimId target, float spiral_angle);
AnimSpec anim_anim_show_increasing_subsets(AnimId target);
AnimSpec anim_anim_show_submobjects_one_by_one(AnimId target);
AnimSpec anim_anim_add_text_letter_by_letter(AnimId target);
AnimSpec anim_anim_remove_text_letter_by_letter(AnimId target);
AnimSpec anim_anim_type_with_cursor(AnimId target, AnimColor cursor_color);
AnimSpec anim_anim_zoom_inset_to(bool enable, AnimCamera cam, const float rect[4]);
AnimSpec anim_anim_camera_fly_to(AnimCamera cam, float arc_height);
AnimSpec anim_anim_camera_orbit_axis(float angle_rad, AnimVec3 axis, float zoom_mul);
AnimSpec anim_anim_camera_dolly_zoom(float target_fov_deg);
AnimSpec anim_anim_camera_track(AnimId target_mob, AnimVec3 eye_offset, bool move_eye);
AnimSpec anim_anim_camera_shake(float amplitude, float frequency);
AnimSpec anim_anim_camera_path(AnimId path_mob, AnimVec3 target_pt, bool look_ahead);
AnimSpec anim_anim_flip_3d(AnimId target, AnimVec3 axis, float turns);
AnimSpec anim_anim_flip_replace_3d(AnimId src, AnimId dst, AnimVec3 axis);
AnimSpec anim_anim_tumble_in(AnimId target, AnimVec3 start_offset, float spin_rad, AnimVec3 axis);
AnimSpec anim_anim_tumble_out(AnimId target, AnimVec3 end_offset, float spin_rad, AnimVec3 axis);
AnimSpec anim_anim_explode_3d(AnimId target, float radius, float spin_rad);
AnimSpec anim_anim_swirl_3d(AnimId target, float twist_rad, AnimVec3 axis);
AnimSpec anim_anim_rotate_around(AnimId target, AnimVec3 pivot, float angle_rad, AnimVec3 axis, bool spin_self);
AnimSpec anim_anim_pulse_glow(AnimId target, float peak_glow, float peak_scale);
AnimSpec anim_anim_spring_move(AnimId target, AnimVec3 dst_pos, float damping, float freq);
AnimSpec anim_anim_viewport_to(uint32_t vp_idx, bool active, AnimCamera cam, const float rect[4]);
AnimSpec anim_anim_apply_complex_fn(AnimId target, AnimComplexFn fn, void *user);
AnimSpec anim_anim_restore(AnimId target);
AnimSpec anim_anim_rotating(AnimId target, float angle_rad, AnimVec3 axis, AnimVec3 about_point);
AnimSpec anim_anim_swap(AnimId a, AnimId b, float arc_height);
AnimSpec anim_anim_fade_transform(AnimId src, AnimId dst);
AnimSpec anim_anim_counterclockwise_transform(AnimId src, AnimId dst, float path_arc_rad);
AnimSpec anim_anim_phase_flow(AnimId target, AnimFieldFn fn, float virtual_time, void *user);
AnimSpec anim_anim_blink(AnimId target, uint32_t blinks);
AnimSpec anim_anim_custom(AnimId target, AnimCustomAnimFn fn, void *user);

/* --------------------------------- Physics & Computer Science Toolkit */

typedef void     (*AnimOdeFn)(float t, const float *y, float *dydt, uint32_t dim, void *user);
typedef AnimVec3 (*AnimOdeProjFn)(const float *y, uint32_t dim, void *user);

void   anim_phys_step_rk4(float *y, uint32_t dim, float t, float dt, AnimOdeFn fn, void *user);
AnimId anim_phys_trajectory(AnimScene *scene, const float *y0, uint32_t dim, float t0, float t1, uint32_t steps,
                            AnimOdeFn fn, AnimOdeProjFn proj, void *user);
AnimId anim_phys_spring(AnimScene *scene, AnimVec3 p0, AnimVec3 p1, uint32_t coils, float width);
void   anim_phys_spring_set(AnimScene *scene, AnimId spring_id, AnimVec3 p0, AnimVec3 p1, uint32_t coils, float width);
AnimId anim_phys_pendulum(AnimScene *scene, AnimVec3 pivot, float length, float angle_rad, float bob_radius);
void   anim_phys_pendulum_set(AnimScene *scene, AnimId pend_id, AnimVec3 pivot, float length, float angle_rad);
AnimId anim_phys_free_body(AnimScene *scene, AnimVec3 center, float box_w, float box_h,
                           const AnimVec3 *forces, const char *const *labels, const AnimColor *colors,
                           uint32_t force_count, const char *font_path);
AnimId anim_phys_wave(AnimScene *scene, AnimVec3 p0, AnimVec3 p1, float amplitude, float cycles, float phase_rad, uint32_t samples);
void   anim_phys_wave_set(AnimScene *scene, AnimId wave_id, AnimVec3 p0, AnimVec3 p1, float amplitude, float cycles, float phase_rad, uint32_t samples);
AnimId anim_phys_charge(AnimScene *scene, AnimVec3 pos, float radius, int charge_sign);
AnimId anim_phys_field_lines(AnimScene *scene, const AnimVec3 *charge_pos, const float *charges, uint32_t charge_count,
                             AnimVec3 bounds_min, AnimVec3 bounds_max, uint32_t lines_per_charge);
AnimId anim_phys_lens(AnimScene *scene, AnimVec3 center, float height, float thickness, bool diverging);
AnimId anim_phys_ray_bundle(AnimScene *scene, AnimVec3 lens_center, float focal_length, float lens_height,
                            const float *y_offsets, uint32_t ray_count, float x_start, float x_end);

typedef enum AnimLogicGateKind {
    ANIM_GATE_AND = 0,
    ANIM_GATE_OR,
    ANIM_GATE_XOR,
    ANIM_GATE_NOT,
    ANIM_GATE_NAND,
    ANIM_GATE_NOR
} AnimLogicGateKind;

void   anim_cs_swap_mobs(AnimTimeline *tl, AnimScene *scene, AnimId a, AnimId b, float arc_height, float duration);
void   anim_cs_bar_swap(AnimTimeline *tl, AnimScene *scene, AnimId *bars, uint32_t i, uint32_t j, float arc_height, float duration);
AnimId anim_cs_array(AnimScene *scene, const char *const *labels, uint32_t count, float cell_w, float cell_h,
                     bool show_indices, const char *font_path, AnimId *out_cells);
AnimId anim_cs_bar_chart(AnimScene *scene, const float *values, const char *const *labels, uint32_t count,
                         float total_w, float max_h, AnimColor c_low, AnimColor c_high, const char *font_path, AnimId *out_bars);
AnimId anim_cs_linked_list(AnimScene *scene, const char *const *labels, uint32_t count, float node_w, float node_h,
                           float spacing, const char *font_path, AnimId *out_nodes);
AnimId anim_cs_stack_queue(AnimScene *scene, const char *const *labels, uint32_t count, bool is_queue,
                           float item_w, float item_h, const char *font_path);
AnimId anim_cs_binary_tree(AnimScene *scene, const char *const *heap_labels, uint32_t heap_size, float level_dy,
                           float root_spread, float node_r, const char *font_path, AnimId *out_nodes);
AnimId anim_cs_fsm(AnimScene *scene, uint32_t state_count, const AnimVec3 *positions, const char *const *labels,
                   const bool *is_accept, uint32_t start_state, uint32_t trans_count, const uint32_t *from,
                   const uint32_t *to, const char *const *edge_labels, float node_r, const char *font_path);
AnimId anim_cs_logic_gate(AnimScene *scene, AnimLogicGateKind kind, AnimVec3 pos, float scale, const char *label, const char *font_path);
AnimId anim_cs_bit_register(AnimScene *scene, uint64_t value, uint32_t bit_count, float bit_w, bool show_indices,
                            const char *font_path, AnimId *out_bits);
AnimId anim_cs_neural_net(AnimScene *scene, const uint32_t *layer_sizes, uint32_t layer_count, float width, float height,
                          float node_r, AnimColor c_in, AnimColor c_out);

AnimTimeline *anim_timeline_create(void);
void          anim_timeline_destroy(AnimTimeline *tl);
void          anim_play(AnimTimeline *tl, AnimScene *scene, const AnimSpec *specs, uint32_t count, float duration, AnimRateKind rate);
void          anim_play_at(AnimTimeline *tl, AnimScene *scene, float start_time_sec, const AnimSpec *specs, uint32_t count, float duration, AnimRateKind rate);
void          anim_play_lagged(AnimTimeline *tl, AnimScene *scene, const AnimSpec *specs, uint32_t count, float total_duration, float lag_ratio, AnimRateKind rate);
void          anim_play_sequence(AnimTimeline *tl, AnimScene *scene, const AnimSpec *specs, uint32_t count, float step_duration, AnimRateKind rate);
void          anim_wait(AnimTimeline *tl, AnimScene *scene, float duration);
void          anim_timeline_next_section(AnimTimeline *tl, const char *name, bool skip_animations);
void          anim_timeline_set_fixed_dt(AnimTimeline *tl, float fixed_dt);
uint32_t      anim_timeline_section_count(const AnimTimeline *tl);
bool          anim_timeline_section_info(const AnimTimeline *tl, uint32_t idx, const char **out_name, float *out_start, float *out_end, bool *out_skip);
float         anim_timeline_duration(const AnimTimeline *tl);
void          anim_seek(AnimTimeline *tl, AnimScene *scene, float time_sec);

/* ----------------------------------------------------- Renderer & Export */

typedef enum AnimResolutionPreset {
    ANIM_RES_720P = 0,
    ANIM_RES_1080P,
    ANIM_RES_4K,
    ANIM_RES_VERTICAL_1080X1920,
    ANIM_RES_SQUARE_1080
} AnimResolutionPreset;

typedef enum AnimToneMapKind {
    ANIM_TONEMAP_SOFT = 0,
    ANIM_TONEMAP_ACES,
    ANIM_TONEMAP_AGX
} AnimToneMapKind;

typedef struct AnimRenderOpts {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    bool     bloom;
    float    bloom_strength;
    float    bloom_threshold;
    float    bloom_knee;
    float    exposure;
    float    saturation;
    float    vignette_radius;
    AnimToneMapKind tonemap;
    float    aberration_px;
    float    grain_amount;
    float    dof_aperture_px; /* 0 = disabled, >0 = bokeh CoC radius in px   */
    float    dof_focus_dist;  /* eye-space focal plane distance (8.0)     */
    float    dof_focus_range; /* +/- depth range in crisp focus (1.5)     */
    uint32_t motion_blur_samples; /* 1 = disabled, >1 = temporal sub-frame AA */
    float    shutter_angle;       /* [0..1], fraction of frame period (0.5)   */
    bool     transparent_bg;
    bool     show_hud;
    float    hud_progress;
    bool     hud_playing;
    bool     zoom_inset_enable;
    AnimCamera zoom_inset_cam;
    float    zoom_inset_rect[4]; /* [x0,y0,x1,y1] normalized screen coords    */
    AnimColor zoom_inset_border;
} AnimRenderOpts;

typedef struct AnimFrameStats {
    uint32_t shape_count;
    uint32_t band_count;
    uint32_t quad_seg_count;
    uint32_t surface_tri_count;
    uint32_t culled_shape_count;
    double   cpu_build_ms;
} AnimFrameStats;

AnimRenderOpts anim_render_opts_default(uint32_t width, uint32_t height);
AnimRenderOpts anim_render_opts_preset(AnimResolutionPreset preset);

typedef struct AnimContext AnimContext;

AnimContext  *anim_context_create(uint32_t width, uint32_t height, const char *title, bool headless, bool validate);
void          anim_context_destroy(AnimContext *ctx);
AnimRenderer *anim_context_renderer(AnimContext *ctx);
Gpu          *anim_context_gpu(AnimContext *ctx);
Platform     *anim_context_platform(AnimContext *ctx);
const char   *anim_context_gpu_error(const AnimContext *ctx);

AnimRenderer *anim_renderer_create(Gpu *gpu, uint32_t width, uint32_t height);
void          anim_renderer_destroy(AnimRenderer *r);
void          anim_render_to_frame(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts);
void          anim_render_to_image(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts, Image dst);
bool          anim_export_png(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts, const char *path);
bool          anim_export_video(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts, const char *mp4_path, const char *audio_wav_path);
bool          anim_export_gif(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts, const char *gif_path);
bool          anim_export_sections(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts, const char *out_dir);
void          anim_run_interactive(Platform *platform, Gpu *gpu, AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, AnimRenderOpts opts);
AnimFrameStats anim_renderer_last_stats(const AnimRenderer *r);

#ifdef __cplusplus
}
#endif

#endif /* MU_ANIM_H */
