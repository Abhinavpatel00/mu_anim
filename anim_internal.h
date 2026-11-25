#ifndef MU_ANIM_INTERNAL_H
#define MU_ANIM_INTERNAL_H

#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_macros.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_common.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_math.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_easing.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_id_pool.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_sort.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_toposort.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_files.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_save.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_vm_arena.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_time.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_log.h"
#include "third_party/mugfx/desktop/external/mu_lib/mu/mu_input.h"

#include "anim.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ANIM_MAX_MOBS       = 16384,
    ANIM_MAX_TRACKERS   = 256,
    ANIM_MAX_UPDATERS   = 512,
    ANIM_MAX_GRAD_STOPS = 8,
    ANIM_BANDS_MAX      = 64
};

typedef struct SubpathMeta {
    uint32_t cubic_offset;
    uint32_t cubic_count;
    bool     closed;
} SubpathMeta;

typedef struct SurfaceVert {
    AnimVec3  pos;
    AnimVec3  norm;
    float     u;
    float     v;
    AnimColor color;
} SurfaceVert;

/* Hot per-VMobject state: layout, transforms, style, hierarchy links. */
typedef struct VMobHot {
    uint32_t  gen;
    bool      alive;
    bool      visible;
    bool      in_scene;

    AnimVec3  pos;
    float     rot_quat[4]; /* xyzw */
    AnimVec3  scale;
    float     world_mat[16];
    bool      mat_dirty;

    AnimColor fill_color;
    AnimColor stroke_color;
    float     stroke_width;
    float     bg_stroke_width;
    float     opacity;
    float     glow;
    float     sheen;
    float     sheen_angle;
    float     shadow_px;
    float     z_index;
    float     trim_start;
    float     trim_end;
    bool      depth_tested;
    bool      fixed_in_frame;

    /* GPU dash params */
    float     dash_px;
    float     gap_px;
    float     dash_offset_px;

    uint32_t  subpath_offset;
    uint32_t  subpath_count;

    /* Optional 3D lit surface mesh stored in scene->surf_verts / surf_indices */
    uint32_t  surf_vert_offset;
    uint32_t  surf_vert_count;
    uint32_t  surf_idx_offset;
    uint32_t  surf_idx_count;

    uint32_t  parent;
    uint32_t  first_child;
    uint32_t  next_sibling;
} VMobHot;

/* Cold per-VMobject metadata: tags, cap/join, gradients, clip, axes/code/table. */
typedef struct VMobCold {
    char          tag[ANIM_TAG_LEN];
    AnimCapStyle  cap;
    AnimJoinStyle join;
    AnimFillRule  fill_rule;

    bool          has_stroke_grad;
    AnimColor     grad_start;
    AnimColor     grad_end;
    AnimColor     bg_stroke_color;
    AnimColor     sheen_color;

    uint8_t       fill_grad_mode; /* 0 = solid, 1 = linear, 2 = radial */
    uint8_t       fill_grad_stop_count;
    AnimColor     fill_grad_c2;
    AnimColor     fill_grad_stops[ANIM_MAX_GRAD_STOPS];
    AnimVec3      fill_grad_p0;
    AnimVec3      fill_grad_p1;
    float         fill_grad_radius;

    bool          has_clip;
    AnimVec3      clip_min;
    AnimVec3      clip_max;

    /* 3D surface appearance */
    float         surf_grid_density;
    float         surf_grid_alpha;
    float         surf_roughness;
    bool          surf_use_fill_color;
    uint32_t      surf_tex_id; /* 1-based index into scene->textures; 0 = untextured */
    bool          surf_unlit;  /* true = direct texture/albedo color without PBR glare */
    bool          surf_tex_nearest; /* true = nearest-neighbor pixel sampling */
    bool          surf_checker;
    AnimColor     surf_checker_c1;
    AnimColor     surf_checker_c2;
    uint32_t      surf_checker_u;
    uint32_t      surf_checker_v;

    /* Optional saved state snapshot for save_state() / Restore() */
    bool          has_saved_state;
    AnimVec3      saved_pos;
    float         saved_rot_quat[4];
    AnimVec3      saved_scale;
    AnimColor     saved_fill;
    AnimColor     saved_stroke;
    float         saved_stroke_w;
    float         saved_opacity;
    float         saved_glow;

    /* Optional Axes/DecimalNumber/Table/Code/Heatmap metadata */
    bool          is_heatmap;
    uint32_t      heatmap_cols;
    uint32_t      heatmap_rows;
    bool          is_axes;
    float         axes_x_min, axes_x_max, axes_x_len;
    float         axes_y_min, axes_y_max, axes_y_len;

    bool          is_decimal;
    uint32_t      dec_decimals;
    float         dec_height;
    char          dec_font[256];

    bool          is_table;
    uint32_t      table_rows;
    uint32_t      table_cols;

    bool          is_code;
    float         code_line_h;
    float         code_left_x;
    float         code_top_y;
    float         code_width;

    bool          is_traced_path;
    uint32_t      traced_target_idx;
    uint32_t      traced_target_gen;
    uint32_t      traced_max_pts;
    bool          is_billboard;
} VMobCold;

typedef struct UpdaterEntry {
    bool          active;
    uint32_t      target_idx;
    AnimUpdaterFn fn;
    void         *user;
    uint32_t      dep_mobs[ANIM_MAX_DEPS];
    uint32_t      dep_mob_count;
    uint32_t      dep_trackers[ANIM_MAX_DEPS];
    uint32_t      dep_tracker_count;
    uint64_t      order;
} UpdaterEntry;

typedef struct AnimTexture {
    uint8_t *rgba;
    uint32_t width;
    uint32_t height;
    uint64_t hash;
} AnimTexture;

struct AnimScene {
    mu_id_pool    mob_pool;
    VMobHot      *hot;
    VMobCold     *cold;
    uint32_t      mob_cap;

    AnimCubic    *cubics;
    uint32_t      cubic_count;
    uint32_t      cubic_cap;

    SubpathMeta  *subpaths;
    uint32_t      subpath_count;
    uint32_t      subpath_cap;

    SurfaceVert  *surf_verts;
    uint32_t      surf_vert_count;
    uint32_t      surf_vert_cap;

    uint32_t     *surf_indices;
    uint32_t      surf_idx_count;
    uint32_t      surf_idx_cap;

    AnimTexture  *textures;
    uint32_t      texture_count;
    uint32_t      texture_cap;
    bool          owns_textures;

    /* ValueTrackers */
    float         tracker_vals[ANIM_MAX_TRACKERS];
    uint32_t      tracker_gens[ANIM_MAX_TRACKERS];
    bool          tracker_alive[ANIM_MAX_TRACKERS];
    uint32_t      tracker_count;

    /* DAG updaters sorted via mu_toposort */
    UpdaterEntry  updaters[ANIM_MAX_UPDATERS];
    uint32_t      updater_count;
    uint32_t      updater_order[ANIM_MAX_UPDATERS];
    uint32_t      updater_sorted_count;
    bool          updater_dirty;
    uint64_t      updater_seq;

    AnimCamera    camera;
    bool          ambient_cam_active;
    bool          ambient_cam_illusion;
    float         ambient_d_az;
    float         ambient_d_el;
    float         ambient_wobble;
    float         ambient_phase;
    AnimColor     bg_top;
    AnimColor     bg_bot;
    char          cache_dir[ANIM_PATH_LEN];
    bool          has_traced_paths;

    bool          zoom_inset_enable;
    AnimCamera    zoom_inset_cam;
    float         zoom_inset_rect[4];
    AnimColor     zoom_inset_border;

    AnimViewport  viewports[ANIM_MAX_VIEWPORTS];
    uint32_t      viewport_count;

    AnimLight     lights[4];
    uint32_t      light_count;
    AnimColor     ambient_color;
    float         ambient_intensity;
    AnimFogDesc   fog;

    AnimCamera    saved_camera;
    bool          has_saved_camera;
    float         sim_time;
};

/* Internal helpers shared across TUs */
void           anim__set_error(const char *fmt, ...);
uint32_t       anim__register_texture_rgba(AnimScene *s, const uint8_t *rgba, uint32_t w, uint32_t h);
uint32_t       anim__load_texture_file(AnimScene *s, const char *path, uint32_t *out_w, uint32_t *out_h);
uint32_t       anim__load_texture_memory(AnimScene *s, const void *bytes, size_t len, uint32_t *out_w, uint32_t *out_h);
void           anim__ensure_cubic_cap(AnimScene *s, uint32_t add);
void           anim__ensure_subpath_cap(AnimScene *s, uint32_t add);
void           anim__ensure_surf_vert_cap(AnimScene *s, uint32_t add);
void           anim__ensure_surf_idx_cap(AnimScene *s, uint32_t add);
void           anim__sync_world_mats(AnimScene *s);
VMobHot       *anim__hot(AnimScene *s, AnimId id);
const VMobHot *anim__hot_const(const AnimScene *s, AnimId id);
VMobCold      *anim__cold(AnimScene *s, AnimId id);
void           anim__collect_subtree(const AnimScene *s, AnimId root, AnimId *out, uint32_t *io_count, uint32_t cap);
uint32_t       anim__count_subtree(const AnimScene *s, AnimId root);
void           anim__cubic_eval(AnimCubic c, float t, AnimVec3 *out_p);
void           anim__cubic_tangent(AnimCubic c, float t, AnimVec3 *out_t);
void           anim__cubic_subcurve(AnimCubic c, float t0, float t1, AnimCubic *out);
void           anim__cubic_split(AnimCubic c, float t, AnimCubic *left, AnimCubic *right);
void           anim__arrow_set(AnimScene *s, AnimId arrow_id, AnimVec3 a, AnimVec3 b, float tip_len, float tip_width);
void           anim__arc_set(AnimScene *s, AnimId arc_id, float radius, float start_angle, float angle);
void           anim__replace_from(AnimScene *s, AnimId dst, AnimId src, bool preserve_child_colors);
void           anim__vmob_trim_range(const AnimScene *s, uint32_t mob_idx, float t0, float t1,
                                     AnimCubic *out_cubics, uint32_t *out_cubic_count,
                                     SubpathMeta *out_subs, uint32_t *out_sub_count,
                                     uint32_t max_cubics, uint32_t max_subs);
void           anim__morph_vmob(AnimScene *s, AnimId dst_id, const AnimScene *snap_a, AnimId a_id,
                                const AnimScene *snap_b, AnimId b_id, float alpha);
AnimScene     *anim__snapshot_clone(const AnimScene *src);
void           anim__snapshot_restore(AnimScene *dst, const AnimScene *snap);
void           anim__snapshot_free(AnimScene *snap);

#endif /* MU_ANIM_INTERNAL_H */
