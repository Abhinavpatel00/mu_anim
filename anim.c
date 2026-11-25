/*
 * anim.c — core DOD scene, 3D Bezier geometry, layout, stdlib, timeline & morphing.
 * Follows mu_lib/AGENTS.md conventions (C11, ZII slot 0, aligned block decls).
 */
#define MU_SAVE_IMPLEMENTATION
#include "anim_internal.h"

/* ----------------------------------------------------------- error tracking */

static char g_anim_err[512];

void anim__set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_anim_err, sizeof g_anim_err, fmt, ap);
    va_end(ap);
}

const char *anim_last_error(void)
{
    return g_anim_err;
}

void anim_clear_error(void)
{
    g_anim_err[0] = '\0';
}

static AnimVec3 v3_add(AnimVec3 a, AnimVec3 b) { return ANIM_V3(a.x + b.x, a.y + b.y, a.z + b.z); }
static AnimVec3 v3_sub(AnimVec3 a, AnimVec3 b) { return ANIM_V3(a.x - b.x, a.y - b.y, a.z - b.z); }
static float    v3_len(AnimVec3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }
static AnimVec3 v3_cross(AnimVec3 a, AnimVec3 b)
{
    return ANIM_V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static AnimVec3 v3_norm(AnimVec3 v)
{
    float l = v3_len(v);
    return (l > 1e-6f) ? ANIM_V3(v.x / l, v.y / l, v.z / l) : ANIM_UP;
}

/* ----------------------------------------------------- colors & rate curves */

static float srgb_to_lin_1(float c)
{
    return (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
}

AnimColor anim_rgba(float r, float g, float b, float a)
{
    return (AnimColor){ r, g, b, a };
}

AnimColor anim_srgb_to_linear(float r, float g, float b, float a)
{
    return (AnimColor){ srgb_to_lin_1(r), srgb_to_lin_1(g), srgb_to_lin_1(b), a };
}

AnimColor anim_hex(uint32_t rgb_hex)
{
    return anim_hex_a(rgb_hex, 1.0f);
}

AnimColor anim_hex_a(uint32_t rgb_hex, float alpha)
{
    float r = (float)((rgb_hex >> 16) & 0xffu) / 255.0f;
    float g = (float)((rgb_hex >> 8) & 0xffu) / 255.0f;
    float b = (float)(rgb_hex & 0xffu) / 255.0f;

    return anim_srgb_to_linear(r, g, b, alpha);
}

static void linear_to_oklab(AnimColor c, float out[3])
{
    float l = 0.4122214708f * c.r + 0.5363325363f * c.g + 0.0514459929f * c.b;
    float m = 0.2119034982f * c.r + 0.6806995451f * c.g + 0.1073969566f * c.b;
    float s = 0.0883024619f * c.r + 0.2817188376f * c.g + 0.6299787005f * c.b;
    float l_ = cbrtf(MU_MAX(l, 0.0f));
    float m_ = cbrtf(MU_MAX(m, 0.0f));
    float s_ = cbrtf(MU_MAX(s, 0.0f));

    out[0] = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
    out[1] = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
    out[2] = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}

static AnimColor oklab_to_linear(const float lab[3], float a)
{
    float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    float l  = l_ * l_ * l_;
    float m  = m_ * m_ * m_;
    float s  = s_ * s_ * s_;

    return (AnimColor){
        MU_MAX(0.0f,  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s),
        MU_MAX(0.0f, -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s),
        MU_MAX(0.0f, -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s),
        a
    };
}

AnimColor anim_color_lerp_oklab(AnimColor a, AnimColor b, float t)
{
    float u = MU_CLAMP(t, 0.0f, 1.0f);
    float la[3];
    float lb[3];
    float lc[3];

    if (a.a <= 0.0001f && b.a > 0.0001f)
        return (AnimColor){ b.r, b.g, b.b, a.a + (b.a - a.a) * u };
    if (b.a <= 0.0001f && a.a > 0.0001f)
        return (AnimColor){ a.r, a.g, a.b, a.a + (b.a - a.a) * u };

    linear_to_oklab(a, la);
    linear_to_oklab(b, lb);
    lc[0] = la[0] + (lb[0] - la[0]) * u;
    lc[1] = la[1] + (lb[1] - la[1]) * u;
    lc[2] = la[2] + (lb[2] - la[2]) * u;
    return oklab_to_linear(lc, a.a + (b.a - a.a) * u);
}

static float rate_smooth(float t)
{
    float u = MU_CLAMP(t, 0.0f, 1.0f);
    return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
}

float anim_rate_eval(AnimRateKind kind, float t)
{
    float u = MU_CLAMP(t, 0.0f, 1.0f);

    switch (kind)
    {
    case ANIM_RATE_LINEAR:
        return u;
    case ANIM_RATE_SMOOTH:
        return rate_smooth(u);
    case ANIM_RATE_DOUBLE_SMOOTH:
        return (u < 0.5f) ? 0.5f * rate_smooth(2.0f * u) : 0.5f * (1.0f + rate_smooth(2.0f * u - 1.0f));
    case ANIM_RATE_RUSH_INTO:
        return 2.0f * rate_smooth(0.5f * u);
    case ANIM_RATE_RUSH_FROM:
        return 2.0f * rate_smooth(0.5f * (u + 1.0f)) - 1.0f;
    case ANIM_RATE_THERE_AND_BACK:
        return (u < 0.5f) ? rate_smooth(2.0f * u) : rate_smooth(2.0f * (1.0f - u));
    case ANIM_RATE_THERE_AND_BACK_PAUSE:
        if (u < 1.0f / 3.0f) return rate_smooth(3.0f * u);
        if (u < 2.0f / 3.0f) return 1.0f;
        return rate_smooth(3.0f * (1.0f - u));
    case ANIM_RATE_WIGGLE:
        return (u < 0.5f ? rate_smooth(2.0f * u) : rate_smooth(2.0f * (1.0f - u))) * sinf(4.0f * ANIM_PI * u);
    case ANIM_RATE_LINGERING:
        return (u < 0.8f) ? rate_smooth(u / 0.8f) : 1.0f;
    case ANIM_RATE_RUNNING_START:
        return mu_ease_in_out_back(u);
    case ANIM_RATE_EASE_OUT_BACK:
        return mu_ease_out_back(u);
    case ANIM_RATE_EASE_IN_OUT_CUBIC:
        return mu_ease_in_out_cubic(u);
    case ANIM_RATE_SPRING:
        return anim_rate_eval_spring(u, 0.72f, 14.0f);
    default:
        return rate_smooth(u);
    }
}

float anim_rate_eval_spring(float t, float damping, float freq)
{
    float u = MU_CLAMP(t, 0.0f, 1.0f);
    float z = MU_CLAMP(damping, 0.1f, 2.5f);
    float w = MU_MAX(freq, 1.0f);

    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    if (z < 0.999f)
    {
        float wd = w * sqrtf(1.0f - z * z);
        float e  = expf(-z * w * u);
        float raw = 1.0f - e * (cosf(wd * u) + (z * w / wd) * sinf(wd * u));
        /* Blend tail cleanly to exact 1.0 at t=1 */
        float e1 = expf(-z * w);
        float r1 = 1.0f - e1 * (cosf(wd) + (z * w / wd) * sinf(wd));
        return raw + (1.0f - r1) * (u * u * u);
    }
    else
    {
        float e   = expf(-w * u);
        float raw = 1.0f - e * (1.0f + w * u);
        float r1  = 1.0f - expf(-w) * (1.0f + w);
        return raw + (1.0f - r1) * (u * u * u);
    }
}

/* ------------------------------------------------------------------- camera */

AnimCamera anim_camera_default(void)
{
    float fov_rad = 45.0f * (ANIM_PI / 180.0f);
    float dist    = (ANIM_WORLD_H * 0.5f) / tanf(0.5f * fov_rad);

    return (AnimCamera){
        .pos          = ANIM_V3(0.0f, 0.0f, dist),
        .target       = ANIM_ORIGIN,
        .up           = ANIM_UP,
        .fov_deg      = 45.0f,
        .frame_height = ANIM_WORLD_H,
        .zoom         = 1.0f,
        .ortho_blend  = 0.0f,
    };
}

void anim_camera_view_proj(const AnimCamera *cam, float aspect, float out_vp[16], float out_view[16])
{
    float eye[3]    = { cam->pos.x, cam->pos.y, cam->pos.z };
    float tgt[3]    = { cam->target.x, cam->target.y, cam->target.z };
    float up[3]     = { cam->up.x, cam->up.y, cam->up.z };
    float view[16];
    float proj_p[16];
    float proj_o[16];
    float proj[16];
    float fov_rad   = cam->fov_deg * (ANIM_PI / 180.0f) / MU_MAX(cam->zoom, 0.05f);
    float hh        = 0.5f * cam->frame_height / MU_MAX(cam->zoom, 0.05f);
    float hw        = hh * aspect;
    float ob        = MU_CLAMP(cam->ortho_blend, 0.0f, 1.0f);
    uint32_t i;

    mu_mat4_look_at(view, eye, tgt, up);
    mu_mat4_perspective_zo(proj_p, fov_rad, aspect, 0.1f, 200.0f);
    mu_mat4_ortho(proj_o, -hw, hw, -hh, hh, 0.1f, 200.0f);
    /* ortho uses [0, 1] Z to match perspective_zo */
    proj_o[10] = -1.0f / (200.0f - 0.1f);
    proj_o[14] = -0.1f / (200.0f - 0.1f);
    for (i = 0u; i < 16u; ++i)
        proj[i] = proj_p[i] + (proj_o[i] - proj_p[i]) * ob;

    mu_mat4_mul(proj, view, out_vp);
    if (out_view)
        memcpy(out_view, view, sizeof view);
}

/* -------------------------------------------------------------- scene core */

static void vmob_reset_slot(VMobHot *h, VMobCold *c, uint32_t gen)
{
    memset(h, 0, sizeof *h);
    memset(c, 0, sizeof *c);
    h->gen          = gen;
    h->alive        = true;
    h->visible      = true;
    h->scale        = ANIM_V3(1.0f, 1.0f, 1.0f);
    mu_quat_identity(h->rot_quat);
    mu_mat4_identity(h->world_mat);
    h->mat_dirty    = true;
    h->stroke_color = ANIM_WHITE;
    h->stroke_width = ANIM_DEFAULT_STROKE_W;
    h->opacity      = 1.0f;
    h->trim_start   = 0.0f;
    h->trim_end     = 1.0f;
}

AnimScene *anim_scene_create(void)
{
    AnimScene *s = (AnimScene *)calloc(1, sizeof *s);

    MU_ASSERT(s != NULL);
    s->mob_cap = ANIM_MAX_MOBS;
    s->hot     = (VMobHot *)calloc(s->mob_cap, sizeof *s->hot);
    s->cold    = (VMobCold *)calloc(s->mob_cap, sizeof *s->cold);
    MU_ASSERT(s->hot != NULL && s->cold != NULL);
    mu_id_pool_init(&s->mob_pool, s->mob_cap);
    {
        uint32_t slot0 = 0u;
        mu_id_pool_create_id(&s->mob_pool, &slot0); /* reserve slot 0 sentinel */
        MU_ASSERT(slot0 == 0u);
    }

    s->cubic_cap   = 4096u;
    s->cubics      = (AnimCubic *)calloc(s->cubic_cap, sizeof *s->cubics);
    s->subpath_cap = 1024u;
    s->subpaths    = (SubpathMeta *)calloc(s->subpath_cap, sizeof *s->subpaths);
    s->surf_vert_cap = 256u;
    s->surf_verts    = (SurfaceVert *)calloc(s->surf_vert_cap, sizeof *s->surf_verts);
    s->surf_idx_cap  = 512u;
    s->surf_indices  = (uint32_t *)calloc(s->surf_idx_cap, sizeof *s->surf_indices);
    MU_ASSERT(s->cubics != NULL && s->subpaths != NULL && s->surf_verts != NULL && s->surf_indices != NULL);

    s->camera = anim_camera_default();
    s->bg_top = ANIM_BG_3B1B;
    s->bg_bot = anim_hex(0x090B12);
    snprintf(s->cache_dir, sizeof s->cache_dir, ".cache/mu_anim");
    s->zoom_inset_enable  = false;
    s->zoom_inset_cam     = anim_camera_default();
    s->zoom_inset_rect[0] = 0.64f; s->zoom_inset_rect[1] = 0.04f;
    s->zoom_inset_rect[2] = 0.96f; s->zoom_inset_rect[3] = 0.36f;
    s->zoom_inset_border  = ANIM_YELLOW;
    s->owns_textures      = true;
    s->viewport_count     = 0u;
    s->ambient_color      = anim_rgba(0.18f, 0.22f, 0.30f, 1.0f);
    s->ambient_intensity  = 1.0f;
    s->sim_time           = 0.0f;
    return s;
}

void anim_scene_destroy(AnimScene *s)
{
    uint32_t i;
    if (!s)
        return;
    if (s->owns_textures && s->textures)
    {
        for (i = 0u; i < s->texture_count; ++i)
            free(s->textures[i].rgba);
        free(s->textures);
    }
    mu_id_pool_destroy_all(&s->mob_pool);
    mu_id_pool_deinit(&s->mob_pool);
    free(s->hot);
    free(s->cold);
    free(s->cubics);
    free(s->subpaths);
    free(s->surf_verts);
    free(s->surf_indices);
    free(s);
}

void anim_scene_clear(AnimScene *s)
{
    uint32_t i;

    for (i = 1u; i < s->mob_cap; ++i)
    {
        if (s->hot[i].alive)
        {
            uint32_t gen = s->hot[i].gen + 1u;
            memset(&s->hot[i], 0, sizeof s->hot[i]);
            memset(&s->cold[i], 0, sizeof s->cold[i]);
            s->hot[i].gen = gen;
        }
    }
    if (s->owns_textures && s->textures)
    {
        for (i = 0u; i < s->texture_count; ++i)
            free(s->textures[i].rgba);
        s->texture_count = 0u;
    }
    mu_id_pool_destroy_all(&s->mob_pool);
    {
        uint32_t slot0 = 0u;
        mu_id_pool_create_id(&s->mob_pool, &slot0);
    }
    s->cubic_count          = 0u;
    s->subpath_count        = 0u;
    s->surf_vert_count      = 0u;
    s->surf_idx_count       = 0u;
    s->updater_count        = 0u;
    s->updater_sorted_count = 0u;
    s->updater_dirty        = false;
    s->has_traced_paths     = false;
    s->zoom_inset_enable    = false;
    s->ambient_cam_active   = false;
    s->ambient_cam_illusion = false;
    s->ambient_phase        = 0.0f;
    s->viewport_count       = 0u;
    memset(s->viewports, 0, sizeof s->viewports);
    memset(s->tracker_alive, 0, sizeof s->tracker_alive);
    s->light_count          = 0u;
    memset(s->lights, 0, sizeof s->lights);
    s->ambient_color        = anim_rgba(0.18f, 0.22f, 0.30f, 1.0f);
    s->ambient_intensity    = 1.0f;
    memset(&s->fog, 0, sizeof s->fog);
    s->has_saved_camera     = false;
    s->sim_time             = 0.0f;
}

void anim_scene_set_camera(AnimScene *s, AnimCamera cam) { s->camera = cam; }
AnimCamera anim_scene_camera(const AnimScene *s)         { return s->camera; }

void anim_scene_begin_ambient_camera_rotation(AnimScene *s, float d_azimuth_rad_per_sec, float d_elevation_rad_per_sec)
{
    if (!s)
        return;
    s->ambient_cam_active   = true;
    s->ambient_cam_illusion = false;
    s->ambient_d_az         = d_azimuth_rad_per_sec;
    s->ambient_d_el         = d_elevation_rad_per_sec;
}

void anim_scene_stop_ambient_camera_rotation(AnimScene *s)
{
    if (!s)
        return;
    s->ambient_cam_active = false;
}

void anim_scene_begin_illusion_camera_rotation(AnimScene *s, float rate_rad_per_sec, float wobble_rad)
{
    if (!s)
        return;
    s->ambient_cam_active   = true;
    s->ambient_cam_illusion = true;
    s->ambient_d_az         = rate_rad_per_sec;
    s->ambient_wobble       = (wobble_rad > 0.0f) ? wobble_rad : 0.22f;
}

void anim_scene_set_zoom_inset(AnimScene *s, bool enable, AnimCamera cam, const float rect[4], AnimColor border)
{
    if (!s)
        return;
    s->zoom_inset_enable = enable;
    s->zoom_inset_cam    = cam;
    if (rect)
        memcpy(s->zoom_inset_rect, rect, sizeof s->zoom_inset_rect);
    if (border.a > 0.0f)
        s->zoom_inset_border = border;
}

uint32_t anim_scene_add_viewport(AnimScene *s, AnimCamera cam, const float rect[4], AnimColor border, float border_width)
{
    uint32_t idx;
    if (!s || s->viewport_count >= (uint32_t)ANIM_MAX_VIEWPORTS)
        return UINT32_MAX;
    idx = s->viewport_count++;
    anim_scene_set_viewport(s, idx, true, cam, rect, border, border_width);
    return idx;
}

void anim_scene_set_viewport(AnimScene *s, uint32_t idx, bool active, AnimCamera cam, const float rect[4], AnimColor border, float border_width)
{
    AnimViewport *vp;
    if (!s || idx >= (uint32_t)ANIM_MAX_VIEWPORTS)
        return;
    if (idx >= s->viewport_count)
        s->viewport_count = idx + 1u;
    vp = &s->viewports[idx];
    vp->active       = active;
    vp->cam          = cam;
    if (rect)
        memcpy(vp->rect, rect, sizeof vp->rect);
    else if (vp->rect[2] <= vp->rect[0] || vp->rect[3] <= vp->rect[1])
    {
        vp->rect[0] = 0.0f; vp->rect[1] = 0.0f; vp->rect[2] = 0.5f; vp->rect[3] = 1.0f;
    }
    vp->border       = border;
    vp->border_width = (border_width > 0.0f) ? border_width : 2.0f;
    vp->draw_bg      = true;
}

void anim_scene_clear_viewports(AnimScene *s)
{
    if (!s)
        return;
    memset(s->viewports, 0, sizeof s->viewports);
    s->viewport_count = 0u;
}

uint32_t anim_scene_viewport_count(const AnimScene *s)
{
    return s ? s->viewport_count : 0u;
}

void anim_scene_set_bg(AnimScene *s, AnimColor top, AnimColor bottom)
{
    s->bg_top = top;
    s->bg_bot = bottom;
}

void anim_scene_set_cache_dir(AnimScene *s, const char *dir_path)
{
    if (dir_path && dir_path[0])
        snprintf(s->cache_dir, sizeof s->cache_dir, "%s", dir_path);
}

void anim_scene_clear_lights(AnimScene *s)
{
    if (!s)
        return;
    s->light_count = 0u;
    memset(s->lights, 0, sizeof s->lights);
}

void anim_scene_add_light(AnimScene *s, AnimLightKind kind, AnimVec3 pos_or_dir, AnimColor color, float intensity)
{
    AnimLight *l;
    if (!s || s->light_count >= 4u)
        return;
    l = &s->lights[s->light_count++];
    l->kind       = kind;
    l->pos_or_dir = (kind == ANIM_LIGHT_DIRECTIONAL) ? v3_norm(pos_or_dir) : pos_or_dir;
    l->color      = color;
    l->intensity  = intensity >= 0.0f ? intensity : 1.0f;
}

void anim_scene_set_ambient_light(AnimScene *s, AnimColor color, float intensity)
{
    if (!s)
        return;
    s->ambient_color     = color;
    s->ambient_intensity = intensity >= 0.0f ? intensity : 0.0f;
}

void anim_scene_set_fog(AnimScene *s, const AnimFogDesc *fog)
{
    if (!s)
        return;
    if (!fog)
        memset(&s->fog, 0, sizeof s->fog);
    else
        s->fog = *fog;
}

AnimCamera anim_camera_frame_mob(const AnimScene *s, AnimId target, float padding, float aspect)
{
    AnimCamera cam = s ? s->camera : anim_camera_default();
    AnimVec3   mn, mx, ctr, span, dir;
    float      pad = padding >= 0.0f ? padding : 0.6f;
    float      asp = aspect > 0.1f ? aspect : (16.0f / 9.0f);
    float      need_h, need_w, half_fov, dist_h, dist_w, dist;

    if (!s || !ANIM_ID_VALID(target))
        return cam;
    anim_bbox_world(s, target, &mn, &mx);
    ctr  = ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
    span = ANIM_V3(MU_MAX(mx.x - mn.x, 0.1f) + 2.0f * pad,
                   MU_MAX(mx.y - mn.y, 0.1f) + 2.0f * pad,
                   MU_MAX(mx.z - mn.z, 0.0f));
    dir  = v3_norm(v3_sub(cam.pos, cam.target));
    if (v3_len(dir) < 1e-4f)
        dir = ANIM_V3(0.0f, 0.0f, 1.0f);
    half_fov = 0.5f * cam.fov_deg * (ANIM_PI / 180.0f) / MU_MAX(cam.zoom, 0.05f);
    need_h   = 0.5f * span.y;
    need_w   = 0.5f * span.x / asp;
    dist_h   = need_h / MU_MAX(tanf(half_fov), 1e-3f);
    dist_w   = need_w / MU_MAX(tanf(half_fov), 1e-3f);
    dist     = MU_MAX(MU_MAX(dist_h, dist_w), 1.0f) + 0.5f * span.z;
    cam.target = ctr;
    cam.pos    = ANIM_V3(ctr.x + dir.x * dist, ctr.y + dir.y * dist, ctr.z + dir.z * dist);
    return cam;
}

void anim_camera_move_to(AnimCamera *cam, AnimVec3 target)
{
    AnimVec3 off;
    if (!cam)
        return;
    off         = v3_sub(cam->pos, cam->target);
    cam->target = target;
    cam->pos    = v3_add(target, off);
}

void anim_scene_camera_save(AnimScene *s)
{
    if (!s)
        return;
    s->saved_camera     = s->camera;
    s->has_saved_camera = true;
}

void anim_scene_camera_restore(AnimScene *s)
{
    if (!s || !s->has_saved_camera)
        return;
    s->camera = s->saved_camera;
}

void anim_scene_camera_set_spherical(AnimScene *s, AnimVec3 target, float distance, float phi_rad, float theta_rad)
{
    float d  = distance > 0.1f ? distance : 8.0f;
    float sp = sinf(phi_rad), cp = cosf(phi_rad);
    float ct = cosf(theta_rad), st = sinf(theta_rad);
    if (!s)
        return;
    s->camera.target = target;
    s->camera.pos    = ANIM_V3(target.x + d * sp * ct, target.y + d * sp * st, target.z + d * cp);
    if (fabsf(sp) < 0.05f)
        s->camera.up = ANIM_V3(-st, ct, 0.0f);
    else
        s->camera.up = ANIM_V3(0.0f, 0.0f, 1.0f);
}

float anim_scene_time(const AnimScene *s)
{
    return s ? s->sim_time : 0.0f;
}

void anim__ensure_cubic_cap(AnimScene *s, uint32_t add)
{
    while (s->cubic_count + add > s->cubic_cap)
    {
        uint32_t   new_cap = s->cubic_cap * 2u;
        AnimCubic *buf     = (AnimCubic *)realloc(s->cubics, (size_t)new_cap * sizeof *buf);
        MU_ASSERT(buf != NULL);
        s->cubics    = buf;
        s->cubic_cap = new_cap;
    }
}

void anim__ensure_subpath_cap(AnimScene *s, uint32_t add)
{
    while (s->subpath_count + add > s->subpath_cap)
    {
        uint32_t     new_cap = s->subpath_cap * 2u;
        SubpathMeta *buf     = (SubpathMeta *)realloc(s->subpaths, (size_t)new_cap * sizeof *buf);
        MU_ASSERT(buf != NULL);
        s->subpaths    = buf;
        s->subpath_cap = new_cap;
    }
}

void anim__ensure_surf_vert_cap(AnimScene *s, uint32_t add)
{
    while (s->surf_vert_count + add > s->surf_vert_cap)
    {
        uint32_t     new_cap = s->surf_vert_cap * 2u;
        SurfaceVert *buf     = (SurfaceVert *)realloc(s->surf_verts, (size_t)new_cap * sizeof *buf);
        MU_ASSERT(buf != NULL);
        s->surf_verts    = buf;
        s->surf_vert_cap = new_cap;
    }
}

void anim__ensure_surf_idx_cap(AnimScene *s, uint32_t add)
{
    while (s->surf_idx_count + add > s->surf_idx_cap)
    {
        uint32_t  new_cap = s->surf_idx_cap * 2u;
        uint32_t *buf     = (uint32_t *)realloc(s->surf_indices, (size_t)new_cap * sizeof *buf);
        MU_ASSERT(buf != NULL);
        s->surf_indices = buf;
        s->surf_idx_cap = new_cap;
    }
}

VMobHot *anim__hot(AnimScene *s, AnimId id)
{
    if (!ANIM_ID_VALID(id) || id.index >= s->mob_cap)
        return NULL;
    if (!s->hot[id.index].alive || s->hot[id.index].gen != id.gen)
        return NULL;
    return &s->hot[id.index];
}

const VMobHot *anim__hot_const(const AnimScene *s, AnimId id)
{
    if (!ANIM_ID_VALID(id) || id.index >= s->mob_cap)
        return NULL;
    if (!s->hot[id.index].alive || s->hot[id.index].gen != id.gen)
        return NULL;
    return &s->hot[id.index];
}

VMobCold *anim__cold(AnimScene *s, AnimId id)
{
    return anim__hot(s, id) ? &s->cold[id.index] : NULL;
}

AnimId anim_vmob_create(AnimScene *s)
{
    uint32_t idx = 0u;
    uint32_t gen;

    if (!mu_id_pool_create_id(&s->mob_pool, &idx))
    {
        log_error("anim: exceeded ANIM_MAX_MOBS (%u)", s->mob_cap);
        return ANIM_ID_NIL;
    }
    gen = s->hot[idx].gen + 1u;
    if (gen == 0u)
        gen = 1u;
    vmob_reset_slot(&s->hot[idx], &s->cold[idx], gen);
    return (AnimId){ idx, gen };
}

static void detach_from_parent(AnimScene *s, uint32_t idx)
{
    uint32_t p = s->hot[idx].parent;
    uint32_t prev;
    uint32_t cur;

    if (p == 0u)
        return;
    prev = 0u;
    cur  = s->hot[p].first_child;
    while (cur != 0u)
    {
        if (cur == idx)
        {
            if (prev == 0u)
                s->hot[p].first_child = s->hot[cur].next_sibling;
            else
                s->hot[prev].next_sibling = s->hot[cur].next_sibling;
            break;
        }
        prev = cur;
        cur  = s->hot[cur].next_sibling;
    }
    s->hot[idx].parent       = 0u;
    s->hot[idx].next_sibling = 0u;
}

void anim_vmob_destroy(AnimScene *s, AnimId id)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t c;

    if (!h)
        return;
    detach_from_parent(s, id.index);
    c = h->first_child;
    while (c != 0u)
    {
        uint32_t next = s->hot[c].next_sibling;
        anim_vmob_destroy(s, (AnimId){ c, s->hot[c].gen });
        c = next;
    }
    h->alive    = false;
    h->in_scene = false;
    mu_id_pool_destroy_id(&s->mob_pool, id.index);
}

bool anim_vmob_alive(const AnimScene *s, AnimId id)
{
    return anim__hot_const(s, id) != NULL;
}

AnimId anim_vmob_clone(AnimScene *s, AnimId id)
{
    const VMobHot *src_h = anim__hot_const(s, id);
    AnimId         dst;
    VMobHot       *dst_h;
    VMobCold      *dst_c;
    uint32_t       child_idx;
    uint32_t       i;

    if (!src_h)
        return ANIM_ID_NIL;
    dst   = anim_vmob_create(s);
    if (!ANIM_ID_VALID(dst))
        return ANIM_ID_NIL;

    /* Copy style/transform (re-fetch src_h in case nothing realloced; hot is fixed size). */
    dst_h = &s->hot[dst.index];
    dst_c = &s->cold[dst.index];
    {
        uint32_t gen = dst_h->gen;
        *dst_h               = *src_h;
        *dst_c               = s->cold[id.index];
        dst_h->gen           = gen;
        dst_h->alive         = true;
        dst_h->in_scene      = false;
        dst_h->parent        = 0u;
        dst_h->first_child   = 0u;
        dst_h->next_sibling  = 0u;
        dst_h->subpath_offset = 0u;
        dst_h->subpath_count  = 0u;
    }

    if (src_h->subpath_count > 0u)
    {
        uint32_t sp_off = s->subpath_count;
        anim__ensure_subpath_cap(s, src_h->subpath_count);
        for (i = 0u; i < src_h->subpath_count; ++i)
        {
            SubpathMeta sm     = s->subpaths[src_h->subpath_offset + i];
            uint32_t    c_off  = s->cubic_count;
            anim__ensure_cubic_cap(s, sm.cubic_count);
            if (sm.cubic_count > 0u)
                memcpy(&s->cubics[c_off], &s->cubics[sm.cubic_offset], (size_t)sm.cubic_count * sizeof(AnimCubic));
            s->cubic_count              += sm.cubic_count;
            sm.cubic_offset              = c_off;
            s->subpaths[sp_off + i]      = sm;
        }
        s->subpath_count      += src_h->subpath_count;
        dst_h->subpath_offset   = sp_off;
        dst_h->subpath_count    = src_h->subpath_count;
    }

    if (src_h->surf_vert_count > 0u && src_h->surf_idx_count > 0u)
    {
        uint32_t vo = s->surf_vert_count;
        uint32_t io = s->surf_idx_count;
        anim__ensure_surf_vert_cap(s, src_h->surf_vert_count);
        anim__ensure_surf_idx_cap(s, src_h->surf_idx_count);
        memcpy(&s->surf_verts[vo], &s->surf_verts[src_h->surf_vert_offset], (size_t)src_h->surf_vert_count * sizeof(SurfaceVert));
        memcpy(&s->surf_indices[io], &s->surf_indices[src_h->surf_idx_offset], (size_t)src_h->surf_idx_count * sizeof(uint32_t));
        s->surf_vert_count    += src_h->surf_vert_count;
        s->surf_idx_count     += src_h->surf_idx_count;
        dst_h->surf_vert_offset = vo;
        dst_h->surf_idx_offset  = io;
    }

    child_idx = src_h->first_child;
    while (child_idx != 0u)
    {
        AnimId cloned_child = anim_vmob_clone(s, (AnimId){ child_idx, s->hot[child_idx].gen });
        if (ANIM_ID_VALID(cloned_child))
            anim_add_child(s, dst, cloned_child);
        child_idx = s->hot[child_idx].next_sibling;
    }
    return dst;
}

/* ------------------------------------------------------------ path building */

void anim_vmob_clear_paths(AnimScene *s, AnimId id)
{
    VMobHot *h = anim__hot(s, id);
    if (!h)
        return;
    h->subpath_offset = 0u;
    h->subpath_count  = 0u;
}

void anim_vmob_begin_subpath(AnimScene *s, AnimId id, AnimVec3 start)
{
    VMobHot    *h = anim__hot(s, id);
    SubpathMeta sm;

    if (!h)
        return;
    /* Ensure contiguous subpath array at tail when appending to a mob */
    if (h->subpath_count == 0u || h->subpath_offset + h->subpath_count != s->subpath_count)
    {
        uint32_t new_off = s->subpath_count;
        anim__ensure_subpath_cap(s, h->subpath_count + 1u);
        if (h->subpath_count > 0u)
            memcpy(&s->subpaths[new_off], &s->subpaths[h->subpath_offset], (size_t)h->subpath_count * sizeof(SubpathMeta));
        h->subpath_offset = new_off;
        s->subpath_count += h->subpath_count;
    }
    else
    {
        anim__ensure_subpath_cap(s, 1u);
    }
    sm.cubic_offset = s->cubic_count;
    sm.cubic_count  = 0u;
    sm.closed       = false;
    s->subpaths[s->subpath_count++] = sm;
    h->subpath_count++;
    /* Store anchor temporarily by encoding a zero-length placeholder? No: track start in a 0-cubic subpath?
     * Even cleaner: we store start point inside a tiny 1-cubic sentinel only if no cubics? No, track last anchor:
     * Let's store a collapsed 0-cubic start? Wait: if begin_subpath is followed by line_to, how does line_to know `start`?
     * Store `start` in `cubics[cubic_offset].p0` pre-reserved without advancing cubic_count! */
    anim__ensure_cubic_cap(s, 1u);
    s->cubics[sm.cubic_offset].p0 = start;
    s->cubics[sm.cubic_offset].p3 = start;
}

static void append_cubic_to_active_subpath(AnimScene *s, VMobHot *h, AnimCubic c)
{
    SubpathMeta *sm;
    uint32_t     sp_idx;

    if (h->subpath_count == 0u)
        anim_vmob_begin_subpath(s, (AnimId){ (uint32_t)(h - s->hot), h->gen }, c.p0);

    sp_idx = h->subpath_offset + h->subpath_count - 1u;
    sm     = &s->subpaths[sp_idx];

    /* If other mobjects wrote cubics after this subpath, relocate this subpath's cubics to tail. */
    if (sm->cubic_offset + sm->cubic_count != s->cubic_count)
    {
        uint32_t new_off = s->cubic_count;
        anim__ensure_cubic_cap(s, sm->cubic_count + 1u);
        if (sm->cubic_count > 0u)
            memcpy(&s->cubics[new_off], &s->cubics[sm->cubic_offset], (size_t)sm->cubic_count * sizeof(AnimCubic));
        sm->cubic_offset = new_off;
        s->cubic_count  += sm->cubic_count;
    }
    else
    {
        anim__ensure_cubic_cap(s, 1u);
    }

    s->cubics[s->cubic_count++] = c;
    sm->cubic_count++;
}

static AnimVec3 current_anchor(const AnimScene *s, const VMobHot *h)
{
    const SubpathMeta *sm;

    if (h->subpath_count == 0u)
        return ANIM_ORIGIN;
    sm = &s->subpaths[h->subpath_offset + h->subpath_count - 1u];
    if (sm->cubic_count == 0u)
        return s->cubics[sm->cubic_offset].p0;
    return s->cubics[sm->cubic_offset + sm->cubic_count - 1u].p3;
}

void anim_vmob_line_to(AnimScene *s, AnimId id, AnimVec3 end)
{
    VMobHot  *h = anim__hot(s, id);
    AnimVec3  p0;
    AnimCubic c;

    if (!h)
        return;
    p0   = current_anchor(s, h);
    c.p0 = p0;
    c.p1 = ANIM_V3(p0.x + (end.x - p0.x) / 3.0f, p0.y + (end.y - p0.y) / 3.0f, p0.z + (end.z - p0.z) / 3.0f);
    c.p2 = ANIM_V3(p0.x + (end.x - p0.x) * (2.0f / 3.0f), p0.y + (end.y - p0.y) * (2.0f / 3.0f), p0.z + (end.z - p0.z) * (2.0f / 3.0f));
    c.p3 = end;
    append_cubic_to_active_subpath(s, h, c);
}

void anim_vmob_quad_to(AnimScene *s, AnimId id, AnimVec3 ctrl, AnimVec3 end)
{
    VMobHot  *h = anim__hot(s, id);
    AnimVec3  p0;
    AnimCubic c;

    if (!h)
        return;
    p0   = current_anchor(s, h);
    c.p0 = p0;
    c.p1 = ANIM_V3(p0.x + (2.0f / 3.0f) * (ctrl.x - p0.x), p0.y + (2.0f / 3.0f) * (ctrl.y - p0.y), p0.z + (2.0f / 3.0f) * (ctrl.z - p0.z));
    c.p2 = ANIM_V3(end.x + (2.0f / 3.0f) * (ctrl.x - end.x), end.y + (2.0f / 3.0f) * (ctrl.y - end.y), end.z + (2.0f / 3.0f) * (ctrl.z - end.z));
    c.p3 = end;
    append_cubic_to_active_subpath(s, h, c);
}

void anim_vmob_cubic_to(AnimScene *s, AnimId id, AnimVec3 c1, AnimVec3 c2, AnimVec3 end)
{
    VMobHot  *h = anim__hot(s, id);
    AnimCubic c;

    if (!h)
        return;
    c.p0 = current_anchor(s, h);
    c.p1 = c1;
    c.p2 = c2;
    c.p3 = end;
    append_cubic_to_active_subpath(s, h, c);
}

void anim_vmob_close_subpath(AnimScene *s, AnimId id)
{
    VMobHot     *h = anim__hot(s, id);
    SubpathMeta *sm;
    AnimVec3     p0;
    AnimVec3     p3;
    float        dx, dy, dz;

    if (!h || h->subpath_count == 0u)
        return;
    sm = &s->subpaths[h->subpath_offset + h->subpath_count - 1u];
    if (sm->cubic_count == 0u)
        return;
    p0 = s->cubics[sm->cubic_offset].p0;
    p3 = s->cubics[sm->cubic_offset + sm->cubic_count - 1u].p3;
    dx = p0.x - p3.x;
    dy = p0.y - p3.y;
    dz = p0.z - p3.z;
    if (dx * dx + dy * dy + dz * dz > 1e-8f)
        anim_vmob_line_to(s, id, p0);
    s->subpaths[h->subpath_offset + h->subpath_count - 1u].closed = true;
}

void anim_vmob_add_cubic(AnimScene *s, AnimId id, AnimCubic c, bool new_subpath)
{
    VMobHot *h = anim__hot(s, id);

    if (!h)
        return;
    if (new_subpath || h->subpath_count == 0u)
        anim_vmob_begin_subpath(s, id, c.p0);
    append_cubic_to_active_subpath(s, h, c);
}

uint32_t anim_vmob_subpath_count(const AnimScene *s, AnimId id)
{
    const VMobHot *h = anim__hot_const(s, id);
    return h ? h->subpath_count : 0u;
}

uint32_t anim_vmob_cubic_count(const AnimScene *s, AnimId id)
{
    const VMobHot *h = anim__hot_const(s, id);
    uint32_t       total = 0u;
    uint32_t       i;

    if (!h)
        return 0u;
    for (i = 0u; i < h->subpath_count; ++i)
        total += s->subpaths[h->subpath_offset + i].cubic_count;
    return total;
}

void anim__cubic_eval(AnimCubic c, float t, AnimVec3 *out_p)
{
    float u  = 1.0f - t;
    float b0 = u * u * u;
    float b1 = 3.0f * u * u * t;
    float b2 = 3.0f * u * t * t;
    float b3 = t * t * t;

    out_p->x = b0 * c.p0.x + b1 * c.p1.x + b2 * c.p2.x + b3 * c.p3.x;
    out_p->y = b0 * c.p0.y + b1 * c.p1.y + b2 * c.p2.y + b3 * c.p3.y;
    out_p->z = b0 * c.p0.z + b1 * c.p1.z + b2 * c.p2.z + b3 * c.p3.z;
}

void anim__cubic_tangent(AnimCubic c, float t, AnimVec3 *out_t)
{
    float u  = 1.0f - t;
    float d0 = 3.0f * u * u;
    float d1 = 6.0f * u * t;
    float d2 = 3.0f * t * t;

    out_t->x = d0 * (c.p1.x - c.p0.x) + d1 * (c.p2.x - c.p1.x) + d2 * (c.p3.x - c.p2.x);
    out_t->y = d0 * (c.p1.y - c.p0.y) + d1 * (c.p2.y - c.p1.y) + d2 * (c.p3.y - c.p2.y);
    out_t->z = d0 * (c.p1.z - c.p0.z) + d1 * (c.p2.z - c.p1.z) + d2 * (c.p3.z - c.p2.z);
}

static AnimVec3 v3_lerp(AnimVec3 a, AnimVec3 b, float t)
{
    return ANIM_V3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

void anim__cubic_split(AnimCubic c, float t, AnimCubic *left, AnimCubic *right)
{
    AnimVec3 a  = v3_lerp(c.p0, c.p1, t);
    AnimVec3 b  = v3_lerp(c.p1, c.p2, t);
    AnimVec3 d  = v3_lerp(c.p2, c.p3, t);
    AnimVec3 e  = v3_lerp(a, b, t);
    AnimVec3 f  = v3_lerp(b, d, t);
    AnimVec3 m  = v3_lerp(e, f, t);

    if (left)
        *left = (AnimCubic){ c.p0, a, e, m };
    if (right)
        *right = (AnimCubic){ m, f, d, c.p3 };
}

void anim__cubic_subcurve(AnimCubic c, float t0, float t1, AnimCubic *out)
{
    AnimCubic tmp;
    float     u1;

    t0 = MU_CLAMP(t0, 0.0f, 1.0f);
    t1 = MU_CLAMP(t1, 0.0f, 1.0f);
    if (t1 <= t0)
    {
        AnimVec3 p;
        anim__cubic_eval(c, t0, &p);
        *out = (AnimCubic){ p, p, p, p };
        return;
    }
    anim__cubic_split(c, t0, NULL, &tmp);
    u1 = (1.0f - t0) > 1e-6f ? (t1 - t0) / (1.0f - t0) : 1.0f;
    anim__cubic_split(tmp, MU_CLAMP(u1, 0.0f, 1.0f), out, NULL);
}

static float cubic_arc_len_table(AnimCubic c, float table[17])
{
    float acc = 0.0f;
    int   k;
    if (table)
        table[0] = 0.0f;
    for (k = 0; k < 16; ++k)
    {
        float    t0 = (float)k / 16.0f;
        float    t1 = (float)(k + 1) / 16.0f;
        float    tm = 0.5f * (t0 + t1);
        AnimVec3 d0, dm, d1;
        float    s0, sm, s1, seg;
        anim__cubic_tangent(c, t0, &d0);
        anim__cubic_tangent(c, tm, &dm);
        anim__cubic_tangent(c, t1, &d1);
        s0  = sqrtf(d0.x*d0.x + d0.y*d0.y + d0.z*d0.z);
        sm  = sqrtf(dm.x*dm.x + dm.y*dm.y + dm.z*dm.z);
        s1  = sqrtf(d1.x*d1.x + d1.y*d1.y + d1.z*d1.z);
        seg = ((s0 + 4.0f * sm + s1) / 6.0f) * (1.0f / 16.0f);
        acc += seg;
        if (table)
            table[k + 1] = acc;
    }
    return acc;
}

static float cubic_inv_arc_len(const float table[17], float target_s)
{
    float total = table[16];
    int   k;
    if (total <= 1e-7f || target_s <= 0.0f)
        return 0.0f;
    if (target_s >= total)
        return 1.0f;
    for (k = 0; k < 16; ++k)
    {
        if (table[k + 1] >= target_s)
        {
            float ds   = table[k + 1] - table[k];
            float frac = ds > 1e-8f ? (target_s - table[k]) / ds : 0.0f;
            return ((float)k + MU_CLAMP(frac, 0.0f, 1.0f)) / 16.0f;
        }
    }
    return 1.0f;
}

static float cubic_approx_len(AnimCubic c)
{
    return cubic_arc_len_table(c, NULL);
}

static float vmob_arc_length_rec(const AnimScene *s, uint32_t idx)
{
    const VMobHot *h = &s->hot[idx];
    float          len = 0.0f;
    uint32_t       si, ci, ch;

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta sm = s->subpaths[h->subpath_offset + si];
        for (ci = 0u; ci < sm.cubic_count; ++ci)
            len += cubic_arc_len_table(s->cubics[sm.cubic_offset + ci], NULL);
    }
    ch = h->first_child;
    while (ch != 0u)
    {
        len += vmob_arc_length_rec(s, ch);
        ch = s->hot[ch].next_sibling;
    }
    return len;
}

float anim_vmob_arc_length(const AnimScene *s, AnimId id)
{
    const VMobHot *h = anim__hot_const(s, id);
    if (!h)
        return 0.0f;
    return vmob_arc_length_rec(s, id.index);
}

static bool vmob_eval_at_arc_s(const AnimScene *s, uint32_t idx, float s_target, float *io_acc, AnimVec3 *out_pos, AnimVec3 *out_tan)
{
    const VMobHot *h = &s->hot[idx];
    uint32_t       si, ci, ch;

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta sm = s->subpaths[h->subpath_offset + si];
        for (ci = 0u; ci < sm.cubic_count; ++ci)
        {
            AnimCubic c = s->cubics[sm.cubic_offset + ci];
            float     tab[17];
            float     clen = MU_MAX(cubic_arc_len_table(c, tab), 1e-6f);
            if (*io_acc + clen >= s_target)
            {
                float u = cubic_inv_arc_len(tab, s_target - *io_acc);
                if (out_pos) anim__cubic_eval(c, u, out_pos);
                if (out_tan) anim__cubic_tangent(c, u, out_tan);
                return true;
            }
            *io_acc += clen;
        }
    }
    ch = h->first_child;
    while (ch != 0u)
    {
        if (vmob_eval_at_arc_s(s, ch, s_target, io_acc, out_pos, out_tan))
            return true;
        ch = s->hot[ch].next_sibling;
    }
    return false;
}

AnimVec3 anim_vmob_point_at(const AnimScene *s, AnimId id, float t)
{
    const VMobHot *h = anim__hot_const(s, id);
    float          total_len;
    float          acc = 0.0f;
    AnimVec3       p = ANIM_ORIGIN;

    if (!h)
        return ANIM_ORIGIN;
    total_len = vmob_arc_length_rec(s, id.index);
    if (total_len <= 1e-6f)
        return ANIM_ORIGIN;
    t = MU_CLAMP(t, 0.0f, 1.0f);
    if (!vmob_eval_at_arc_s(s, id.index, t * total_len, &acc, &p, NULL))
        (void)vmob_eval_at_arc_s(s, id.index, total_len - 1e-6f, &(float){ 0.0f }, &p, NULL);
    return p;
}

AnimVec3 anim_vmob_tangent_at(const AnimScene *s, AnimId id, float t)
{
    const VMobHot *h = anim__hot_const(s, id);
    float          total_len;
    float          acc = 0.0f;
    AnimVec3       tan = ANIM_RIGHT;

    if (!h)
        return ANIM_RIGHT;
    total_len = vmob_arc_length_rec(s, id.index);
    if (total_len <= 1e-6f)
        return ANIM_RIGHT;
    t = MU_CLAMP(t, 0.0f, 1.0f);
    if (!vmob_eval_at_arc_s(s, id.index, t * total_len, &acc, NULL, &tan))
        (void)vmob_eval_at_arc_s(s, id.index, total_len - 1e-6f, &(float){ 0.0f }, NULL, &tan);
    return v3_norm(tan);
}

void anim__vmob_trim_range(const AnimScene *s, uint32_t mob_idx, float t0, float t1,
                           AnimCubic *out_cubics, uint32_t *out_cubic_count,
                           SubpathMeta *out_subs, uint32_t *out_sub_count,
                           uint32_t max_cubics, uint32_t max_subs)
{
    const VMobHot *h         = &s->hot[mob_idx];
    float          total_len = 0.0f;
    float          len0, len1;
    float          acc       = 0.0f;
    uint32_t       si, ci;

    *out_cubic_count = 0u;
    *out_sub_count   = 0u;
    t0 = MU_CLAMP(t0, 0.0f, 1.0f);
    t1 = MU_CLAMP(t1, 0.0f, 1.0f);
    if (t1 <= t0 || h->subpath_count == 0u)
        return;

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta sm = s->subpaths[h->subpath_offset + si];
        for (ci = 0u; ci < sm.cubic_count; ++ci)
            total_len += MU_MAX(cubic_arc_len_table(s->cubics[sm.cubic_offset + ci], NULL), 1e-5f);
    }
    if (total_len <= 1e-6f)
        return;
    len0 = t0 * total_len;
    len1 = t1 * total_len;

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta sm        = s->subpaths[h->subpath_offset + si];
        uint32_t    sub_start = *out_cubic_count;

        for (ci = 0u; ci < sm.cubic_count; ++ci)
        {
            AnimCubic c    = s->cubics[sm.cubic_offset + ci];
            float     tab[17];
            float     clen = MU_MAX(cubic_arc_len_table(c, tab), 1e-5f);
            float     s_a  = acc;
            float     s_b  = acc + clen;

            acc = s_b;
            if (s_b <= len0 || s_a >= len1)
                continue;
            if (*out_cubic_count >= max_cubics)
                break;
            {
                float     u0 = (len0 > s_a) ? cubic_inv_arc_len(tab, len0 - s_a) : 0.0f;
                float     u1 = (len1 < s_b) ? cubic_inv_arc_len(tab, len1 - s_a) : 1.0f;
                AnimCubic sub_c;
                anim__cubic_subcurve(c, u0, u1, &sub_c);
                out_cubics[(*out_cubic_count)++] = sub_c;
            }
        }
        if (*out_cubic_count > sub_start && *out_sub_count < max_subs)
        {
            out_subs[(*out_sub_count)++] = (SubpathMeta){
                .cubic_offset = sub_start,
                .cubic_count  = *out_cubic_count - sub_start,
                .closed       = sm.closed && (t0 <= 1e-4f && t1 >= 1.0f - 1e-4f),
            };
        }
    }
}

void anim_vmob_subdivide(AnimScene *s, AnimId id, uint32_t cuts_per_cubic)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t si, ci, k;

    if (!h || cuts_per_cubic <= 1u || h->subpath_count == 0u)
        return;

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta *sm      = &s->subpaths[h->subpath_offset + si];
        uint32_t     new_n   = sm->cubic_count * cuts_per_cubic;
        uint32_t     new_off = s->cubic_count;

        if (sm->cubic_count == 0u)
            continue;
        anim__ensure_cubic_cap(s, new_n);
        sm = &s->subpaths[h->subpath_offset + si];
        for (ci = 0u; ci < sm->cubic_count; ++ci)
        {
            AnimCubic orig = s->cubics[sm->cubic_offset + ci];
            for (k = 0u; k < cuts_per_cubic; ++k)
            {
                float     t0 = (float)k / (float)cuts_per_cubic;
                float     t1 = (float)(k + 1u) / (float)cuts_per_cubic;
                AnimCubic sub_c;
                anim__cubic_subcurve(orig, t0, t1, &sub_c);
                s->cubics[new_off + ci * cuts_per_cubic + k] = sub_c;
            }
        }
        sm->cubic_offset = new_off;
        sm->cubic_count  = new_n;
        s->cubic_count  += new_n;
    }
}

void anim_vmob_apply_warp(AnimScene *s, AnimId id, AnimWarpFn fn, void *user, uint32_t subdiv_cuts)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    uint32_t si, ci;

    if (!h || !fn)
        return;
    if (subdiv_cuts > 1u)
        anim_vmob_subdivide(s, id, subdiv_cuts);

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta sm = s->subpaths[h->subpath_offset + si];
        for (ci = 0u; ci < sm.cubic_count; ++ci)
        {
            AnimCubic *c = &s->cubics[sm.cubic_offset + ci];
            c->p0 = fn(c->p0, user);
            c->p1 = fn(c->p1, user);
            c->p2 = fn(c->p2, user);
            c->p3 = fn(c->p3, user);
        }
    }

    child = h->first_child;
    while (child != 0u)
    {
        anim_vmob_apply_warp(s, (AnimId){ child, s->hot[child].gen }, fn, user, subdiv_cuts);
        child = s->hot[child].next_sibling;
    }
}

void anim_vmob_apply_complex_fn(AnimScene *s, AnimId id, AnimComplexFn fn, void *user, uint32_t subdiv_cuts)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    uint32_t si, ci;

    if (!h || !fn)
        return;
    if (subdiv_cuts > 1u)
        anim_vmob_subdivide(s, id, subdiv_cuts);

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta sm = s->subpaths[h->subpath_offset + si];
        for (ci = 0u; ci < sm.cubic_count; ++ci)
        {
            AnimCubic *c = &s->cubics[sm.cubic_offset + ci];
            AnimComplex z0 = fn((AnimComplex){ c->p0.x, c->p0.y }, user);
            AnimComplex z1 = fn((AnimComplex){ c->p1.x, c->p1.y }, user);
            AnimComplex z2 = fn((AnimComplex){ c->p2.x, c->p2.y }, user);
            AnimComplex z3 = fn((AnimComplex){ c->p3.x, c->p3.y }, user);
            c->p0 = ANIM_V3(z0.re, z0.im, c->p0.z);
            c->p1 = ANIM_V3(z1.re, z1.im, c->p1.z);
            c->p2 = ANIM_V3(z2.re, z2.im, c->p2.z);
            c->p3 = ANIM_V3(z3.re, z3.im, c->p3.z);
        }
    }

    child = h->first_child;
    while (child != 0u)
    {
        anim_vmob_apply_complex_fn(s, (AnimId){ child, s->hot[child].gen }, fn, user, subdiv_cuts);
        child = s->hot[child].next_sibling;
    }
}

void anim_save_state(AnimScene *s, AnimId id)
{
    VMobHot  *h = anim__hot(s, id);
    VMobCold *c = anim__cold(s, id);
    uint32_t  child;

    if (!h || !c)
        return;
    c->has_saved_state = true;
    c->saved_pos       = h->pos;
    memcpy(c->saved_rot_quat, h->rot_quat, sizeof h->rot_quat);
    c->saved_scale     = h->scale;
    c->saved_fill      = h->fill_color;
    c->saved_stroke    = h->stroke_color;
    c->saved_stroke_w  = h->stroke_width;
    c->saved_opacity   = h->opacity;
    c->saved_glow      = h->glow;

    child = h->first_child;
    while (child != 0u)
    {
        anim_save_state(s, (AnimId){ child, s->hot[child].gen });
        child = s->hot[child].next_sibling;
    }
}

void anim_vmob_set_stroke_gradient(AnimScene *s, AnimId id, AnimColor c_start, AnimColor c_end)
{
    VMobCold *c = anim__cold(s, id);
    if (!c)
        return;
    c->has_stroke_grad = true;
    c->grad_start      = c_start;
    c->grad_end        = c_end;
}

/* ------------------------------------------------------------------ styling */

static void apply_recursive(AnimScene *s, AnimId id, bool recursive, void (*fn)(AnimScene *, uint32_t, void *), void *arg)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    fn(s, id.index, arg);
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        apply_recursive(s, (AnimId){ child, s->hot[child].gen }, true, fn, arg);
        child = s->hot[child].next_sibling;
    }
}

static void fn_set_fill(AnimScene *s, uint32_t idx, void *arg) { s->hot[idx].fill_color = *(const AnimColor *)arg; }
typedef struct StrokeArgs { AnimColor c; float w; } StrokeArgs;
static void fn_set_stroke(AnimScene *s, uint32_t idx, void *arg)
{
    const StrokeArgs *a = (const StrokeArgs *)arg;
    s->hot[idx].stroke_color = a->c;
    s->hot[idx].stroke_width = a->w;
}
static void fn_set_color(AnimScene *s, uint32_t idx, void *arg)
{
    AnimColor c = *(const AnimColor *)arg;
    if (s->hot[idx].fill_color.a > 0.0f)
        s->hot[idx].fill_color = (AnimColor){ c.r, c.g, c.b, s->hot[idx].fill_color.a };
    s->hot[idx].stroke_color = (AnimColor){ c.r, c.g, c.b, s->hot[idx].stroke_color.a > 0.0f ? c.a : 0.0f };
}
static void fn_set_opacity(AnimScene *s, uint32_t idx, void *arg) { s->hot[idx].opacity = *(const float *)arg; }
static void fn_set_glow(AnimScene *s, uint32_t idx, void *arg)    { s->hot[idx].glow = *(const float *)arg; }
static void fn_set_z(AnimScene *s, uint32_t idx, void *arg)       { s->hot[idx].z_index = *(const float *)arg; }
static void fn_set_vis(AnimScene *s, uint32_t idx, void *arg)     { s->hot[idx].visible = *(const bool *)arg; }

void anim_set_fill(AnimScene *s, AnimId id, AnimColor color, bool recursive)
{
    apply_recursive(s, id, recursive, fn_set_fill, &color);
}

void anim_set_fill_gradient_linear(AnimScene *s, AnimId id, AnimColor c0, AnimColor c1, AnimVec3 world_p0, AnimVec3 world_p1, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    h->fill_color                     = c0;
    s->cold[id.index].fill_grad_mode  = 1u;
    s->cold[id.index].fill_grad_stop_count = 0u;
    s->cold[id.index].fill_grad_c2    = c1;
    s->cold[id.index].fill_grad_p0    = world_p0;
    s->cold[id.index].fill_grad_p1    = world_p1;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_fill_gradient_linear(s, (AnimId){ child, s->hot[child].gen }, c0, c1, world_p0, world_p1, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_fill_gradient_radial(AnimScene *s, AnimId id, AnimColor c_inner, AnimColor c_outer, AnimVec3 world_center, float world_radius, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    h->fill_color                     = c_inner;
    s->cold[id.index].fill_grad_mode  = 2u;
    s->cold[id.index].fill_grad_stop_count = 0u;
    s->cold[id.index].fill_grad_c2    = c_outer;
    s->cold[id.index].fill_grad_p0    = world_center;
    s->cold[id.index].fill_grad_radius = MU_MAX(world_radius, 1e-4f);
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_fill_gradient_radial(s, (AnimId){ child, s->hot[child].gen }, c_inner, c_outer, world_center, world_radius, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_fill_gradient_stops(AnimScene *s, AnimId id, const AnimColor *stops, uint32_t stop_count,
                                  AnimVec3 world_p0, AnimVec3 world_p1, bool radial, float world_radius, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child, n, i;

    if (!h || !stops || stop_count == 0u)
        return;
    n = MU_MIN(stop_count, (uint32_t)ANIM_MAX_GRAD_STOPS);
    h->fill_color = stops[0];
    if (n == 1u)
    {
        s->cold[id.index].fill_grad_mode       = 0u;
        s->cold[id.index].fill_grad_stop_count = 0u;
    }
    else
    {
        s->cold[id.index].fill_grad_mode       = radial ? 2u : 1u;
        s->cold[id.index].fill_grad_stop_count = (uint8_t)n;
        for (i = 0u; i < n; ++i)
            s->cold[id.index].fill_grad_stops[i] = stops[i];
        s->cold[id.index].fill_grad_c2     = stops[n - 1u];
        s->cold[id.index].fill_grad_p0     = world_p0;
        s->cold[id.index].fill_grad_p1     = world_p1;
        s->cold[id.index].fill_grad_radius = MU_MAX(world_radius, 1e-4f);
    }
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_fill_gradient_stops(s, (AnimId){ child, s->hot[child].gen }, stops, stop_count, world_p0, world_p1, radial, world_radius, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_clear_fill_gradient(AnimScene *s, AnimId id, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    s->cold[id.index].fill_grad_mode       = 0u;
    s->cold[id.index].fill_grad_stop_count = 0u;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_clear_fill_gradient(s, (AnimId){ child, s->hot[child].gen }, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_background_stroke(AnimScene *s, AnimId id, AnimColor color, float width_pt, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    h->bg_stroke_width               = MU_MAX(width_pt, 0.0f);
    s->cold[id.index].bg_stroke_color = color;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_background_stroke(s, (AnimId){ child, s->hot[child].gen }, color, width_pt, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_dash(AnimScene *s, AnimId id, float dash_px, float gap_px, float offset_px, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    h->dash_px        = MU_MAX(dash_px, 0.0f);
    h->gap_px         = MU_MAX(gap_px, 0.0f);
    h->dash_offset_px = offset_px;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_dash(s, (AnimId){ child, s->hot[child].gen }, dash_px, gap_px, offset_px, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_shadow(AnimScene *s, AnimId id, float shadow_px, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    h->shadow_px = MU_MAX(shadow_px, 0.0f);
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_shadow(s, (AnimId){ child, s->hot[child].gen }, shadow_px, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_clip_rect(AnimScene *s, AnimId id, AnimVec3 world_min, AnimVec3 world_max, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    s->cold[id.index].has_clip = true;
    s->cold[id.index].clip_min = world_min;
    s->cold[id.index].clip_max = world_max;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_clip_rect(s, (AnimId){ child, s->hot[child].gen }, world_min, world_max, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_clear_clip(AnimScene *s, AnimId id, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    s->cold[id.index].has_clip = false;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_clear_clip(s, (AnimId){ child, s->hot[child].gen }, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_stroke(AnimScene *s, AnimId id, AnimColor color, float width_pt, bool recursive)
{
    StrokeArgs a = { color, width_pt };
    apply_recursive(s, id, recursive, fn_set_stroke, &a);
}

void anim_set_color(AnimScene *s, AnimId id, AnimColor color, bool recursive)
{
    apply_recursive(s, id, recursive, fn_set_color, &color);
}

void anim_set_opacity(AnimScene *s, AnimId id, float opacity, bool recursive)
{
    apply_recursive(s, id, recursive, fn_set_opacity, &opacity);
}

void anim_set_glow(AnimScene *s, AnimId id, float glow, bool recursive)
{
    apply_recursive(s, id, recursive, fn_set_glow, &glow);
}

void anim_set_sheen(AnimScene *s, AnimId id, float sheen, float angle_rad, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    h->sheen       = sheen;
    h->sheen_angle = angle_rad;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_sheen(s, (AnimId){ child, s->hot[child].gen }, sheen, angle_rad, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_sheen_color(AnimScene *s, AnimId id, float sheen, float angle_rad, AnimColor color, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    h->sheen                      = sheen;
    h->sheen_angle                = angle_rad;
    s->cold[id.index].sheen_color = color;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_sheen_color(s, (AnimId){ child, s->hot[child].gen }, sheen, angle_rad, color, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_depth_test(AnimScene *s, AnimId id, bool depth_test, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    h->depth_tested = depth_test;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_depth_test(s, (AnimId){ child, s->hot[child].gen }, depth_test, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_fixed_in_frame(AnimScene *s, AnimId id, bool fixed_in_frame, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    h->fixed_in_frame = fixed_in_frame;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_fixed_in_frame(s, (AnimId){ child, s->hot[child].gen }, fixed_in_frame, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_z_index(AnimScene *s, AnimId id, float z_index, bool recursive)
{
    apply_recursive(s, id, recursive, fn_set_z, &z_index);
}

void anim_set_trim(AnimScene *s, AnimId id, float start, float end, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    h->trim_start = MU_CLAMP(start, 0.0f, 1.0f);
    h->trim_end   = MU_CLAMP(end, 0.0f, 1.0f);
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_trim(s, (AnimId){ child, s->hot[child].gen }, start, end, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_visible(AnimScene *s, AnimId id, bool visible, bool recursive)
{
    apply_recursive(s, id, recursive, fn_set_vis, &visible);
}

void anim_set_billboard(AnimScene *s, AnimId id, bool billboard, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    s->cold[id.index].is_billboard = billboard;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_billboard(s, (AnimId){ child, s->hot[child].gen }, billboard, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_cap_join(AnimScene *s, AnimId id, AnimCapStyle cap, AnimJoinStyle join, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    s->cold[id.index].cap  = cap;
    s->cold[id.index].join = join;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_cap_join(s, (AnimId){ child, s->hot[child].gen }, cap, join, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_fill_rule(AnimScene *s, AnimId id, AnimFillRule rule, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;

    if (!h)
        return;
    s->cold[id.index].fill_rule = rule;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_set_fill_rule(s, (AnimId){ child, s->hot[child].gen }, rule, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_set_tag(AnimScene *s, AnimId id, const char *tag)
{
    VMobCold *c = anim__cold(s, id);
    if (!c)
        return;
    snprintf(c->tag, sizeof c->tag, "%s", tag ? tag : "");
}

/* --------------------------------------------------------------- hierarchy */

static void mark_subtree_dirty(AnimScene *s, uint32_t idx)
{
    uint32_t c = s->hot[idx].first_child;
    s->hot[idx].mat_dirty = true;
    while (c != 0u)
    {
        mark_subtree_dirty(s, c);
        c = s->hot[c].next_sibling;
    }
}

void anim_add(AnimScene *s, AnimId id)
{
    VMobHot *h = anim__hot(s, id);
    if (!h)
        return;
    h->in_scene = true;
}

void anim_add_child(AnimScene *s, AnimId parent, AnimId child)
{
    VMobHot *ph = anim__hot(s, parent);
    VMobHot *ch = anim__hot(s, child);
    uint32_t cur;

    if (!ph || !ch || parent.index == child.index)
        return;
    detach_from_parent(s, child.index);
    ch->parent       = parent.index;
    ch->next_sibling = 0u;
    if (ph->first_child == 0u)
    {
        ph->first_child = child.index;
    }
    else
    {
        cur = ph->first_child;
        while (s->hot[cur].next_sibling != 0u)
            cur = s->hot[cur].next_sibling;
        s->hot[cur].next_sibling = child.index;
    }
    mark_subtree_dirty(s, child.index);
}

void anim_remove(AnimScene *s, AnimId id)
{
    VMobHot *h = anim__hot(s, id);
    if (!h)
        return;
    h->in_scene = false;
}

uint32_t anim_child_count(const AnimScene *s, AnimId id)
{
    const VMobHot *h = anim__hot_const(s, id);
    uint32_t       n = 0u;
    uint32_t       c;

    if (!h)
        return 0u;
    c = h->first_child;
    while (c != 0u)
    {
        n++;
        c = s->hot[c].next_sibling;
    }
    return n;
}

AnimId anim_child_at(const AnimScene *s, AnimId id, uint32_t idx)
{
    const VMobHot *h = anim__hot_const(s, id);
    uint32_t       c;
    uint32_t       i = 0u;

    if (!h)
        return ANIM_ID_NIL;
    c = h->first_child;
    while (c != 0u)
    {
        if (i == idx)
            return (AnimId){ c, s->hot[c].gen };
        i++;
        c = s->hot[c].next_sibling;
    }
    return ANIM_ID_NIL;
}

AnimId anim_find_by_tag(const AnimScene *s, AnimId root, const char *tag)
{
    uint32_t c;

    if (!tag || !tag[0])
        return ANIM_ID_NIL;
    if (ANIM_ID_VALID(root))
    {
        const VMobHot *h = anim__hot_const(s, root);
        if (!h)
            return ANIM_ID_NIL;
        if (strcmp(s->cold[root.index].tag, tag) == 0)
            return root;
        c = h->first_child;
        while (c != 0u)
        {
            AnimId found = anim_find_by_tag(s, (AnimId){ c, s->hot[c].gen }, tag);
            if (ANIM_ID_VALID(found))
                return found;
            c = s->hot[c].next_sibling;
        }
        return ANIM_ID_NIL;
    }
    for (c = 1u; c < s->mob_cap; ++c)
    {
        if (s->hot[c].alive && strcmp(s->cold[c].tag, tag) == 0)
            return (AnimId){ c, s->hot[c].gen };
    }
    return ANIM_ID_NIL;
}

void anim__collect_subtree(const AnimScene *s, AnimId root, AnimId *out, uint32_t *io_count, uint32_t cap)
{
    const VMobHot *h = anim__hot_const(s, root);
    uint32_t       c;

    if (!h || *io_count >= cap)
        return;
    out[(*io_count)++] = root;
    c = h->first_child;
    while (c != 0u)
    {
        anim__collect_subtree(s, (AnimId){ c, s->hot[c].gen }, out, io_count, cap);
        c = s->hot[c].next_sibling;
    }
}

uint32_t anim__count_subtree(const AnimScene *s, AnimId root)
{
    const VMobHot *h = anim__hot_const(s, root);
    uint32_t       n = 0u;
    uint32_t       c;

    if (!h)
        return 0u;
    n = 1u;
    c = h->first_child;
    while (c != 0u)
    {
        n += anim__count_subtree(s, (AnimId){ c, s->hot[c].gen });
        c = s->hot[c].next_sibling;
    }
    return n;
}

static void sync_node_mat(AnimScene *s, uint32_t idx)
{
    VMobHot *h = &s->hot[idx];
    float    local[16];
    float    t[3] = { h->pos.x, h->pos.y, h->pos.z };
    float    sc[3] = { h->scale.x, h->scale.y, h->scale.z };
    uint32_t c;

    mu_mat4_trs(local, t, h->rot_quat, sc);
    if (h->parent != 0u)
        mu_mat4_mul(s->hot[h->parent].world_mat, local, h->world_mat);
    else
        memcpy(h->world_mat, local, sizeof local);
    h->mat_dirty = false;

    c = h->first_child;
    while (c != 0u)
    {
        sync_node_mat(s, c);
        c = s->hot[c].next_sibling;
    }
}

void anim__sync_world_mats(AnimScene *s)
{
    uint32_t i;

    for (i = 1u; i < s->mob_cap; ++i)
    {
        if (s->hot[i].alive && s->hot[i].parent == 0u)
            sync_node_mat(s, i);
    }
}

/* -------------------------------------------------------- transforms/layout */

AnimVec3 anim_pos(const AnimScene *s, AnimId id)
{
    const VMobHot *h = anim__hot_const(s, id);
    return h ? h->pos : ANIM_ORIGIN;
}

void anim_set_pos(AnimScene *s, AnimId id, AnimVec3 pos)
{
    VMobHot *h = anim__hot(s, id);
    if (!h)
        return;
    h->pos = pos;
    mark_subtree_dirty(s, id.index);
}

static void expand_bbox(AnimVec3 *mn, AnimVec3 *mx, bool *has, AnimVec3 p)
{
    if (!*has)
    {
        *mn  = p;
        *mx  = p;
        *has = true;
        return;
    }
    mn->x = MU_MIN(mn->x, p.x); mn->y = MU_MIN(mn->y, p.y); mn->z = MU_MIN(mn->z, p.z);
    mx->x = MU_MAX(mx->x, p.x); mx->y = MU_MAX(mx->y, p.y); mx->z = MU_MAX(mx->z, p.z);
}

static void bbox_node(const AnimScene *s, uint32_t idx, AnimVec3 *mn, AnimVec3 *mx, bool *has)
{
    const VMobHot *h = &s->hot[idx];
    uint32_t       si, ci, c;
    int            k;

    for (si = 0u; si < h->subpath_count; ++si)
    {
        SubpathMeta sm = s->subpaths[h->subpath_offset + si];
        for (ci = 0u; ci < sm.cubic_count; ++ci)
        {
            AnimCubic cb = s->cubics[sm.cubic_offset + ci];
            for (k = 0; k <= 4; ++k)
            {
                AnimVec3 lp, wp;
                float    pin[3], pout[3];
                anim__cubic_eval(cb, (float)k * 0.25f, &lp);
                pin[0] = lp.x; pin[1] = lp.y; pin[2] = lp.z;
                mu_mat4_transform_point(pout, h->world_mat, pin);
                wp = ANIM_V3(pout[0], pout[1], pout[2]);
                expand_bbox(mn, mx, has, wp);
            }
        }
    }
    for (si = 0u; si < h->surf_vert_count; ++si)
    {
        const SurfaceVert *sv = &s->surf_verts[h->surf_vert_offset + si];
        float              pin[3] = { sv->pos.x, sv->pos.y, sv->pos.z };
        float              pout[3];
        mu_mat4_transform_point(pout, h->world_mat, pin);
        expand_bbox(mn, mx, has, ANIM_V3(pout[0], pout[1], pout[2]));
    }
    c = h->first_child;
    while (c != 0u)
    {
        bbox_node(s, c, mn, mx, has);
        c = s->hot[c].next_sibling;
    }
}

void anim_bbox_world(const AnimScene *s, AnimId id, AnimVec3 *out_min, AnimVec3 *out_max)
{
    const VMobHot *h   = anim__hot_const(s, id);
    AnimVec3       mn  = ANIM_ORIGIN;
    AnimVec3       mx  = ANIM_ORIGIN;
    bool           has = false;

    if (h)
    {
        anim__sync_world_mats((AnimScene *)s);
        bbox_node(s, id.index, &mn, &mx, &has);
        if (!has)
        {
            mn = ANIM_V3(h->world_mat[12], h->world_mat[13], h->world_mat[14]);
            mx = mn;
        }
    }
    if (out_min) *out_min = mn;
    if (out_max) *out_max = mx;
}

AnimVec3 anim_center(const AnimScene *s, AnimId id)
{
    AnimVec3 mn, mx;
    anim_bbox_world(s, id, &mn, &mx);
    return ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
}

float anim_width(const AnimScene *s, AnimId id)
{
    AnimVec3 mn, mx;
    anim_bbox_world(s, id, &mn, &mx);
    return mx.x - mn.x;
}

float anim_height(const AnimScene *s, AnimId id)
{
    AnimVec3 mn, mx;
    anim_bbox_world(s, id, &mn, &mx);
    return mx.y - mn.y;
}

void anim_shift(AnimScene *s, AnimId id, AnimVec3 delta)
{
    VMobHot *h = anim__hot(s, id);
    if (!h)
        return;
    h->pos.x += delta.x;
    h->pos.y += delta.y;
    h->pos.z += delta.z;
    mark_subtree_dirty(s, id.index);
}

void anim_move_to(AnimScene *s, AnimId id, AnimVec3 target)
{
    AnimVec3 c = anim_center(s, id);
    anim_shift(s, id, ANIM_V3(target.x - c.x, target.y - c.y, target.z - c.z));
}

void anim_scale(AnimScene *s, AnimId id, float factor)
{
    anim_scale_vec(s, id, ANIM_V3(factor, factor, factor));
}

void anim_scale_vec(AnimScene *s, AnimId id, AnimVec3 factors)
{
    VMobHot *h = anim__hot(s, id);
    if (!h)
        return;
    h->scale.x *= factors.x;
    h->scale.y *= factors.y;
    h->scale.z *= factors.z;
    mark_subtree_dirty(s, id.index);
}

void anim_scale_to_width(AnimScene *s, AnimId id, float width)
{
    float w = anim_width(s, id);
    if (w > 1e-5f)
        anim_scale(s, id, width / w);
}

void anim_scale_to_height(AnimScene *s, AnimId id, float height)
{
    float h = anim_height(s, id);
    if (h > 1e-5f)
        anim_scale(s, id, height / h);
}

void anim_rotate(AnimScene *s, AnimId id, float angle_rad, AnimVec3 axis)
{
    VMobHot *h = anim__hot(s, id);
    float    ax[3] = { axis.x, axis.y, axis.z };
    float    nax[3];
    float    dq[4];
    float    nq[4];

    if (!h)
        return;
    mu_vec3_normalize(nax, ax);
    if (mu_vec3_length2(nax) < 1e-6f)
        nax[2] = 1.0f;
    mu_quat_axis_angle(dq, nax, angle_rad);
    mu_quat_mul(nq, dq, h->rot_quat);
    mu_quat_normalize(h->rot_quat, nq);
    mark_subtree_dirty(s, id.index);
}

void anim_next_to(AnimScene *s, AnimId id, AnimId ref, AnimVec3 dir, float buff)
{
    AnimVec3 rmin, rmax, mmin, mmax, rc, mc;
    float    dlen;
    AnimVec3 ndir;
    AnimVec3 target;

    anim_bbox_world(s, ref, &rmin, &rmax);
    anim_bbox_world(s, id, &mmin, &mmax);
    rc   = ANIM_V3(0.5f * (rmin.x + rmax.x), 0.5f * (rmin.y + rmax.y), 0.5f * (rmin.z + rmax.z));
    mc   = ANIM_V3(0.5f * (mmin.x + mmax.x), 0.5f * (mmin.y + mmax.y), 0.5f * (mmin.z + mmax.z));
    dlen = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    ndir = dlen > 1e-6f ? ANIM_V3(dir.x / dlen, dir.y / dlen, dir.z / dlen) : ANIM_RIGHT;

    target = rc;
    if (fabsf(dir.x) > 1e-4f)
    {
        float sx = (dir.x > 0.0f) ? (rmax.x - mmin.x) : (rmin.x - mmax.x);
        target.x = mc.x + sx + ndir.x * buff;
    }
    if (fabsf(dir.y) > 1e-4f)
    {
        float sy = (dir.y > 0.0f) ? (rmax.y - mmin.y) : (rmin.y - mmax.y);
        target.y = mc.y + sy + ndir.y * buff;
    }
    if (fabsf(dir.z) > 1e-4f)
    {
        float sz = (dir.z > 0.0f) ? (rmax.z - mmin.z) : (rmin.z - mmax.z);
        target.z = mc.z + sz + ndir.z * buff;
    }
    anim_move_to(s, id, target);
}

void anim_align_to(AnimScene *s, AnimId id, AnimId ref, AnimVec3 dir)
{
    AnimVec3 rmin, rmax, mmin, mmax;
    AnimVec3 delta = ANIM_ORIGIN;

    anim_bbox_world(s, ref, &rmin, &rmax);
    anim_bbox_world(s, id, &mmin, &mmax);
    if (dir.x > 0.5f)       delta.x = rmax.x - mmax.x;
    else if (dir.x < -0.5f) delta.x = rmin.x - mmin.x;
    if (dir.y > 0.5f)       delta.y = rmax.y - mmax.y;
    else if (dir.y < -0.5f) delta.y = rmin.y - mmin.y;
    if (dir.z > 0.5f)       delta.z = rmax.z - mmax.z;
    else if (dir.z < -0.5f) delta.z = rmin.z - mmin.z;
    anim_shift(s, id, delta);
}

void anim_arrange(AnimScene *s, const AnimId *ids, uint32_t count, AnimVec3 dir, float buff)
{
    uint32_t i;
    AnimVec3 total_min = ANIM_ORIGIN, total_max = ANIM_ORIGIN;
    bool     has = false;

    if (count == 0u)
        return;
    for (i = 1u; i < count; ++i)
        anim_next_to(s, ids[i], ids[i - 1u], dir, buff);
    for (i = 0u; i < count; ++i)
    {
        AnimVec3 mn, mx;
        anim_bbox_world(s, ids[i], &mn, &mx);
        expand_bbox(&total_min, &total_max, &has, mn);
        expand_bbox(&total_min, &total_max, &has, mx);
    }
    {
        AnimVec3 c = ANIM_V3(0.5f * (total_min.x + total_max.x), 0.5f * (total_min.y + total_max.y), 0.5f * (total_min.z + total_max.z));
        for (i = 0u; i < count; ++i)
            anim_shift(s, ids[i], ANIM_V3(-c.x, -c.y, -c.z));
    }
}

void anim_arrange_in_grid(AnimScene *s, const AnimId *ids, uint32_t count, uint32_t rows, uint32_t cols, float h_buff, float v_buff)
{
    uint32_t r, c, i;
    float    max_w = 0.0f, max_h = 0.0f;
    float    step_x, step_y, ox, oy;

    if (count == 0u || rows == 0u || cols == 0u)
        return;
    for (i = 0u; i < count; ++i)
    {
        max_w = MU_MAX(max_w, anim_width(s, ids[i]));
        max_h = MU_MAX(max_h, anim_height(s, ids[i]));
    }
    step_x = max_w + h_buff;
    step_y = max_h + v_buff;
    ox     = -0.5f * (float)(cols - 1u) * step_x;
    oy     =  0.5f * (float)(rows - 1u) * step_y;
    for (i = 0u; i < count; ++i)
    {
        r = i / cols;
        c = i % cols;
        if (r >= rows)
            break;
        anim_move_to(s, ids[i], ANIM_V3(ox + (float)c * step_x, oy - (float)r * step_y, 0.0f));
    }
}

void anim_to_edge(AnimScene *s, AnimId id, AnimVec3 dir, float buff)
{
    AnimVec3 mn, mx;
    float    hw = 0.5f * ANIM_WORLD_W_16_9;
    float    hh = 0.5f * ANIM_WORLD_H;
    AnimVec3 delta = ANIM_ORIGIN;

    anim_bbox_world(s, id, &mn, &mx);
    if (dir.x > 0.5f)       delta.x = (hw - buff) - mx.x;
    else if (dir.x < -0.5f) delta.x = (-hw + buff) - mn.x;
    if (dir.y > 0.5f)       delta.y = (hh - buff) - mx.y;
    else if (dir.y < -0.5f) delta.y = (-hh + buff) - mn.y;
    anim_shift(s, id, delta);
}

void anim_to_corner(AnimScene *s, AnimId id, AnimVec3 corner, float buff)
{
    anim_to_edge(s, id, corner, buff);
}

/* --------------------------------------------------------- standard library */

AnimId anim_group(AnimScene *s, const AnimId *children, uint32_t count)
{
    AnimId   g = anim_vmob_create(s);
    uint32_t i;

    if (!ANIM_ID_VALID(g))
        return g;
    s->hot[g.index].stroke_color.a = 0.0f;
    for (i = 0u; i < count; ++i)
        if (ANIM_ID_VALID(children[i]))
            anim_add_child(s, g, children[i]);
    return g;
}

AnimId anim_line(AnimScene *s, AnimVec3 a, AnimVec3 b)
{
    AnimId id = anim_vmob_create(s);
    anim_vmob_begin_subpath(s, id, a);
    anim_vmob_line_to(s, id, b);
    return id;
}

AnimId anim_dashed_line(AnimScene *s, AnimVec3 a, AnimVec3 b, float dash_len, float gap_len)
{
    AnimId   id  = anim_vmob_create(s);
    AnimVec3 d   = ANIM_V3(b.x - a.x, b.y - a.y, b.z - a.z);
    float    len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    float    cur = 0.0f;

    if (len < 1e-5f || dash_len <= 1e-4f)
        return anim_line(s, a, b);
    while (cur < len)
    {
        float    t0 = cur / len;
        float    t1 = MU_MIN(1.0f, (cur + dash_len) / len);
        AnimVec3 p0 = v3_lerp(a, b, t0);
        AnimVec3 p1 = v3_lerp(a, b, t1);
        anim_vmob_begin_subpath(s, id, p0);
        anim_vmob_line_to(s, id, p1);
        cur += dash_len + MU_MAX(gap_len, 1e-4f);
    }
    return id;
}

static void add_arrow_tip(AnimScene *s, AnimId tip, AnimVec3 tip_pt, AnimVec3 dir, float tip_len, float tip_width)
{
    AnimVec3 perp = ANIM_V3(-dir.y, dir.x, 0.0f);
    float    plen = sqrtf(perp.x * perp.x + perp.y * perp.y);
    AnimVec3 base;
    AnimVec3 p_l, p_r;

    if (plen < 1e-5f)
        perp = ANIM_V3(1.0f, 0.0f, 0.0f);
    else
        perp = ANIM_V3(perp.x / plen, perp.y / plen, 0.0f);

    base = ANIM_V3(tip_pt.x - dir.x * tip_len, tip_pt.y - dir.y * tip_len, tip_pt.z - dir.z * tip_len);
    p_l  = ANIM_V3(base.x + perp.x * (0.5f * tip_width), base.y + perp.y * (0.5f * tip_width), base.z);
    p_r  = ANIM_V3(base.x - perp.x * (0.5f * tip_width), base.y - perp.y * (0.5f * tip_width), base.z);
    anim_vmob_begin_subpath(s, tip, tip_pt);
    anim_vmob_line_to(s, tip, p_l);
    anim_vmob_line_to(s, tip, p_r);
    anim_vmob_close_subpath(s, tip);
    anim_set_fill(s, tip, ANIM_WHITE, false);
    anim_set_stroke(s, tip, ANIM_WHITE, 1.0f, false);
}

AnimId anim_arrow(AnimScene *s, AnimVec3 a, AnimVec3 b, float tip_len, float tip_width)
{
    AnimVec3 d    = ANIM_V3(b.x - a.x, b.y - a.y, b.z - a.z);
    float    len  = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    AnimVec3 dir  = (len > 1e-5f) ? ANIM_V3(d.x / len, d.y / len, d.z / len) : ANIM_RIGHT;
    float    tl   = (tip_len > 0.0f) ? tip_len : MU_MIN(0.24f, len * 0.35f);
    float    tw   = (tip_width > 0.0f) ? tip_width : tl * 0.85f;
    AnimVec3 end  = ANIM_V3(b.x - dir.x * tl * 0.7f, b.y - dir.y * tl * 0.7f, b.z - dir.z * tl * 0.7f);
    AnimId   grp  = anim_group(s, NULL, 0u);
    AnimId   stem = anim_line(s, a, (len > tl) ? end : a);
    AnimId   tip  = anim_vmob_create(s);

    add_arrow_tip(s, tip, b, dir, tl, tw);
    anim_add_child(s, grp, stem);
    anim_add_child(s, grp, tip);
    return grp;
}

void anim__arrow_set(AnimScene *s, AnimId arrow_id, AnimVec3 a, AnimVec3 b, float tip_len, float tip_width)
{
    AnimVec3 d    = ANIM_V3(b.x - a.x, b.y - a.y, b.z - a.z);
    float    len  = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    AnimVec3 dir  = (len > 1e-5f) ? ANIM_V3(d.x / len, d.y / len, d.z / len) : ANIM_RIGHT;
    float    tl   = (tip_len > 0.0f) ? tip_len : MU_MIN(0.24f, len * 0.35f);
    float    tw   = (tip_width > 0.0f) ? tip_width : tl * 0.85f;
    AnimVec3 end  = ANIM_V3(b.x - dir.x * tl * 0.7f, b.y - dir.y * tl * 0.7f, b.z - dir.z * tl * 0.7f);
    AnimId   stem = anim_child_at(s, arrow_id, 0u);
    AnimId   tip  = anim_child_at(s, arrow_id, 1u);
    if (!ANIM_ID_VALID(stem) || !ANIM_ID_VALID(tip))
        return;
    anim_vmob_clear_paths(s, stem);
    anim_vmob_begin_subpath(s, stem, a);
    anim_vmob_line_to(s, stem, (len > tl) ? end : a);
    {
        VMobHot  *th = &s->hot[tip.index];
        AnimColor fc = th->fill_color;
        AnimColor sc = th->stroke_color;
        float     sw = th->stroke_width;
        anim_vmob_clear_paths(s, tip);
        add_arrow_tip(s, tip, b, dir, tl, tw);
        th->fill_color   = fc;
        th->stroke_color = sc;
        th->stroke_width = sw;
    }
}

static void append_arc_cubics(AnimScene *s, AnimId id, float rx, float ry, float a0, float sweep, bool start_subpath);

void anim__arc_set(AnimScene *s, AnimId arc_id, float radius, float start_angle, float angle)
{
    if (!anim__hot(s, arc_id))
        return;
    anim_vmob_clear_paths(s, arc_id);
    append_arc_cubics(s, arc_id, radius, radius, start_angle, angle, true);
}

void anim__replace_from(AnimScene *s, AnimId dst, AnimId src, bool preserve_child_colors)
{
    VMobHot *dh = anim__hot(s, dst);
    VMobHot *sh = anim__hot(s, src);
    uint32_t c;

    if (!dh || !sh)
        return;

    c = dh->first_child;
    while (c != 0u)
    {
        uint32_t next = s->hot[c].next_sibling;
        anim_vmob_destroy(s, (AnimId){ c, s->hot[c].gen });
        c = next;
    }
    dh->first_child = 0u;

    dh->subpath_offset   = sh->subpath_offset;
    dh->subpath_count    = sh->subpath_count;
    dh->surf_vert_offset = sh->surf_vert_offset;
    dh->surf_vert_count  = sh->surf_vert_count;
    dh->surf_idx_offset  = sh->surf_idx_offset;
    dh->surf_idx_count   = sh->surf_idx_count;

    c = sh->first_child;
    while (c != 0u)
    {
        uint32_t next = s->hot[c].next_sibling;
        VMobHot *ch   = &s->hot[c];
        ch->parent       = 0u;
        ch->next_sibling = 0u;
        anim_add_child(s, dst, (AnimId){ c, ch->gen });
        ch->in_scene = dh->in_scene;
        ch->visible  = dh->visible;
        if (preserve_child_colors)
        {
            ch->opacity *= dh->opacity;
            ch->glow     = MU_MAX(ch->glow, dh->glow);
        }
        else
        {
            ch->fill_color   = dh->fill_color;
            ch->stroke_color = dh->stroke_color;
            ch->stroke_width = dh->stroke_width;
            ch->opacity      = dh->opacity;
            ch->glow         = dh->glow;
        }
        c = next;
    }

    sh->first_child     = 0u;
    sh->subpath_count   = 0u;
    sh->surf_vert_count = 0u;
    sh->surf_idx_count  = 0u;
    anim_vmob_destroy(s, src);
}

AnimId anim_double_arrow(AnimScene *s, AnimVec3 a, AnimVec3 b, float tip_len, float tip_width)
{
    AnimVec3 d    = ANIM_V3(b.x - a.x, b.y - a.y, b.z - a.z);
    float    len  = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    AnimVec3 dir  = (len > 1e-5f) ? ANIM_V3(d.x / len, d.y / len, d.z / len) : ANIM_RIGHT;
    float    tl   = (tip_len > 0.0f) ? tip_len : MU_MIN(0.24f, len * 0.25f);
    float    tw   = (tip_width > 0.0f) ? tip_width : tl * 0.85f;
    AnimVec3 sa   = ANIM_V3(a.x + dir.x * tl * 0.7f, a.y + dir.y * tl * 0.7f, a.z + dir.z * tl * 0.7f);
    AnimVec3 sb   = ANIM_V3(b.x - dir.x * tl * 0.7f, b.y - dir.y * tl * 0.7f, b.z - dir.z * tl * 0.7f);
    AnimId   grp  = anim_group(s, NULL, 0u);
    AnimId   stem = anim_line(s, sa, sb);
    AnimId   tip1 = anim_vmob_create(s);
    AnimId   tip2 = anim_vmob_create(s);

    add_arrow_tip(s, tip1, b, dir, tl, tw);
    add_arrow_tip(s, tip2, a, ANIM_V3(-dir.x, -dir.y, -dir.z), tl, tw);
    anim_add_child(s, grp, stem);
    anim_add_child(s, grp, tip1);
    anim_add_child(s, grp, tip2);
    return grp;
}

AnimId anim_vector(AnimScene *s, AnimVec3 dir)
{
    return anim_arrow(s, ANIM_ORIGIN, dir, 0.22f, 0.18f);
}

static void append_arc_cubics(AnimScene *s, AnimId id, float rx, float ry, float a0, float sweep, bool start_subpath)
{
    int   segs = (int)ceilf(fabsf(sweep) / (ANIM_PI * 0.5f));
    float dt;
    int   i;

    if (segs < 1)
        segs = 1;
    dt = sweep / (float)segs;
    for (i = 0; i < segs; ++i)
    {
        float    t0 = a0 + (float)i * dt;
        float    t1 = t0 + dt;
        float    k  = (4.0f / 3.0f) * tanf(0.25f * dt);
        float    c0 = cosf(t0), s0 = sinf(t0);
        float    c1 = cosf(t1), s1 = sinf(t1);
        AnimVec3 p0 = ANIM_V3(rx * c0, ry * s0, 0.0f);
        AnimVec3 p1 = ANIM_V3(rx * (c0 - k * s0), ry * (s0 + k * c0), 0.0f);
        AnimVec3 p2 = ANIM_V3(rx * (c1 + k * s1), ry * (s1 - k * c1), 0.0f);
        AnimVec3 p3 = ANIM_V3(rx * c1, ry * s1, 0.0f);

        if (i == 0 && start_subpath)
            anim_vmob_begin_subpath(s, id, p0);
        anim_vmob_cubic_to(s, id, p1, p2, p3);
    }
}

AnimId anim_arc(AnimScene *s, float radius, float start_angle, float angle)
{
    AnimId id = anim_vmob_create(s);
    append_arc_cubics(s, id, radius, radius, start_angle, angle, true);
    return id;
}

AnimId anim_circle(AnimScene *s, float radius)
{
    AnimId id = anim_arc(s, radius, 0.0f, ANIM_TAU);
    anim_vmob_close_subpath(s, id);
    return id;
}

AnimId anim_annulus(AnimScene *s, float r_inner, float r_outer)
{
    AnimId id = anim_vmob_create(s);
    append_arc_cubics(s, id, r_outer, r_outer, 0.0f, ANIM_TAU, true);
    anim_vmob_close_subpath(s, id);
    append_arc_cubics(s, id, r_inner, r_inner, ANIM_TAU, -ANIM_TAU, true);
    anim_vmob_close_subpath(s, id);
    return id;
}

AnimId anim_ellipse(AnimScene *s, float width, float height)
{
    AnimId id = anim_vmob_create(s);
    append_arc_cubics(s, id, 0.5f * width, 0.5f * height, 0.0f, ANIM_TAU, true);
    anim_vmob_close_subpath(s, id);
    return id;
}

AnimId anim_rect(AnimScene *s, float width, float height)
{
    float    hw  = 0.5f * width;
    float    hh  = 0.5f * height;
    AnimVec3 pts[4] = {
        ANIM_V3(-hw, -hh, 0.0f),
        ANIM_V3( hw, -hh, 0.0f),
        ANIM_V3( hw,  hh, 0.0f),
        ANIM_V3(-hw,  hh, 0.0f),
    };
    return anim_polygon(s, pts, 4u);
}

AnimId anim_rounded_rect(AnimScene *s, float width, float height, float corner_radius)
{
    AnimId id = anim_vmob_create(s);
    float  hw = 0.5f * width;
    float  hh = 0.5f * height;
    float  r  = MU_CLAMP(corner_radius, 0.0f, MU_MIN(hw, hh));
    float  k  = 0.5522847498f * r;

    if (r < 1e-4f)
        return anim_rect(s, width, height);

    anim_vmob_begin_subpath(s, id, ANIM_V3(-hw + r, -hh, 0.0f));
    anim_vmob_line_to(s, id, ANIM_V3(hw - r, -hh, 0.0f));
    anim_vmob_cubic_to(s, id, ANIM_V3(hw - r + k, -hh, 0.0f), ANIM_V3(hw, -hh + r - k, 0.0f), ANIM_V3(hw, -hh + r, 0.0f));
    anim_vmob_line_to(s, id, ANIM_V3(hw, hh - r, 0.0f));
    anim_vmob_cubic_to(s, id, ANIM_V3(hw, hh - r + k, 0.0f), ANIM_V3(hw - r + k, hh, 0.0f), ANIM_V3(hw - r, hh, 0.0f));
    anim_vmob_line_to(s, id, ANIM_V3(-hw + r, hh, 0.0f));
    anim_vmob_cubic_to(s, id, ANIM_V3(-hw + r - k, hh, 0.0f), ANIM_V3(-hw, hh - r + k, 0.0f), ANIM_V3(-hw, hh - r, 0.0f));
    anim_vmob_line_to(s, id, ANIM_V3(-hw, -hh + r, 0.0f));
    anim_vmob_cubic_to(s, id, ANIM_V3(-hw, -hh + r - k, 0.0f), ANIM_V3(-hw + r - k, -hh, 0.0f), ANIM_V3(-hw + r, -hh, 0.0f));
    anim_vmob_close_subpath(s, id);
    return id;
}

AnimId anim_square(AnimScene *s, float side)
{
    return anim_rect(s, side, side);
}

AnimId anim_polygon(AnimScene *s, const AnimVec3 *pts, uint32_t count)
{
    AnimId   id = anim_vmob_create(s);
    uint32_t i;

    if (count == 0u)
        return id;
    anim_vmob_begin_subpath(s, id, pts[0]);
    for (i = 1u; i < count; ++i)
        anim_vmob_line_to(s, id, pts[i]);
    anim_vmob_close_subpath(s, id);
    return id;
}

AnimId anim_regular_polygon(AnimScene *s, uint32_t sides, float radius)
{
    AnimVec3 pts[128];
    uint32_t n = MU_CLAMP(sides, 3u, 128u);
    uint32_t i;

    for (i = 0u; i < n; ++i)
    {
        float a = ANIM_PI * 0.5f + (float)i * (ANIM_TAU / (float)n);
        pts[i]  = ANIM_V3(radius * cosf(a), radius * sinf(a), 0.0f);
    }
    return anim_polygon(s, pts, n);
}

AnimId anim_star(AnimScene *s, uint32_t points, float r_outer, float r_inner)
{
    AnimVec3 pts[128];
    uint32_t n = MU_CLAMP(points, 3u, 64u) * 2u;
    uint32_t i;

    for (i = 0u; i < n; ++i)
    {
        float r = (i & 1u) ? r_inner : r_outer;
        float a = ANIM_PI * 0.5f + (float)i * (ANIM_TAU / (float)n);
        pts[i]  = ANIM_V3(r * cosf(a), r * sinf(a), 0.0f);
    }
    return anim_polygon(s, pts, n);
}

AnimId anim_dot(AnimScene *s, AnimVec3 pos, float radius)
{
    AnimId id = anim_circle(s, radius > 0.0f ? radius : 0.08f);
    anim_set_fill(s, id, ANIM_WHITE, false);
    anim_set_stroke(s, id, ANIM_WHITE, 0.0f, false);
    anim_set_pos(s, id, pos);
    return id;
}

AnimId anim_brace(AnimScene *s, AnimId target, AnimVec3 dir, float buff)
{
    AnimVec3 mn, mx, c;
    float    span;
    AnimId   id = anim_vmob_create(s);
    float    h  = 0.16f;
    float    tip = 0.30f;

    anim_bbox_world(s, target, &mn, &mx);
    c = ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
    if (fabsf(dir.y) >= fabsf(dir.x))
    {
        float hw = 0.5f * MU_MAX(mx.x - mn.x, 0.35f);
        float sgn = (dir.y >= 0.0f) ? 1.0f : -1.0f;
        /* Canonical horizontal brace centered at origin, tip pointing +dir.y */
        anim_vmob_begin_subpath(s, id, ANIM_V3(-hw, -sgn * h, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(-hw, 0.0f, 0.0f), ANIM_V3(-hw * 0.55f, 0.0f, 0.0f), ANIM_V3(-hw * 0.30f, 0.0f, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(-hw * 0.10f, 0.0f, 0.0f), ANIM_V3(-0.04f, sgn * h * 0.4f, 0.0f), ANIM_V3(0.0f, sgn * tip, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(0.04f, sgn * h * 0.4f, 0.0f), ANIM_V3(hw * 0.10f, 0.0f, 0.0f), ANIM_V3(hw * 0.30f, 0.0f, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(hw * 0.55f, 0.0f, 0.0f), ANIM_V3(hw, 0.0f, 0.0f), ANIM_V3(hw, -sgn * h, 0.0f));
    }
    else
    {
        float hh  = 0.5f * MU_MAX(mx.y - mn.y, 0.35f);
        float sgn = (dir.x >= 0.0f) ? 1.0f : -1.0f;
        span = hh;
        anim_vmob_begin_subpath(s, id, ANIM_V3(-sgn * h, -span, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(0.0f, -span, 0.0f), ANIM_V3(0.0f, -span * 0.55f, 0.0f), ANIM_V3(0.0f, -span * 0.30f, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(0.0f, -span * 0.10f, 0.0f), ANIM_V3(sgn * h * 0.4f, -0.04f, 0.0f), ANIM_V3(sgn * tip, 0.0f, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(sgn * h * 0.4f, 0.04f, 0.0f), ANIM_V3(0.0f, span * 0.10f, 0.0f), ANIM_V3(0.0f, span * 0.30f, 0.0f));
        anim_vmob_cubic_to(s, id, ANIM_V3(0.0f, span * 0.55f, 0.0f), ANIM_V3(0.0f, span, 0.0f), ANIM_V3(-sgn * h, span, 0.0f));
    }
    anim_set_stroke(s, id, ANIM_WHITE, 3.0f, false);
    anim_move_to(s, id, c);
    anim_next_to(s, id, target, dir, buff);
    return id;
}

AnimId anim_number_line(AnimScene *s, float min_v, float max_v, float step, float length, bool include_ticks)
{
    AnimId grp  = anim_group(s, NULL, 0u);
    float  half = 0.5f * length;
    AnimId base = anim_line(s, ANIM_V3(-half, 0.0f, 0.0f), ANIM_V3(half, 0.0f, 0.0f));

    anim_add_child(s, grp, base);
    if (include_ticks && step > 1e-5f && max_v > min_v)
    {
        float v;
        for (v = min_v; v <= max_v + 1e-4f * step; v += step)
        {
            float  u    = (v - min_v) / (max_v - min_v);
            float  x    = -half + u * length;
            AnimId tick = anim_line(s, ANIM_V3(x, -0.09f, 0.0f), ANIM_V3(x, 0.09f, 0.0f));
            anim_set_stroke(s, tick, ANIM_GREY_B, 2.5f, false);
            anim_add_child(s, grp, tick);
        }
    }
    return grp;
}

AnimId anim_axes(AnimScene *s, const AnimAxesDesc *desc)
{
    AnimAxesDesc d   = desc ? *desc : (AnimAxesDesc){ -5.0f, 5.0f, 1.0f, -3.0f, 3.0f, 1.0f, 10.0f, 6.0f, true, true, true, false, NULL };
    AnimId       grp = anim_group(s, NULL, 0u);
    VMobCold    *cold;
    float        x_span = MU_MAX(d.x_max - d.x_min, 1e-4f);
    float        y_span = MU_MAX(d.y_max - d.y_min, 1e-4f);
    float        zx     = -0.5f * d.x_length + (-d.x_min / x_span) * d.x_length;
    float        zy     = -0.5f * d.y_length + (-d.y_min / y_span) * d.y_length;
    AnimId       x_ax, y_ax;
    float        v;

    if (d.x_min > 0.0f || d.x_max < 0.0f) zx = -0.5f * d.x_length;
    if (d.y_min > 0.0f || d.y_max < 0.0f) zy = -0.5f * d.y_length;

    cold = anim__cold(s, grp);
    cold->is_axes    = true;
    cold->axes_x_min = d.x_min;
    cold->axes_x_max = d.x_max;
    cold->axes_x_len = d.x_length;
    cold->axes_y_min = d.y_min;
    cold->axes_y_max = d.y_max;
    cold->axes_y_len = d.y_length;

    if (d.include_grid)
    {
        AnimId grid = anim_vmob_create(s);
        if (d.x_step > 1e-5f)
        {
            for (v = d.x_min; v <= d.x_max + 1e-4f * d.x_step; v += d.x_step)
            {
                float gx = -0.5f * d.x_length + ((v - d.x_min) / x_span) * d.x_length;
                anim_vmob_begin_subpath(s, grid, ANIM_V3(gx, -0.5f * d.y_length, 0.0f));
                anim_vmob_line_to(s, grid, ANIM_V3(gx, 0.5f * d.y_length, 0.0f));
            }
        }
        if (d.y_step > 1e-5f)
        {
            for (v = d.y_min; v <= d.y_max + 1e-4f * d.y_step; v += d.y_step)
            {
                float gy = -0.5f * d.y_length + ((v - d.y_min) / y_span) * d.y_length;
                anim_vmob_begin_subpath(s, grid, ANIM_V3(-0.5f * d.x_length, gy, 0.0f));
                anim_vmob_line_to(s, grid, ANIM_V3(0.5f * d.x_length, gy, 0.0f));
            }
        }
        anim_set_stroke(s, grid, anim_hex_a(0x3B5370, 0.45f), 1.4f, false);
        anim_add_child(s, grp, grid);
    }

    x_ax = d.include_tips ? anim_arrow(s, ANIM_V3(-0.5f * d.x_length, zy, 0.0f), ANIM_V3(0.5f * d.x_length, zy, 0.0f), 0.18f, 0.14f)
                          : anim_line(s, ANIM_V3(-0.5f * d.x_length, zy, 0.0f), ANIM_V3(0.5f * d.x_length, zy, 0.0f));
    y_ax = d.include_tips ? anim_arrow(s, ANIM_V3(zx, -0.5f * d.y_length, 0.0f), ANIM_V3(zx, 0.5f * d.y_length, 0.0f), 0.18f, 0.14f)
                          : anim_line(s, ANIM_V3(zx, -0.5f * d.y_length, 0.0f), ANIM_V3(zx, 0.5f * d.y_length, 0.0f));
    anim_set_stroke(s, x_ax, ANIM_GREY_A, 3.0f, true);
    anim_set_stroke(s, y_ax, ANIM_GREY_A, 3.0f, true);
    anim_add_child(s, grp, x_ax);
    anim_add_child(s, grp, y_ax);

    if (d.include_ticks)
    {
        AnimId ticks = anim_vmob_create(s);
        if (d.x_step > 1e-5f)
        {
            float v_start = ceilf((d.x_min - 1e-4f * d.x_step) / d.x_step) * d.x_step;
            for (v = v_start; v <= d.x_max + 1e-4f * d.x_step; v += d.x_step)
            {
                float tx = -0.5f * d.x_length + ((v - d.x_min) / x_span) * d.x_length;
                if (fabsf(v) < 1e-3f) continue;
                anim_vmob_begin_subpath(s, ticks, ANIM_V3(tx, zy - 0.08f, 0.0f));
                anim_vmob_line_to(s, ticks, ANIM_V3(tx, zy + 0.08f, 0.0f));
                if (d.include_numbers)
                {
                    char   buf[24];
                    AnimId lbl;
                    float  vv = (fabsf(v) < 1e-3f) ? 0.0f : v;
                    snprintf(buf, sizeof buf, fabsf(vv - roundf(vv)) < 0.05f ? "%.0f" : "%.1f", (double)vv);
                    lbl = anim_text(s, d.font_path, buf, 0.24f);
                    anim_set_color(s, lbl, ANIM_GREY_B, true);
                    anim_move_to(s, lbl, ANIM_V3(tx, zy - 0.28f, 0.0f));
                    anim_add_child(s, grp, lbl);
                }
            }
        }
        if (d.y_step > 1e-5f)
        {
            float v_start = ceilf((d.y_min - 1e-4f * d.y_step) / d.y_step) * d.y_step;
            for (v = v_start; v <= d.y_max + 1e-4f * d.y_step; v += d.y_step)
            {
                float ty = -0.5f * d.y_length + ((v - d.y_min) / y_span) * d.y_length;
                if (fabsf(v) < 1e-3f) continue;
                anim_vmob_begin_subpath(s, ticks, ANIM_V3(zx - 0.08f, ty, 0.0f));
                anim_vmob_line_to(s, ticks, ANIM_V3(zx + 0.08f, ty, 0.0f));
                if (d.include_numbers)
                {
                    char   buf[24];
                    AnimId lbl;
                    float  vv = (fabsf(v) < 1e-3f) ? 0.0f : v;
                    snprintf(buf, sizeof buf, fabsf(vv - roundf(vv)) < 0.05f ? "%.0f" : "%.1f", (double)vv);
                    lbl = anim_text(s, d.font_path, buf, 0.24f);
                    anim_set_color(s, lbl, ANIM_GREY_B, true);
                    anim_move_to(s, lbl, ANIM_V3(zx - 0.30f, ty, 0.0f));
                    anim_add_child(s, grp, lbl);
                }
            }
        }
        anim_set_stroke(s, ticks, ANIM_GREY_A, 2.5f, false);
        anim_add_child(s, grp, ticks);
    }
    return grp;
}

AnimVec3 anim_axes_c2p(const AnimScene *s, AnimId axes_id, float x, float y)
{
    const VMobHot  *h = anim__hot_const(s, axes_id);
    const VMobCold *c = h ? &s->cold[axes_id.index] : NULL;
    float           lx, ly;
    float           pin[3], pout[3];

    if (!h || !c || !c->is_axes)
        return ANIM_V3(x, y, 0.0f);
    anim__sync_world_mats((AnimScene *)s);
    lx = -0.5f * c->axes_x_len + ((x - c->axes_x_min) / MU_MAX(c->axes_x_max - c->axes_x_min, 1e-5f)) * c->axes_x_len;
    ly = -0.5f * c->axes_y_len + ((y - c->axes_y_min) / MU_MAX(c->axes_y_max - c->axes_y_min, 1e-5f)) * c->axes_y_len;
    pin[0] = lx; pin[1] = ly; pin[2] = 0.0f;
    mu_mat4_transform_point(pout, h->world_mat, pin);
    return ANIM_V3(pout[0], pout[1], pout[2]);
}

void anim_axes_p2c(const AnimScene *s, AnimId axes_id, AnimVec3 world_pt, float *out_x, float *out_y)
{
    const VMobHot  *h = anim__hot_const(s, axes_id);
    const VMobCold *c = h ? &s->cold[axes_id.index] : NULL;
    float           inv[16];
    float           pin[3] = { world_pt.x, world_pt.y, world_pt.z };
    float           ploc[3];

    if (!h || !c || !c->is_axes)
    {
        if (out_x) *out_x = world_pt.x;
        if (out_y) *out_y = world_pt.y;
        return;
    }
    anim__sync_world_mats((AnimScene *)s);
    mu_mat4_inverse(h->world_mat, inv);
    mu_mat4_transform_point(ploc, inv, pin);
    if (out_x)
        *out_x = c->axes_x_min + ((ploc[0] + 0.5f * c->axes_x_len) / MU_MAX(c->axes_x_len, 1e-5f)) * (c->axes_x_max - c->axes_x_min);
    if (out_y)
        *out_y = c->axes_y_min + ((ploc[1] + 0.5f * c->axes_y_len) / MU_MAX(c->axes_y_len, 1e-5f)) * (c->axes_y_max - c->axes_y_min);
}

static void add_catmull_rom_curve(AnimScene *s, AnimId id, const AnimVec3 *pts, uint32_t n)
{
    uint32_t i;

    if (n < 2u)
        return;
    anim_vmob_begin_subpath(s, id, pts[0]);
    for (i = 0u; i + 1u < n; ++i)
    {
        AnimVec3 pm1 = (i > 0u) ? pts[i - 1u] : pts[0];
        AnimVec3 p0  = pts[i];
        AnimVec3 p1  = pts[i + 1u];
        AnimVec3 p2  = (i + 2u < n) ? pts[i + 2u] : pts[n - 1u];
        AnimVec3 c1  = ANIM_V3(p0.x + (p1.x - pm1.x) / 6.0f, p0.y + (p1.y - pm1.y) / 6.0f, p0.z + (p1.z - pm1.z) / 6.0f);
        AnimVec3 c2  = ANIM_V3(p1.x - (p2.x - p0.x) / 6.0f, p1.y - (p2.y - p0.y) / 6.0f, p1.z - (p2.z - p0.z) / 6.0f);
        anim_vmob_cubic_to(s, id, c1, c2, p1);
    }
}

AnimId anim_plot(AnimScene *s, AnimId axes_id, AnimScalarFn fn, float x_min, float x_max, uint32_t samples, void *user)
{
    AnimId   id  = anim_vmob_create(s);
    uint32_t n   = MU_CLAMP(samples, 4u, 512u);
    AnimVec3 pts[512];
    uint32_t i;

    for (i = 0u; i < n; ++i)
    {
        float x = x_min + ((float)i / (float)(n - 1u)) * (x_max - x_min);
        float y = fn ? fn(x, user) : 0.0f;
        pts[i]  = anim_axes_c2p(s, axes_id, x, y);
    }
    add_catmull_rom_curve(s, id, pts, n);
    anim_set_stroke(s, id, ANIM_YELLOW, 4.0f, false);
    return id;
}

AnimId anim_area_under_curve(AnimScene *s, AnimId axes_id, AnimScalarFn fn, float x_min, float x_max, uint32_t samples, void *user)
{
    AnimId   id = anim_vmob_create(s);
    uint32_t n  = MU_CLAMP(samples, 4u, 512u);
    AnimVec3 pts[512];
    uint32_t i;

    for (i = 0u; i < n; ++i)
    {
        float x = x_min + ((float)i / (float)(n - 1u)) * (x_max - x_min);
        float y = fn ? fn(x, user) : 0.0f;
        pts[i]  = anim_axes_c2p(s, axes_id, x, y);
    }
    add_catmull_rom_curve(s, id, pts, n);
    anim_vmob_line_to(s, id, anim_axes_c2p(s, axes_id, x_max, 0.0f));
    anim_vmob_line_to(s, id, anim_axes_c2p(s, axes_id, x_min, 0.0f));
    anim_vmob_close_subpath(s, id);
    anim_set_fill(s, id, anim_hex_a(0x58C4DD, 0.45f), false);
    anim_set_stroke(s, id, ANIM_BLUE, 0.0f, false);
    return id;
}

AnimId anim_riemann_rectangles(AnimScene *s, AnimId axes_id, AnimScalarFn fn, float x_min, float x_max, float dx, void *user, AnimColor c_left, AnimColor c_right)
{
    AnimId grp = anim_group(s, NULL, 0u);
    float  x;
    float  span = MU_MAX(x_max - x_min, 1e-5f);

    if (dx <= 1e-4f)
        dx = span / 10.0f;
    for (x = x_min; x < x_max - 1e-5f * dx; x += dx)
    {
        float    x1 = MU_MIN(x + dx, x_max);
        float    y  = fn ? fn(x, user) : 0.0f;
        AnimVec3 p0 = anim_axes_c2p(s, axes_id, x, 0.0f);
        AnimVec3 p1 = anim_axes_c2p(s, axes_id, x1, y);
        float    w  = fabsf(p1.x - p0.x);
        float    h  = fabsf(p1.y - p0.y);
        AnimId   rc = anim_rect(s, w, MU_MAX(h, 1e-4f));
        float    u  = (x - x_min) / span;
        AnimColor col = anim_color_lerp_oklab(c_left, c_right, u);

        anim_move_to(s, rc, ANIM_V3(0.5f * (p0.x + p1.x), 0.5f * (p0.y + p1.y), 0.0f));
        anim_set_fill(s, rc, col, false);
        anim_set_stroke(s, rc, ANIM_WHITE, 1.5f, false);
        anim_add_child(s, grp, rc);
    }
    return grp;
}

AnimId anim_vector_field(AnimScene *s, AnimId axes_id, AnimFieldFn fn, float x_step, float y_step, float max_arrow_len, void *user)
{
    const VMobCold *ac = anim__cold(s, axes_id);
    AnimId          grp = anim_group(s, NULL, 0u);
    float           xmin = ac && ac->is_axes ? ac->axes_x_min : -5.0f;
    float           xmax = ac && ac->is_axes ? ac->axes_x_max :  5.0f;
    float           ymin = ac && ac->is_axes ? ac->axes_y_min : -3.0f;
    float           ymax = ac && ac->is_axes ? ac->axes_y_max :  3.0f;
    float           x, y;

    if (x_step <= 1e-4f) x_step = 1.0f;
    if (y_step <= 1e-4f) y_step = 1.0f;
    for (y = ymin; y <= ymax + 1e-4f; y += y_step)
    {
        for (x = xmin; x <= xmax + 1e-4f; x += x_step)
        {
            AnimVec3 v    = fn ? fn(ANIM_V3(x, y, 0.0f), user) : ANIM_ORIGIN;
            float    mag  = sqrtf(v.x * v.x + v.y * v.y);
            float    len  = MU_MIN(mag * 0.35f, max_arrow_len > 0.0f ? max_arrow_len : 0.65f);
            AnimVec3 p0   = anim_axes_c2p(s, axes_id, x, y);
            AnimVec3 dir  = (mag > 1e-5f) ? ANIM_V3(v.x / mag, v.y / mag, 0.0f) : ANIM_RIGHT;
            AnimVec3 p1   = ANIM_V3(p0.x + dir.x * len, p0.y + dir.y * len, 0.0f);
            AnimId   arr  = anim_arrow(s, p0, p1, 0.11f, 0.09f);
            float    u    = MU_CLAMP(mag / 2.5f, 0.0f, 1.0f);
            AnimColor col = anim_color_lerp_oklab(ANIM_BLUE_D, ANIM_GOLD, u);
            anim_set_color(s, arr, col, true);
            anim_set_stroke(s, arr, col, 2.2f, true);
            anim_add_child(s, grp, arr);
        }
    }
    return grp;
}

AnimId anim_decimal_number(AnimScene *s, float value, uint32_t num_decimals, const char *font_path, float height)
{
    char      buf[48];
    AnimId    id;
    VMobCold *c;

    snprintf(buf, sizeof buf, "%.*f", (int)MU_MIN(num_decimals, 6u), (double)value);
    id = anim_text(s, font_path, buf, height);
    c  = anim__cold(s, id);
    if (c)
    {
        c->is_decimal   = true;
        c->dec_decimals = MU_MIN(num_decimals, 6u);
        c->dec_height   = height;
        snprintf(c->dec_font, sizeof c->dec_font, "%s", font_path ? font_path : "");
    }
    return id;
}

void anim_decimal_number_set(AnimScene *s, AnimId num_id, float value)
{
    VMobHot  *h = anim__hot(s, num_id);
    VMobCold *c = h ? &s->cold[num_id.index] : NULL;
    char      buf[48];
    AnimVec3  old_center;
    AnimColor old_fill, old_stroke;
    float     old_sw, old_op, old_glow;
    uint32_t  child;
    AnimId    fresh;

    if (!h || !c || !c->is_decimal)
        return;
    old_center = anim_center(s, num_id);
    old_fill   = h->fill_color;
    old_stroke = h->stroke_color;
    old_sw     = h->stroke_width;
    old_op     = h->opacity;
    old_glow   = h->glow;
    /* Preserve first child's style if group has children */
    if (h->first_child != 0u)
    {
        old_fill   = s->hot[h->first_child].fill_color;
        old_stroke = s->hot[h->first_child].stroke_color;
        old_sw     = s->hot[h->first_child].stroke_width;
    }

    child = h->first_child;
    while (child != 0u)
    {
        uint32_t next = s->hot[child].next_sibling;
        anim_vmob_destroy(s, (AnimId){ child, s->hot[child].gen });
        child = next;
    }

    snprintf(buf, sizeof buf, "%.*f", (int)c->dec_decimals, (double)value);
    fresh = anim_text(s, c->dec_font[0] ? c->dec_font : NULL, buf, c->dec_height);
    if (ANIM_ID_VALID(fresh))
    {
        uint32_t fc = s->hot[fresh.index].first_child;
        while (fc != 0u)
        {
            uint32_t next = s->hot[fc].next_sibling;
            anim_add_child(s, num_id, (AnimId){ fc, s->hot[fc].gen });
            fc = next;
        }
        s->hot[fresh.index].first_child = 0u;
        anim_vmob_destroy(s, fresh);
    }
    anim_set_fill(s, num_id, old_fill, true);
    anim_set_stroke(s, num_id, old_stroke, old_sw, true);
    anim_set_opacity(s, num_id, old_op, true);
    anim_set_glow(s, num_id, old_glow, true);
    anim_move_to(s, num_id, old_center);
}

AnimId anim_matrix(AnimScene *s, const float *values, uint32_t rows, uint32_t cols, uint32_t decimals, const char *font_path, float cell_height)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    AnimId   cells[128];
    uint32_t count = MU_MIN(rows * cols, 128u);
    uint32_t i;
    AnimVec3 mn, mx;
    AnimId   lb, rb;
    float    pad_x = 0.20f, pad_y = 0.14f, lip = 0.14f;

    for (i = 0u; i < count; ++i)
    {
        cells[i] = anim_decimal_number(s, values[i], decimals, font_path, cell_height);
        anim_add_child(s, grp, cells[i]);
    }
    anim_arrange_in_grid(s, cells, count, rows, cols, 0.45f, 0.32f);
    anim_bbox_world(s, grp, &mn, &mx);

    lb = anim_vmob_create(s);
    anim_vmob_begin_subpath(s, lb, ANIM_V3(mn.x - pad_x + lip, mx.y + pad_y, 0.0f));
    anim_vmob_line_to(s, lb, ANIM_V3(mn.x - pad_x, mx.y + pad_y, 0.0f));
    anim_vmob_line_to(s, lb, ANIM_V3(mn.x - pad_x, mn.y - pad_y, 0.0f));
    anim_vmob_line_to(s, lb, ANIM_V3(mn.x - pad_x + lip, mn.y - pad_y, 0.0f));
    anim_set_stroke(s, lb, ANIM_WHITE, 3.0f, false);

    rb = anim_vmob_create(s);
    anim_vmob_begin_subpath(s, rb, ANIM_V3(mx.x + pad_x - lip, mx.y + pad_y, 0.0f));
    anim_vmob_line_to(s, rb, ANIM_V3(mx.x + pad_x, mx.y + pad_y, 0.0f));
    anim_vmob_line_to(s, rb, ANIM_V3(mx.x + pad_x, mn.y - pad_y, 0.0f));
    anim_vmob_line_to(s, rb, ANIM_V3(mx.x + pad_x - lip, mn.y - pad_y, 0.0f));
    anim_set_stroke(s, rb, ANIM_WHITE, 3.0f, false);

    anim_add_child(s, grp, lb);
    anim_add_child(s, grp, rb);
    return grp;
}

/* ----------------------------- 3D Curves, Lit Surfaces & Technical Widgets */

AnimId anim_parametric_curve(AnimScene *s, AnimParamFn fn, float t_min, float t_max, uint32_t samples, void *user)
{
    AnimId    id  = anim_vmob_create(s);
    uint32_t  n   = MU_CLAMP(samples, 4u, 2048u);
    AnimVec3 *pts = (AnimVec3 *)malloc((size_t)n * sizeof *pts);
    uint32_t  i;

    MU_ASSERT(pts != NULL);
    for (i = 0u; i < n; ++i)
    {
        float t = t_min + ((float)i / (float)(n - 1u)) * (t_max - t_min);
        pts[i]  = fn ? fn(t, user) : ANIM_ORIGIN;
    }
    add_catmull_rom_curve(s, id, pts, n);
    free(pts);
    anim_set_stroke(s, id, ANIM_TEAL, 3.5f, false);
    return id;
}

AnimId anim_surface3d(AnimScene *s, AnimSurfaceFn fn, AnimSurfaceColorFn color_fn,
                      float u_min, float u_max, float v_min, float v_max,
                      uint32_t u_steps, uint32_t v_steps, void *user)
{
    AnimId    id   = anim_vmob_create(s);
    VMobHot  *h    = anim__hot(s, id);
    VMobCold *c    = anim__cold(s, id);
    uint32_t  nu   = MU_CLAMP(u_steps, 4u, 256u);
    uint32_t  nv   = MU_CLAMP(v_steps, 4u, 256u);
    uint32_t  vcnt = (nu + 1u) * (nv + 1u);
    uint32_t  icnt = nu * nv * 6u;
    uint32_t  vo   = s->surf_vert_count;
    uint32_t  io   = s->surf_idx_count;
    uint32_t  iu, iv;
    float     du   = (u_max - u_min) / (float)nu;
    float     dv   = (v_max - v_min) / (float)nv;
    float     eps  = 1e-3f;

    if (!h || !c || !fn)
        return ANIM_ID_NIL;
    anim__ensure_surf_vert_cap(s, vcnt);
    anim__ensure_surf_idx_cap(s, icnt);

    for (iv = 0u; iv <= nv; ++iv)
    {
        float vf = (float)iv / (float)nv;
        float v  = v_min + vf * (v_max - v_min);
        for (iu = 0u; iu <= nu; ++iu)
        {
            float       uf = (float)iu / (float)nu;
            float       u  = u_min + uf * (u_max - u_min);
            AnimVec3    p  = fn(u, v, user);
            AnimVec3    pu = fn(u + eps * MU_MAX(fabsf(du), 1e-2f), v, user);
            AnimVec3    pv = fn(u, v + eps * MU_MAX(fabsf(dv), 1e-2f), user);
            AnimVec3    n  = v3_norm(v3_cross(v3_sub(pu, p), v3_sub(pv, p)));
            AnimColor   col;
            SurfaceVert sv;

            if (color_fn)
            {
                col = color_fn(u, v, p, n, user);
            }
            else
            {
                float hcol = MU_CLAMP(0.5f + 0.35f * p.y, 0.0f, 1.0f);
                col        = anim_color_lerp_oklab(ANIM_BLUE_E, ANIM_TEAL, hcol);
            }
            sv.pos   = p;
            sv.norm  = n;
            sv.u     = uf;
            sv.v     = vf;
            sv.color = col;
            s->surf_verts[vo + iv * (nu + 1u) + iu] = sv;
        }
    }

    {
        uint32_t k = io;
        for (iv = 0u; iv < nv; ++iv)
        {
            for (iu = 0u; iu < nu; ++iu)
            {
                uint32_t a = iv * (nu + 1u) + iu;
                uint32_t b = a + 1u;
                uint32_t c_idx = a + (nu + 1u);
                uint32_t d = c_idx + 1u;
                s->surf_indices[k++] = a;
                s->surf_indices[k++] = b;
                s->surf_indices[k++] = d;
                s->surf_indices[k++] = a;
                s->surf_indices[k++] = d;
                s->surf_indices[k++] = c_idx;
            }
        }
    }

    s->surf_vert_count += vcnt;
    s->surf_idx_count  += icnt;

    h->surf_vert_offset = vo;
    h->surf_vert_count  = vcnt;
    h->surf_idx_offset  = io;
    h->surf_idx_count   = icnt;
    h->fill_color       = ANIM_BLUE_C;
    h->stroke_width     = 0.0f;
    c->surf_grid_density = 12.0f;
    c->surf_grid_alpha   = 0.45f;
    c->surf_roughness    = 0.35f;
    return id;
}

static AnimVec3 sphere_surf_fn(float u, float v, void *user)
{
    float r   = *(const float *)user;
    float phi = v * ANIM_PI;
    float th  = u * ANIM_TAU;
    return ANIM_V3(r * sinf(phi) * cosf(th), r * cosf(phi), r * sinf(phi) * sinf(th));
}

static AnimColor sphere_color_fn(float u, float v, AnimVec3 p, AnimVec3 n, void *user)
{
    (void)u; (void)p; (void)n; (void)user;
    return anim_color_lerp_oklab(ANIM_BLUE_E, ANIM_TEAL, v);
}

AnimId anim_sphere(AnimScene *s, float radius, uint32_t u_steps, uint32_t v_steps)
{
    float r = MU_MAX(radius, 1e-3f);
    return anim_surface3d(s, sphere_surf_fn, sphere_color_fn, 0.0f, 1.0f, 0.02f, 0.98f, u_steps, v_steps, &r);
}

typedef struct TorusParams { float r_major, r_minor; } TorusParams;
static AnimVec3 torus_surf_fn(float u, float v, void *user)
{
    const TorusParams *tp = (const TorusParams *)user;
    float th = u * ANIM_TAU;
    float ph = v * ANIM_TAU;
    float rr = tp->r_major + tp->r_minor * cosf(ph);
    return ANIM_V3(rr * cosf(th), tp->r_minor * sinf(ph), rr * sinf(th));
}

static AnimColor torus_color_fn(float u, float v, AnimVec3 p, AnimVec3 n, void *user)
{
    (void)v; (void)p; (void)n; (void)user;
    float t = 0.5f + 0.5f * sinf(u * ANIM_TAU);
    return anim_color_lerp_oklab(ANIM_PURPLE, ANIM_GOLD, t);
}

AnimId anim_torus(AnimScene *s, float r_major, float r_minor, uint32_t u_steps, uint32_t v_steps)
{
    TorusParams tp = { MU_MAX(r_major, 0.05f), MU_MAX(r_minor, 0.02f) };
    return anim_surface3d(s, torus_surf_fn, torus_color_fn, 0.0f, 1.0f, 0.0f, 1.0f, u_steps, v_steps, &tp);
}

AnimId anim_axes3d(AnimScene *s, float x_range, float y_range, float z_range)
{
    AnimId grp = anim_group(s, NULL, 0u);
    AnimId ax  = anim_arrow(s, ANIM_V3(-x_range, 0.0f, 0.0f), ANIM_V3(x_range, 0.0f, 0.0f), 0.16f, 0.12f);
    AnimId ay  = anim_arrow(s, ANIM_V3(0.0f, -y_range, 0.0f), ANIM_V3(0.0f, y_range, 0.0f), 0.16f, 0.12f);
    AnimId az  = anim_arrow(s, ANIM_V3(0.0f, 0.0f, -z_range), ANIM_V3(0.0f, 0.0f, z_range), 0.16f, 0.12f);
    AnimId grid = anim_vmob_create(s);
    float  k;

    anim_set_color(s, ax, ANIM_RED_A, true);
    anim_set_stroke(s, ax, ANIM_RED_A, 2.5f, true);
    anim_set_color(s, ay, ANIM_GREEN_A, true);
    anim_set_stroke(s, ay, ANIM_GREEN_A, 2.5f, true);
    anim_set_color(s, az, ANIM_BLUE_B, true);
    anim_set_stroke(s, az, ANIM_BLUE_B, 2.5f, true);

    for (k = -x_range; k <= x_range + 1e-4f; k += 1.0f)
    {
        anim_vmob_begin_subpath(s, grid, ANIM_V3(k, 0.0f, -z_range));
        anim_vmob_line_to(s, grid, ANIM_V3(k, 0.0f, z_range));
    }
    for (k = -z_range; k <= z_range + 1e-4f; k += 1.0f)
    {
        anim_vmob_begin_subpath(s, grid, ANIM_V3(-x_range, 0.0f, k));
        anim_vmob_line_to(s, grid, ANIM_V3(x_range, 0.0f, k));
    }
    anim_set_stroke(s, grid, anim_hex_a(0x405B78, 0.40f), 1.4f, false);

    anim_add_child(s, grp, grid);
    anim_add_child(s, grp, ax);
    anim_add_child(s, grp, ay);
    anim_add_child(s, grp, az);
    return grp;
}

void anim_surface_set_wireframe(AnimScene *s, AnimId id, float grid_density, float line_alpha, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    s->cold[id.index].surf_grid_density = MU_MAX(grid_density, 0.0f);
    s->cold[id.index].surf_grid_alpha   = MU_CLAMP(line_alpha, 0.0f, 1.0f);
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_surface_set_wireframe(s, (AnimId){ child, s->hot[child].gen }, grid_density, line_alpha, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_surface_set_roughness(AnimScene *s, AnimId id, float roughness, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    s->cold[id.index].surf_roughness = MU_CLAMP(roughness, 0.05f, 0.99f);
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_surface_set_roughness(s, (AnimId){ child, s->hot[child].gen }, roughness, true);
        child = s->hot[child].next_sibling;
    }
}

static void apply_tex_id_rec(AnimScene *s, AnimId id, uint32_t tex_id, bool lit, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    s->cold[id.index].surf_tex_id = tex_id;
    s->cold[id.index].surf_unlit  = !lit;
    if (tex_id != 0u)
    {
        s->cold[id.index].surf_use_fill_color = false;
        h->fill_color = ANIM_WHITE;
    }
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        apply_tex_id_rec(s, (AnimId){ child, s->hot[child].gen }, tex_id, lit, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_surface_set_texture(AnimScene *s, AnimId id, const char *path, bool lit, bool recursive)
{
    uint32_t tex_id;
    if (!s || !path || !path[0])
        return;
    tex_id = anim__load_texture_file(s, path, NULL, NULL);
    if (tex_id != 0u)
        apply_tex_id_rec(s, id, tex_id, lit, recursive);
}

void anim_surface_set_checkerboard(AnimScene *s, AnimId id, AnimColor c1, AnimColor c2, uint32_t u_divs, uint32_t v_divs, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    s->cold[id.index].surf_checker    = true;
    s->cold[id.index].surf_checker_c1 = c1;
    s->cold[id.index].surf_checker_c2 = c2;
    s->cold[id.index].surf_checker_u  = MU_MAX(u_divs, 1u);
    s->cold[id.index].surf_checker_v  = MU_MAX(v_divs, 1u);
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_surface_set_checkerboard(s, (AnimId){ child, s->hot[child].gen }, c1, c2, u_divs, v_divs, true);
        child = s->hot[child].next_sibling;
    }
}

void anim_image_set_lit(AnimScene *s, AnimId id, bool lit, bool recursive)
{
    VMobHot *h = anim__hot(s, id);
    uint32_t child;
    if (!h)
        return;
    s->cold[id.index].surf_unlit = !lit;
    if (!recursive)
        return;
    child = h->first_child;
    while (child != 0u)
    {
        anim_image_set_lit(s, (AnimId){ child, s->hot[child].gen }, lit, true);
        child = s->hot[child].next_sibling;
    }
}

AnimId anim_paragraph(AnimScene *s, const char *font_path, const char *utf8, float line_height, float line_spacing, AnimAlign align)
{
    AnimId      grp = anim_group(s, NULL, 0u);
    const char *p   = utf8 ? utf8 : "";
    float       y   = 0.0f;
    float       step = line_height * (line_spacing > 0.2f ? line_spacing : 1.35f);
    AnimId      lines[128];
    uint32_t    n = 0u, i;
    float       max_w = 0.0f;

    while (*p && n < 128u)
    {
        const char *nl = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        char        buf[1024];
        if (len > 0u)
        {
            size_t cp = MU_MIN(len, sizeof buf - 1u);
            memcpy(buf, p, cp);
            buf[cp] = '\0';
            lines[n] = anim_text(s, font_path, buf, line_height);
            max_w    = MU_MAX(max_w, anim_width(s, lines[n]));
            anim_move_to(s, lines[n], ANIM_V3(0.0f, y, 0.0f));
            anim_add_child(s, grp, lines[n]);
            n++;
        }
        y -= step;
        if (!nl)
            break;
        p = nl + 1;
    }

    if (align != ANIM_ALIGN_CENTER)
    {
        for (i = 0u; i < n; ++i)
        {
            float lw = anim_width(s, lines[i]);
            float dx = (align == ANIM_ALIGN_LEFT) ? (-0.5f * (max_w - lw)) : (0.5f * (max_w - lw));
            anim_shift(s, lines[i], ANIM_V3(dx, 0.0f, 0.0f));
        }
    }
    anim_move_to(s, grp, ANIM_ORIGIN);
    return grp;
}

static bool is_code_kw(const char *w, size_t len)
{
    static const char *const KWS[] = {
        "int", "float", "double", "void", "bool", "char", "uint32_t", "uint64_t",
        "size_t", "struct", "enum", "typedef", "return", "if", "else", "for", "while",
        "switch", "case", "break", "continue", "const", "static", "inline", "sizeof",
        "def", "class", "import", "from", "as", "with", "yield", "lambda", "self",
        "let", "fn", "pub", "mut", "in", "true", "false", "True", "False", "NULL", "None",
        "auto", "vec2", "vec3", "vec4", "mat4", "float2", "float3", "float4", "uniform",
        "scene", "play", "lagged", "sequence", "wait", "show", "hide", "at", "do", "end"
    };
    size_t i;
    for (i = 0u; i < sizeof KWS / sizeof KWS[0]; ++i)
    {
        if (strlen(KWS[i]) == len && memcmp(w, KWS[i], len) == 0)
            return true;
    }
    return false;
}

AnimId anim_code(AnimScene *s, const char *font_path, const char *code_utf8, float font_height, bool line_numbers)
{
    const char *mono = (font_path && font_path[0]) ? font_path : "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
    AnimId      grp  = anim_group(s, NULL, 0u);
    AnimId      body = anim_group(s, NULL, 0u);
    const char *p    = code_utf8 ? code_utf8 : "";
    float       fh   = font_height > 0.05f ? font_height : 0.24f;
    float       lh   = fh * 1.48f;
    float       adv  = fh * 0.56f;
    uint32_t    ln   = 1u;
    float       y    = 0.0f;
    AnimVec3    mn, mx;
    AnimId      card;
    VMobCold   *cold;

    while (*p)
    {
        const char *nl   = strchr(p, '\n');
        size_t      llen = nl ? (size_t)(nl - p) : strlen(p);
        float       x    = 0.0f;
        size_t      pos  = 0u;

        if (line_numbers)
        {
            char   nb[16];
            AnimId lnum;
            snprintf(nb, sizeof nb, "%2u", ln);
            lnum = anim_text(s, mono, nb, fh * 0.85f);
            anim_set_color(s, lnum, ANIM_GREY_C, true);
            anim_move_to(s, lnum, ANIM_V3(-0.65f, y, 0.0f));
            anim_add_child(s, body, lnum);
        }

        while (pos < llen)
        {
            char      tok[256];
            size_t    tlen = 0u;
            AnimColor tcol = ANIM_GREY_A;
            char      ch   = p[pos];

            if (ch == ' ' || ch == '\t')
            {
                x += (ch == '\t' ? 4.0f : 1.0f) * adv;
                pos++;
                continue;
            }
            if ((ch == '/' && pos + 1u < llen && p[pos + 1u] == '/') || ch == '#')
            {
                tlen = MU_MIN(llen - pos, sizeof tok - 1u);
                memcpy(tok, p + pos, tlen);
                tcol = ANIM_GREY_C;
                pos  = llen;
            }
            else if (ch == '"' || ch == '\'')
            {
                char q = ch;
                tlen = 1u;
                while (pos + tlen < llen && tlen < sizeof tok - 2u)
                {
                    char c2 = p[pos + tlen];
                    tlen++;
                    if (c2 == q && p[pos + tlen - 2u] != '\\')
                        break;
                }
                memcpy(tok, p + pos, tlen);
                tcol = ANIM_GREEN_A;
                pos += tlen;
            }
            else if ((ch >= '0' && ch <= '9'))
            {
                while (pos + tlen < llen && tlen < sizeof tok - 1u &&
                       ((p[pos + tlen] >= '0' && p[pos + tlen] <= '9') || p[pos + tlen] == '.' ||
                        p[pos + tlen] == 'f' || p[pos + tlen] == 'x' || p[pos + tlen] == 'u'))
                    tlen++;
                memcpy(tok, p + pos, tlen);
                tcol = ANIM_GOLD;
                pos += tlen;
            }
            else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_')
            {
                while (pos + tlen < llen && tlen < sizeof tok - 1u &&
                       ((p[pos + tlen] >= 'a' && p[pos + tlen] <= 'z') ||
                        (p[pos + tlen] >= 'A' && p[pos + tlen] <= 'Z') ||
                        (p[pos + tlen] >= '0' && p[pos + tlen] <= '9') || p[pos + tlen] == '_'))
                    tlen++;
                memcpy(tok, p + pos, tlen);
                if (is_code_kw(tok, tlen))
                    tcol = ANIM_PINK;
                else if (pos + tlen < llen && p[pos + tlen] == '(')
                    tcol = ANIM_BLUE_B;
                else
                    tcol = ANIM_WHITE;
                pos += tlen;
            }
            else
            {
                tok[0] = ch;
                tlen   = 1u;
                tcol   = ANIM_TEAL;
                pos++;
            }

            tok[tlen] = '\0';
            {
                AnimId tm = anim_text(s, mono, tok, fh);
                float  tw = anim_width(s, tm);
                anim_set_color(s, tm, tcol, true);
                anim_move_to(s, tm, ANIM_V3(x + 0.5f * tw, y, 0.0f));
                anim_add_child(s, body, tm);
                x += (float)tlen * adv;
            }
        }

        ln++;
        y -= lh;
        if (!nl)
            break;
        p = nl + 1;
    }

    anim_bbox_world(s, body, &mn, &mx);
    {
        float cw = MU_MAX((mx.x - mn.x) + 0.8f, 3.0f);
        float ch_box = MU_MAX((mx.y - mn.y) + 0.7f, 1.0f);
        card = anim_rounded_rect(s, cw, ch_box, 0.16f);
        anim_move_to(s, card, ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.0f));
        anim_set_fill(s, card, anim_hex_a(0x141926, 0.92f), false);
        anim_set_stroke(s, card, anim_hex_a(0x58C4DD, 0.65f), 2.0f, false);
        anim_set_z_index(s, card, -0.5f, false);
        anim_add_child(s, grp, card);
        anim_add_child(s, grp, body);
        anim_move_to(s, grp, ANIM_ORIGIN);

        cold = anim__cold(s, grp);
        if (cold)
        {
            cold->is_code     = true;
            cold->code_line_h = lh;
            cold->code_top_y  = 0.5f * (mx.y - mn.y) - 0.35f;
            cold->code_width  = cw - 0.16f;
        }
    }
    return grp;
}

AnimId anim_code_highlight_line(AnimScene *s, AnimId code_id, uint32_t line_1based, AnimColor highlight_color)
{
    const VMobCold *c = anim__cold(s, code_id);
    AnimVec3        mn, mx;
    float           lh = (c && c->is_code) ? c->code_line_h : 0.35f;
    AnimId          bar;
    float           y;

    anim_bbox_world(s, code_id, &mn, &mx);
    y   = mx.y - 0.35f - 0.5f * lh - (float)(line_1based > 0u ? line_1based - 1u : 0u) * lh;
    bar = anim_rounded_rect(s, (mx.x - mn.x) - 0.20f, lh * 0.92f, 0.06f);
    anim_move_to(s, bar, ANIM_V3(0.5f * (mn.x + mx.x), y, 0.0f));
    anim_set_fill(s, bar, highlight_color, false);
    anim_set_stroke(s, bar, highlight_color, 1.5f, false);
    anim_set_z_index(s, bar, -0.2f, false);
    return bar;
}

AnimId anim_table(AnimScene *s, const char *const *cells, uint32_t rows, uint32_t cols, const char *font_path, float cell_height, bool header_row)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    AnimId   cell_mobs[256];
    uint32_t count = MU_MIN(rows * cols, 256u);
    uint32_t r, c;
    float    col_w[32];
    float    row_h = MU_MAX(cell_height, 0.18f) * 2.1f;
    float    total_w = 0.0f, total_h = (float)rows * row_h;
    float    cx, cy;
    VMobCold *tc;

    if (rows == 0u || cols == 0u || cols > 32u)
        return grp;
    for (c = 0u; c < cols; ++c)
        col_w[c] = 1.2f;

    for (r = 0u; r < rows; ++r)
    {
        for (c = 0u; c < cols; ++c)
        {
            uint32_t idx = r * cols + c;
            const char *txt = (cells && idx < count && cells[idx]) ? cells[idx] : "";
            AnimId tm = anim_text(s, font_path, txt, cell_height);
            if (header_row && r == 0u)
                anim_set_color(s, tm, ANIM_YELLOW, true);
            else
                anim_set_color(s, tm, ANIM_WHITE, true);
            cell_mobs[idx] = tm;
            col_w[c] = MU_MAX(col_w[c], anim_width(s, tm) + 0.55f);
        }
    }
    for (c = 0u; c < cols; ++c)
        total_w += col_w[c];

    /* Cell text positions */
    cy = 0.5f * total_h - 0.5f * row_h;
    for (r = 0u; r < rows; ++r)
    {
        cx = -0.5f * total_w;
        for (c = 0u; c < cols; ++c)
        {
            uint32_t idx = r * cols + c;
            anim_move_to(s, cell_mobs[idx], ANIM_V3(cx + 0.5f * col_w[c], cy, 0.0f));
            anim_add_child(s, grp, cell_mobs[idx]);
            cx += col_w[c];
        }
        cy -= row_h;
    }

    /* Table grid lines */
    {
        AnimId grid = anim_vmob_create(s);
        float  x    = -0.5f * total_w;
        float  y    = 0.5f * total_h;
        for (c = 0u; c <= cols; ++c)
        {
            anim_vmob_begin_subpath(s, grid, ANIM_V3(x, -0.5f * total_h, 0.0f));
            anim_vmob_line_to(s, grid, ANIM_V3(x, 0.5f * total_h, 0.0f));
            if (c < cols)
                x += col_w[c];
        }
        for (r = 0u; r <= rows; ++r)
        {
            anim_vmob_begin_subpath(s, grid, ANIM_V3(-0.5f * total_w, y, 0.0f));
            anim_vmob_line_to(s, grid, ANIM_V3(0.5f * total_w, y, 0.0f));
            y -= row_h;
        }
        anim_set_stroke(s, grid, anim_hex_a(0x88A4C4, 0.75f), 2.0f, false);
        anim_add_child(s, grp, grid);
    }

    tc = anim__cold(s, grp);
    if (tc)
    {
        tc->is_table   = true;
        tc->table_rows = rows;
        tc->table_cols = cols;
    }
    return grp;
}

AnimId anim_table_cell(const AnimScene *s, AnimId table_id, uint32_t row, uint32_t col)
{
    const VMobCold *c = anim__hot_const(s, table_id) ? &s->cold[table_id.index] : NULL;
    if (!c || !c->is_table || row >= c->table_rows || col >= c->table_cols)
        return ANIM_ID_NIL;
    return anim_child_at(s, table_id, row * c->table_cols + col);
}

AnimId anim_graph_network(AnimScene *s, uint32_t node_count, const AnimVec3 *positions, const char *const *labels,
                          uint32_t edge_count, const uint32_t *edge_from, const uint32_t *edge_to, bool directed, const char *font_path)
{
    AnimId   grp = anim_group(s, NULL, 0u);
    uint32_t i;
    float    node_r = 0.34f;

    for (i = 0u; i < edge_count; ++i)
    {
        uint32_t u = edge_from ? edge_from[i] : 0u;
        uint32_t v = edge_to ? edge_to[i] : 0u;
        AnimVec3 a, b, d;
        float    len;
        if (u >= node_count || v >= node_count || !positions)
            continue;
        a   = positions[u];
        b   = positions[v];
        d   = v3_sub(b, a);
        len = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len > 2.0f * node_r)
        {
            AnimVec3 dir = ANIM_V3(d.x / len, d.y / len, d.z / len);
            AnimVec3 p0  = ANIM_V3(a.x + dir.x * (node_r + 0.04f), a.y + dir.y * (node_r + 0.04f), a.z + dir.z * (node_r + 0.04f));
            AnimVec3 p1  = ANIM_V3(b.x - dir.x * (node_r + 0.04f), b.y - dir.y * (node_r + 0.04f), b.z - dir.z * (node_r + 0.04f));
            AnimId   e   = directed ? anim_arrow(s, p0, p1, 0.14f, 0.11f) : anim_line(s, p0, p1);
            anim_set_color(s, e, ANIM_GREY_B, true);
            anim_set_stroke(s, e, ANIM_GREY_B, 2.5f, true);
            anim_add_child(s, grp, e);
        }
    }

    for (i = 0u; i < node_count; ++i)
    {
        AnimVec3 p    = positions ? positions[i] : ANIM_ORIGIN;
        AnimId   circ = anim_circle(s, node_r);
        anim_move_to(s, circ, p);
        anim_set_fill(s, circ, anim_hex_a(0x1C3554, 0.92f), false);
        anim_set_stroke(s, circ, ANIM_BLUE_C, 3.0f, false);
        anim_add_child(s, grp, circ);
        if (labels && labels[i] && labels[i][0])
        {
            AnimId lbl = anim_text(s, font_path, labels[i], 0.26f);
            anim_set_color(s, lbl, ANIM_WHITE, true);
            anim_move_to(s, lbl, p);
            anim_add_child(s, grp, lbl);
        }
    }
    return grp;
}

/* ----------------------------------------------------- ValueTracker & DAG */

AnimTrackerId anim_tracker_create(AnimScene *s, float initial_value)
{
    uint32_t i;

    for (i = 1u; i < ANIM_MAX_TRACKERS; ++i)
    {
        if (!s->tracker_alive[i])
        {
            s->tracker_alive[i] = true;
            s->tracker_gens[i]++;
            if (s->tracker_gens[i] == 0u)
                s->tracker_gens[i] = 1u;
            s->tracker_vals[i] = initial_value;
            if (i >= s->tracker_count)
                s->tracker_count = i + 1u;
            return (AnimTrackerId){ i, s->tracker_gens[i] };
        }
    }
    log_error("anim: exceeded ANIM_MAX_TRACKERS");
    return ANIM_TRACKER_NIL;
}

float anim_tracker_get(const AnimScene *s, AnimTrackerId id)
{
    if (!ANIM_TRACKER_VALID(id) || id.index >= ANIM_MAX_TRACKERS)
        return 0.0f;
    if (!s->tracker_alive[id.index] || s->tracker_gens[id.index] != id.gen)
        return 0.0f;
    return s->tracker_vals[id.index];
}

void anim_tracker_set(AnimScene *s, AnimTrackerId id, float value)
{
    if (!ANIM_TRACKER_VALID(id) || id.index >= ANIM_MAX_TRACKERS)
        return;
    if (!s->tracker_alive[id.index] || s->tracker_gens[id.index] != id.gen)
        return;
    s->tracker_vals[id.index] = value;
}

static void rebuild_updater_topo(AnimScene *s)
{
    uint32_t    n = s->updater_count;
    uint32_t    offsets[ANIM_MAX_UPDATERS + 1];
    uint32_t    cols[ANIM_MAX_UPDATERS * ANIM_MAX_DEPS];
    uint32_t    degree[ANIM_MAX_UPDATERS];
    uint32_t    queue[ANIM_MAX_UPDATERS];
    mu_toposort ts;
    uint32_t    edge_count = 0u;
    uint32_t    i, j, k;

    for (i = 0u; i < n; ++i)
    {
        offsets[i] = edge_count;
        if (!s->updaters[i].active)
            continue;
        /* Edge i -> j if updater j depends on updater i's target_idx */
        for (j = 0u; j < n; ++j)
        {
            if (i == j || !s->updaters[j].active)
                continue;
            for (k = 0u; k < s->updaters[j].dep_mob_count; ++k)
            {
                if (s->updaters[j].dep_mobs[k] == s->updaters[i].target_idx && edge_count < ANIM_MAX_UPDATERS * ANIM_MAX_DEPS)
                {
                    cols[edge_count++] = j;
                    break;
                }
            }
        }
    }
    offsets[n] = edge_count;

    mu_toposort_init_static(&ts, degree, queue, n);
    {
        bool has_cycle = false;
        s->updater_sorted_count = mu_toposort_run(&ts, offsets, cols, n, s->updater_order, &has_cycle);
        MU_ASSERT(!has_cycle);
    }
    s->updater_dirty        = false;
}

void anim_add_updater(AnimScene *s, AnimId target, AnimUpdaterFn fn, void *user,
                      const AnimId *dep_mobs, uint32_t dep_mob_count,
                      const AnimTrackerId *dep_trackers, uint32_t dep_tracker_count)
{
    UpdaterEntry *u;
    uint32_t      i;

    if (!ANIM_ID_VALID(target) || !fn || s->updater_count >= ANIM_MAX_UPDATERS)
        return;
    u                    = &s->updaters[s->updater_count++];
    memset(u, 0, sizeof *u);
    u->active            = true;
    u->target_idx        = target.index;
    u->fn                = fn;
    u->user              = user;
    u->order             = s->updater_seq++;
    u->dep_mob_count     = MU_MIN(dep_mob_count, (uint32_t)ANIM_MAX_DEPS);
    u->dep_tracker_count = MU_MIN(dep_tracker_count, (uint32_t)ANIM_MAX_DEPS);
    for (i = 0u; i < u->dep_mob_count; ++i)
        u->dep_mobs[i] = dep_mobs[i].index;
    for (i = 0u; i < u->dep_tracker_count; ++i)
        u->dep_trackers[i] = dep_trackers[i].index;
    s->updater_dirty = true;
}

void anim_clear_updaters(AnimScene *s, AnimId target)
{
    uint32_t i;
    for (i = 0u; i < s->updater_count; ++i)
    {
        if (s->updaters[i].target_idx == target.index)
            s->updaters[i].active = false;
    }
    s->updater_dirty = true;
}

void anim_eval_updaters(AnimScene *s, float dt)
{
    uint32_t i;

    if (!s)
        return;
    s->sim_time += dt;
    if (s->updater_dirty)
        rebuild_updater_topo(s);
    for (i = 0u; i < s->updater_sorted_count; ++i)
    {
        const UpdaterEntry *u = &s->updaters[s->updater_order[i]];
        if (!u->active || !u->fn)
            continue;
        if (!s->hot[u->target_idx].alive)
            continue;
        u->fn(s, (AnimId){ u->target_idx, s->hot[u->target_idx].gen }, dt, u->user);
    }
}

void anim_scene_step_updaters(AnimScene *s, float total_dt, float max_substep)
{
    float    step_lim = max_substep > 1e-4f ? max_substep : (1.0f / 60.0f);
    uint32_t n;
    float    h;
    uint32_t k;

    if (!s)
        return;
    if (total_dt <= 1e-6f)
    {
        anim_eval_updaters(s, 0.0f);
        return;
    }
    n = (uint32_t)MU_CLAMP(ceilf(total_dt / step_lim), 1.0f, 240.0f);
    h = total_dt / (float)n;
    for (k = 0u; k < n; ++k)
        anim_eval_updaters(s, h);
}

/* ----------------------------------------------------- hierarchical morphing */

static void bake_cubic_to_world(const float m[16], AnimCubic c, AnimCubic *out)
{
    float p[3], r[3];

    p[0] = c.p0.x; p[1] = c.p0.y; p[2] = c.p0.z; mu_mat4_transform_point(r, m, p); out->p0 = ANIM_V3(r[0], r[1], r[2]);
    p[0] = c.p1.x; p[1] = c.p1.y; p[2] = c.p1.z; mu_mat4_transform_point(r, m, p); out->p1 = ANIM_V3(r[0], r[1], r[2]);
    p[0] = c.p2.x; p[1] = c.p2.y; p[2] = c.p2.z; mu_mat4_transform_point(r, m, p); out->p2 = ANIM_V3(r[0], r[1], r[2]);
    p[0] = c.p3.x; p[1] = c.p3.y; p[2] = c.p3.z; mu_mat4_transform_point(r, m, p); out->p3 = ANIM_V3(r[0], r[1], r[2]);
}

static AnimVec3 subpath_centroid(const AnimCubic *cubics, uint32_t count)
{
    AnimVec3 sum = ANIM_ORIGIN;
    uint32_t i;

    if (count == 0u)
        return ANIM_ORIGIN;
    for (i = 0u; i < count; ++i)
    {
        sum.x += cubics[i].p0.x;
        sum.y += cubics[i].p0.y;
        sum.z += cubics[i].p0.z;
    }
    return ANIM_V3(sum.x / (float)count, sum.y / (float)count, sum.z / (float)count);
}

static void resample_subpath_cubics(AnimCubic *arr, uint32_t cur_n, uint32_t target_n)
{
    while (cur_n < target_n)
    {
        uint32_t  best_i = 0u;
        float     best_l = -1.0f;
        uint32_t  i;
        AnimCubic left, right;

        for (i = 0u; i < cur_n; ++i)
        {
            float l = cubic_approx_len(arr[i]);
            if (l > best_l)
            {
                best_l = l;
                best_i = i;
            }
        }
        anim__cubic_split(arr[best_i], 0.5f, &left, &right);
        memmove(&arr[best_i + 2u], &arr[best_i + 1u], (size_t)(cur_n - best_i - 1u) * sizeof(AnimCubic));
        arr[best_i]      = left;
        arr[best_i + 1u] = right;
        cur_n++;
    }
}

static void align_closed_rotation(const AnimCubic *ref, AnimCubic *mob, uint32_t n)
{
    AnimCubic  stack_tmp[512];
    AnimCubic *tmp;
    uint32_t   best_shift = 0u;
    float      best_err   = 1e30f;
    uint32_t   shift, i;

    if (n <= 1u)
        return;
    tmp = (n <= 512u) ? stack_tmp : (AnimCubic *)malloc((size_t)n * sizeof *tmp);
    if (!tmp)
        return;
    for (shift = 0u; shift < n; ++shift)
    {
        float err = 0.0f;
        for (i = 0u; i < n; ++i)
        {
            uint32_t j  = (i + shift) % n;
            float    dx = ref[i].p0.x - mob[j].p0.x;
            float    dy = ref[i].p0.y - mob[j].p0.y;
            float    dz = ref[i].p0.z - mob[j].p0.z;
            err += dx * dx + dy * dy + dz * dz;
            if (err >= best_err)
                break;
        }
        if (err < best_err)
        {
            best_err   = err;
            best_shift = shift;
        }
    }
    if (best_shift == 0u)
    {
        if (tmp != stack_tmp)
            free(tmp);
        return;
    }
    for (i = 0u; i < n; ++i)
        tmp[i] = mob[(i + best_shift) % n];
    memcpy(mob, tmp, (size_t)n * sizeof(AnimCubic));
    if (tmp != stack_tmp)
        free(tmp);
}

void anim__morph_vmob(AnimScene *s, AnimId dst_id, const AnimScene *snap_a, AnimId a_id,
                      const AnimScene *snap_b, AnimId b_id, float alpha)
{
    const VMobHot *ha = anim__hot_const(snap_a, a_id);
    const VMobHot *hb = anim__hot_const(snap_b, b_id);
    VMobHot       *hd = anim__hot(s, dst_id);
    uint32_t       sp_a, sp_b, sp_max, si;
    uint32_t       new_sub_off;
    AnimCubic      stack_a[512], stack_b[512];
    AnimCubic     *ca_buf = stack_a, *cb_buf = stack_b;
    uint32_t       buf_cap = 512u;

    float          inv_parent[16];
    bool           has_parent = false;

    if (!ha || !hb || !hd)
        return;
    alpha = MU_CLAMP(alpha, 0.0f, 1.0f);
    anim__sync_world_mats((AnimScene *)snap_a);
    anim__sync_world_mats((AnimScene *)snap_b);
    anim__sync_world_mats(s);
    if (hd->parent != 0u && hd->parent < s->mob_cap && s->hot[hd->parent].alive)
    {
        mu_mat4_inverse(s->hot[hd->parent].world_mat, inv_parent);
        has_parent = true;
    }

    /* Reset local TRS to identity; if hd has a parent, transform morphed world cubics into parent's local space. */
    hd->pos   = ANIM_ORIGIN;
    hd->scale = ANIM_V3(1.0f, 1.0f, 1.0f);
    mu_quat_identity(hd->rot_quat);
    hd->mat_dirty = true;

    hd->fill_color     = anim_color_lerp_oklab(ha->fill_color, hb->fill_color, alpha);
    hd->stroke_color   = anim_color_lerp_oklab(ha->stroke_color, hb->stroke_color, alpha);
    hd->stroke_width   = ha->stroke_width + (hb->stroke_width - ha->stroke_width) * alpha;
    hd->opacity        = ha->opacity + (hb->opacity - ha->opacity) * alpha;
    hd->glow           = ha->glow + (hb->glow - ha->glow) * alpha;
    hd->sheen          = ha->sheen + (hb->sheen - ha->sheen) * alpha;
    hd->sheen_angle    = ha->sheen_angle + (hb->sheen_angle - ha->sheen_angle) * alpha;
    hd->shadow_px      = ha->shadow_px + (hb->shadow_px - ha->shadow_px) * alpha;
    hd->dash_px        = ha->dash_px + (hb->dash_px - ha->dash_px) * alpha;
    hd->gap_px         = ha->gap_px + (hb->gap_px - ha->gap_px) * alpha;
    hd->dash_offset_px = ha->dash_offset_px + (hb->dash_offset_px - ha->dash_offset_px) * alpha;
    hd->z_index        = ha->z_index + (hb->z_index - ha->z_index) * alpha;
    hd->trim_start     = ha->trim_start + (hb->trim_start - ha->trim_start) * alpha;
    hd->trim_end       = ha->trim_end + (hb->trim_end - ha->trim_end) * alpha;
    hd->visible        = ha->visible || hb->visible;

    if (ha->surf_vert_count > 0u && ha->surf_idx_count >= 3u &&
        hb->surf_vert_count > 0u && hb->surf_idx_count >= 3u)
    {
        bool     exact = (ha->surf_vert_count == hb->surf_vert_count && ha->surf_idx_count == hb->surf_idx_count);
        bool     use_b = (hb->surf_vert_count >= ha->surf_vert_count);
        uint32_t vcnt  = use_b ? hb->surf_vert_count : ha->surf_vert_count;
        uint32_t icnt  = use_b ? hb->surf_idx_count  : ha->surf_idx_count;
        uint32_t ocnt  = use_b ? ha->surf_vert_count : hb->surf_vert_count;
        uint32_t vo    = s->surf_vert_count;
        uint32_t io    = s->surf_idx_count;
        float    inv_a[16], inv_b[16];
        bool     uv_a = false, uv_b = false, use_uv;
        uint32_t vi, k;

        anim__ensure_surf_vert_cap(s, vcnt);
        anim__ensure_surf_idx_cap(s, icnt);
        hd = &s->hot[dst_id.index];
        mu_mat4_inverse(ha->world_mat, inv_a);
        mu_mat4_inverse(hb->world_mat, inv_b);

        if (!exact)
        {
            float ua0 = snap_a->surf_verts[ha->surf_vert_offset].u;
            float va0 = snap_a->surf_verts[ha->surf_vert_offset].v;
            float ub0 = snap_b->surf_verts[hb->surf_vert_offset].u;
            float vb0 = snap_b->surf_verts[hb->surf_vert_offset].v;
            for (k = 1u; k < ha->surf_vert_count; ++k)
            {
                const SurfaceVert *sv = &snap_a->surf_verts[ha->surf_vert_offset + k];
                if (fabsf(sv->u - ua0) + fabsf(sv->v - va0) > 0.05f) { uv_a = true; break; }
            }
            for (k = 1u; k < hb->surf_vert_count; ++k)
            {
                const SurfaceVert *sv = &snap_b->surf_verts[hb->surf_vert_offset + k];
                if (fabsf(sv->u - ub0) + fabsf(sv->v - vb0) > 0.05f) { uv_b = true; break; }
            }
        }
        use_uv = uv_a && uv_b;

        for (vi = 0u; vi < vcnt; ++vi)
        {
            uint32_t           ia, ib;
            const SurfaceVert *va, *vb;
            float              pa_w[3], pb_w[3], na_w[3], nb_w[3];
            AnimVec3           pw, nw;
            SurfaceVert        mv;

            if (exact)
            {
                ia = vi;
                ib = vi;
            }
            else if (use_b)
            {
                const SurfaceVert *ref_b = &snap_b->surf_verts[hb->surf_vert_offset + vi];
                uint32_t best = 0u;
                float    best_d2 = 1e30f;
                ib = vi;
                for (k = 0u; k < ocnt; ++k)
                {
                    const SurfaceVert *cand = &snap_a->surf_verts[ha->surf_vert_offset + k];
                    float d2 = use_uv
                        ? ((cand->u - ref_b->u) * (cand->u - ref_b->u) + (cand->v - ref_b->v) * (cand->v - ref_b->v))
                        : ((cand->pos.x - ref_b->pos.x) * (cand->pos.x - ref_b->pos.x) +
                           (cand->pos.y - ref_b->pos.y) * (cand->pos.y - ref_b->pos.y) +
                           (cand->pos.z - ref_b->pos.z) * (cand->pos.z - ref_b->pos.z));
                    if (d2 < best_d2) { best_d2 = d2; best = k; }
                }
                ia = best;
            }
            else
            {
                const SurfaceVert *ref_a = &snap_a->surf_verts[ha->surf_vert_offset + vi];
                uint32_t best = 0u;
                float    best_d2 = 1e30f;
                ia = vi;
                for (k = 0u; k < ocnt; ++k)
                {
                    const SurfaceVert *cand = &snap_b->surf_verts[hb->surf_vert_offset + k];
                    float d2 = use_uv
                        ? ((cand->u - ref_a->u) * (cand->u - ref_a->u) + (cand->v - ref_a->v) * (cand->v - ref_a->v))
                        : ((cand->pos.x - ref_a->pos.x) * (cand->pos.x - ref_a->pos.x) +
                           (cand->pos.y - ref_a->pos.y) * (cand->pos.y - ref_a->pos.y) +
                           (cand->pos.z - ref_a->pos.z) * (cand->pos.z - ref_a->pos.z));
                    if (d2 < best_d2) { best_d2 = d2; best = k; }
                }
                ib = best;
            }

            va = &snap_a->surf_verts[ha->surf_vert_offset + ia];
            vb = &snap_b->surf_verts[hb->surf_vert_offset + ib];
            mu_mat4_transform_point(pa_w, ha->world_mat, (float[3]){ va->pos.x, va->pos.y, va->pos.z });
            mu_mat4_transform_point(pb_w, hb->world_mat, (float[3]){ vb->pos.x, vb->pos.y, vb->pos.z });
            na_w[0] = inv_a[0] * va->norm.x + inv_a[1] * va->norm.y + inv_a[2]  * va->norm.z;
            na_w[1] = inv_a[4] * va->norm.x + inv_a[5] * va->norm.y + inv_a[6]  * va->norm.z;
            na_w[2] = inv_a[8] * va->norm.x + inv_a[9] * va->norm.y + inv_a[10] * va->norm.z;
            nb_w[0] = inv_b[0] * vb->norm.x + inv_b[1] * vb->norm.y + inv_b[2]  * vb->norm.z;
            nb_w[1] = inv_b[4] * vb->norm.x + inv_b[5] * vb->norm.y + inv_b[6]  * vb->norm.z;
            nb_w[2] = inv_b[8] * vb->norm.x + inv_b[9] * vb->norm.y + inv_b[10] * vb->norm.z;

            pw = v3_lerp(ANIM_V3(pa_w[0], pa_w[1], pa_w[2]), ANIM_V3(pb_w[0], pb_w[1], pb_w[2]), alpha);
            nw = v3_norm(v3_lerp(v3_norm(ANIM_V3(na_w[0], na_w[1], na_w[2])),
                                 v3_norm(ANIM_V3(nb_w[0], nb_w[1], nb_w[2])), alpha));
            if (has_parent)
            {
                float pl[3];
                mu_mat4_transform_point(pl, inv_parent, (float[3]){ pw.x, pw.y, pw.z });
                pw = ANIM_V3(pl[0], pl[1], pl[2]);
            }
            mv.pos   = pw;
            mv.norm  = nw;
            mv.u     = va->u + (vb->u - va->u) * alpha;
            mv.v     = va->v + (vb->v - va->v) * alpha;
            mv.color = anim_color_lerp_oklab(va->color, vb->color, alpha);
            s->surf_verts[vo + vi] = mv;
        }
        {
            const uint32_t *src_idx = use_b
                ? &snap_b->surf_indices[hb->surf_idx_offset]
                : &snap_a->surf_indices[ha->surf_idx_offset];
            memcpy(&s->surf_indices[io], src_idx, (size_t)icnt * sizeof(uint32_t));
        }
        s->surf_vert_count  += vcnt;
        s->surf_idx_count   += icnt;
        hd->surf_vert_offset = vo;
        hd->surf_vert_count  = vcnt;
        hd->surf_idx_offset  = io;
        hd->surf_idx_count   = icnt;
        {
            const VMobCold *ca = &snap_a->cold[a_id.index];
            const VMobCold *cb = &snap_b->cold[b_id.index];
            VMobCold       *cd = &s->cold[dst_id.index];
            cd->surf_grid_density = ca->surf_grid_density + (cb->surf_grid_density - ca->surf_grid_density) * alpha;
            cd->surf_grid_alpha   = ca->surf_grid_alpha   + (cb->surf_grid_alpha   - ca->surf_grid_alpha)   * alpha;
            cd->surf_roughness    = ca->surf_roughness    + (cb->surf_roughness    - ca->surf_roughness)    * alpha;
            cd->surf_use_fill_color = (alpha < 0.5f) ? ca->surf_use_fill_color : cb->surf_use_fill_color;
            cd->surf_tex_id         = (alpha < 0.5f) ? ca->surf_tex_id         : cb->surf_tex_id;
            cd->surf_unlit          = (alpha < 0.5f) ? ca->surf_unlit          : cb->surf_unlit;
            cd->surf_tex_nearest    = (alpha < 0.5f) ? ca->surf_tex_nearest    : cb->surf_tex_nearest;
        }
    }

    sp_a   = ha->subpath_count;
    sp_b   = hb->subpath_count;
    sp_max = MU_MAX(sp_a, sp_b);
    if (sp_max == 0u)
    {
        hd->subpath_count = 0u;
        return;
    }

    new_sub_off = s->subpath_count;
    anim__ensure_subpath_cap(s, sp_max);
    hd = &s->hot[dst_id.index];
    hd->subpath_offset = new_sub_off;
    hd->subpath_count  = sp_max;
    s->subpath_count  += sp_max;

    for (si = 0u; si < sp_max; ++si)
    {
        uint32_t na = 0u, nb = 0u, nm, i;
        bool     closed_a = false, closed_b = false;
        uint32_t c_off;

        if (si < sp_a)
            na = snap_a->subpaths[ha->subpath_offset + si].cubic_count;
        if (si < sp_b)
            nb = snap_b->subpaths[hb->subpath_offset + si].cubic_count;
        nm = MU_MAX(na, nb);
        if (nm > buf_cap)
        {
            if (ca_buf != stack_a) free(ca_buf);
            if (cb_buf != stack_b) free(cb_buf);
            buf_cap = nm;
            ca_buf = (AnimCubic *)malloc((size_t)buf_cap * sizeof *ca_buf);
            cb_buf = (AnimCubic *)malloc((size_t)buf_cap * sizeof *cb_buf);
            MU_ASSERT(ca_buf && cb_buf);
        }

        if (si < sp_a)
        {
            SubpathMeta sma = snap_a->subpaths[ha->subpath_offset + si];
            closed_a = sma.closed;
            for (i = 0u; i < na; ++i)
                bake_cubic_to_world(ha->world_mat, snap_a->cubics[sma.cubic_offset + i], &ca_buf[i]);
        }
        if (si < sp_b)
        {
            SubpathMeta smb = snap_b->subpaths[hb->subpath_offset + si];
            closed_b = smb.closed;
            for (i = 0u; i < nb; ++i)
                bake_cubic_to_world(hb->world_mat, snap_b->cubics[smb.cubic_offset + i], &cb_buf[i]);
        }
        if (na == 0u && nb == 0u)
        {
            s->subpaths[new_sub_off + si] = (SubpathMeta){ 0u, 0u, false };
            continue;
        }
        if (na == 0u)
        {
            AnimVec3 cen = subpath_centroid(cb_buf, nb);
            na = 1u;
            ca_buf[0] = (AnimCubic){ cen, cen, cen, cen };
            closed_a  = closed_b;
        }
        if (nb == 0u)
        {
            AnimVec3 cen = subpath_centroid(ca_buf, na);
            nb = 1u;
            cb_buf[0] = (AnimCubic){ cen, cen, cen, cen };
            closed_b  = closed_a;
        }

        resample_subpath_cubics(ca_buf, na, nm);
        resample_subpath_cubics(cb_buf, nb, nm);
        if (closed_a && closed_b)
            align_closed_rotation(ca_buf, cb_buf, nm);

        c_off = s->cubic_count;
        anim__ensure_cubic_cap(s, nm);
        for (i = 0u; i < nm; ++i)
        {
            AnimCubic mc = {
                .p0 = v3_lerp(ca_buf[i].p0, cb_buf[i].p0, alpha),
                .p1 = v3_lerp(ca_buf[i].p1, cb_buf[i].p1, alpha),
                .p2 = v3_lerp(ca_buf[i].p2, cb_buf[i].p2, alpha),
                .p3 = v3_lerp(ca_buf[i].p3, cb_buf[i].p3, alpha),
            };
            if (has_parent)
            {
                bake_cubic_to_world(inv_parent, mc, &s->cubics[c_off + i]);
            }
            else
            {
                s->cubics[c_off + i] = mc;
            }
        }
        s->cubic_count               += nm;
        s->subpaths[new_sub_off + si] = (SubpathMeta){ c_off, nm, closed_a || closed_b };
    }
    if (ca_buf != stack_a) free(ca_buf);
    if (cb_buf != stack_b) free(cb_buf);
}

/* --------------------------------------------------------------- snapshots */

AnimScene *anim__snapshot_clone(const AnimScene *src)
{
    AnimScene *dst = (AnimScene *)calloc(1, sizeof *dst);

    MU_ASSERT(dst != NULL);
    dst->mob_cap = src->mob_cap;
    dst->hot     = (VMobHot *)malloc((size_t)src->mob_cap * sizeof *dst->hot);
    dst->cold    = (VMobCold *)malloc((size_t)src->mob_cap * sizeof *dst->cold);
    memcpy(dst->hot, src->hot, (size_t)src->mob_cap * sizeof *dst->hot);
    memcpy(dst->cold, src->cold, (size_t)src->mob_cap * sizeof *dst->cold);

    dst->cubic_count = src->cubic_count;
    dst->cubic_cap   = MU_MAX(src->cubic_count, 1u);
    dst->cubics      = (AnimCubic *)malloc((size_t)dst->cubic_cap * sizeof *dst->cubics);
    if (src->cubic_count > 0u)
        memcpy(dst->cubics, src->cubics, (size_t)src->cubic_count * sizeof *dst->cubics);

    dst->subpath_count = src->subpath_count;
    dst->subpath_cap   = MU_MAX(src->subpath_count, 1u);
    dst->subpaths      = (SubpathMeta *)malloc((size_t)dst->subpath_cap * sizeof *dst->subpaths);
    if (src->subpath_count > 0u)
        memcpy(dst->subpaths, src->subpaths, (size_t)src->subpath_count * sizeof *dst->subpaths);

    dst->surf_vert_count = src->surf_vert_count;
    dst->surf_vert_cap   = MU_MAX(src->surf_vert_count, 1u);
    dst->surf_verts      = (SurfaceVert *)malloc((size_t)dst->surf_vert_cap * sizeof *dst->surf_verts);
    if (src->surf_vert_count > 0u)
        memcpy(dst->surf_verts, src->surf_verts, (size_t)src->surf_vert_count * sizeof *dst->surf_verts);

    dst->surf_idx_count = src->surf_idx_count;
    dst->surf_idx_cap   = MU_MAX(src->surf_idx_count, 1u);
    dst->surf_indices   = (uint32_t *)malloc((size_t)dst->surf_idx_cap * sizeof *dst->surf_indices);
    if (src->surf_idx_count > 0u)
        memcpy(dst->surf_indices, src->surf_indices, (size_t)src->surf_idx_count * sizeof *dst->surf_indices);

    dst->mob_pool.count    = src->mob_pool.count;
    dst->mob_pool.capacity = MU_MAX(src->mob_pool.count, 1u);
    dst->mob_pool.max_id   = src->mob_pool.max_id;
    dst->mob_pool.used_ids = src->mob_pool.used_ids;
    dst->mob_pool.ranges   = (mu_id_pool_range *)malloc((size_t)dst->mob_pool.capacity * sizeof *dst->mob_pool.ranges);
    if (src->mob_pool.count > 0u)
        memcpy(dst->mob_pool.ranges, src->mob_pool.ranges, (size_t)src->mob_pool.count * sizeof *dst->mob_pool.ranges);

    memcpy(dst->tracker_vals, src->tracker_vals, sizeof dst->tracker_vals);
    memcpy(dst->tracker_gens, src->tracker_gens, sizeof dst->tracker_gens);
    memcpy(dst->tracker_alive, src->tracker_alive, sizeof dst->tracker_alive);
    dst->tracker_count = src->tracker_count;

    memcpy(dst->updaters, src->updaters, sizeof dst->updaters);
    dst->updater_count        = src->updater_count;
    memcpy(dst->updater_order, src->updater_order, sizeof dst->updater_order);
    dst->updater_sorted_count = src->updater_sorted_count;
    dst->updater_dirty        = src->updater_dirty;
    dst->updater_seq          = src->updater_seq;

    dst->camera               = src->camera;
    dst->ambient_cam_active   = src->ambient_cam_active;
    dst->ambient_cam_illusion = src->ambient_cam_illusion;
    dst->ambient_d_az         = src->ambient_d_az;
    dst->ambient_d_el         = src->ambient_d_el;
    dst->ambient_wobble       = src->ambient_wobble;
    dst->ambient_phase        = src->ambient_phase;
    dst->bg_top               = src->bg_top;
    dst->bg_bot               = src->bg_bot;
    dst->has_traced_paths   = src->has_traced_paths;
    dst->zoom_inset_enable  = src->zoom_inset_enable;
    dst->zoom_inset_cam     = src->zoom_inset_cam;
    memcpy(dst->zoom_inset_rect, src->zoom_inset_rect, sizeof dst->zoom_inset_rect);
    dst->zoom_inset_border  = src->zoom_inset_border;
    memcpy(dst->viewports, src->viewports, sizeof dst->viewports);
    dst->viewport_count     = src->viewport_count;
    dst->owns_textures      = false;
    dst->textures           = src->textures;
    dst->texture_count      = src->texture_count;
    dst->texture_cap        = src->texture_cap;
    memcpy(dst->lights, src->lights, sizeof dst->lights);
    dst->light_count        = src->light_count;
    dst->ambient_color      = src->ambient_color;
    dst->ambient_intensity  = src->ambient_intensity;
    dst->fog                = src->fog;
    dst->saved_camera       = src->saved_camera;
    dst->has_saved_camera   = src->has_saved_camera;
    dst->sim_time           = src->sim_time;
    snprintf(dst->cache_dir, sizeof dst->cache_dir, "%s", src->cache_dir);
    return dst;
}

void anim__snapshot_restore(AnimScene *dst, const AnimScene *snap)
{
    MU_ASSERT(dst->mob_cap == snap->mob_cap);
    memcpy(dst->hot, snap->hot, (size_t)snap->mob_cap * sizeof *dst->hot);
    memcpy(dst->cold, snap->cold, (size_t)snap->mob_cap * sizeof *dst->cold);

    if (snap->mob_pool.count > dst->mob_pool.capacity)
    {
        dst->mob_pool.ranges   = (mu_id_pool_range *)realloc(dst->mob_pool.ranges,
                                                             (size_t)snap->mob_pool.count * sizeof *dst->mob_pool.ranges);
        dst->mob_pool.capacity = snap->mob_pool.count;
    }
    dst->mob_pool.count    = snap->mob_pool.count;
    dst->mob_pool.max_id   = snap->mob_pool.max_id;
    dst->mob_pool.used_ids = snap->mob_pool.used_ids;
    if (snap->mob_pool.count > 0u)
        memcpy(dst->mob_pool.ranges, snap->mob_pool.ranges, (size_t)snap->mob_pool.count * sizeof *dst->mob_pool.ranges);

    if (snap->cubic_count > dst->cubic_cap)
    {
        dst->cubics    = (AnimCubic *)realloc(dst->cubics, (size_t)snap->cubic_cap * sizeof *dst->cubics);
        dst->cubic_cap = snap->cubic_cap;
    }
    dst->cubic_count = snap->cubic_count;
    if (snap->cubic_count > 0u)
        memcpy(dst->cubics, snap->cubics, (size_t)snap->cubic_count * sizeof *dst->cubics);

    if (snap->subpath_count > dst->subpath_cap)
    {
        dst->subpaths    = (SubpathMeta *)realloc(dst->subpaths, (size_t)snap->subpath_cap * sizeof *dst->subpaths);
        dst->subpath_cap = snap->subpath_cap;
    }
    dst->subpath_count = snap->subpath_count;
    if (snap->subpath_count > 0u)
        memcpy(dst->subpaths, snap->subpaths, (size_t)snap->subpath_count * sizeof *dst->subpaths);

    if (snap->surf_vert_count > dst->surf_vert_cap)
    {
        dst->surf_verts    = (SurfaceVert *)realloc(dst->surf_verts, (size_t)snap->surf_vert_cap * sizeof *dst->surf_verts);
        dst->surf_vert_cap = snap->surf_vert_cap;
    }
    dst->surf_vert_count = snap->surf_vert_count;
    if (snap->surf_vert_count > 0u)
        memcpy(dst->surf_verts, snap->surf_verts, (size_t)snap->surf_vert_count * sizeof *dst->surf_verts);

    if (snap->surf_idx_count > dst->surf_idx_cap)
    {
        dst->surf_indices = (uint32_t *)realloc(dst->surf_indices, (size_t)snap->surf_idx_cap * sizeof *dst->surf_indices);
        dst->surf_idx_cap = snap->surf_idx_cap;
    }
    dst->surf_idx_count = snap->surf_idx_count;
    if (snap->surf_idx_count > 0u)
        memcpy(dst->surf_indices, snap->surf_indices, (size_t)snap->surf_idx_count * sizeof *dst->surf_indices);

    memcpy(dst->tracker_vals, snap->tracker_vals, sizeof dst->tracker_vals);
    memcpy(dst->tracker_gens, snap->tracker_gens, sizeof dst->tracker_gens);
    memcpy(dst->tracker_alive, snap->tracker_alive, sizeof dst->tracker_alive);
    dst->tracker_count = snap->tracker_count;

    memcpy(dst->updaters, snap->updaters, sizeof dst->updaters);
    dst->updater_count        = snap->updater_count;
    memcpy(dst->updater_order, snap->updater_order, sizeof dst->updater_order);
    dst->updater_sorted_count = snap->updater_sorted_count;
    dst->updater_dirty        = snap->updater_dirty;
    dst->updater_seq          = snap->updater_seq;

    dst->camera               = snap->camera;
    dst->ambient_cam_active   = snap->ambient_cam_active;
    dst->ambient_cam_illusion = snap->ambient_cam_illusion;
    dst->ambient_d_az         = snap->ambient_d_az;
    dst->ambient_d_el         = snap->ambient_d_el;
    dst->ambient_wobble       = snap->ambient_wobble;
    dst->ambient_phase        = snap->ambient_phase;
    dst->bg_top               = snap->bg_top;
    dst->bg_bot               = snap->bg_bot;
    dst->has_traced_paths   = snap->has_traced_paths;
    dst->zoom_inset_enable  = snap->zoom_inset_enable;
    dst->zoom_inset_cam     = snap->zoom_inset_cam;
    memcpy(dst->zoom_inset_rect, snap->zoom_inset_rect, sizeof dst->zoom_inset_rect);
    dst->zoom_inset_border  = snap->zoom_inset_border;
    memcpy(dst->viewports, snap->viewports, sizeof dst->viewports);
    dst->viewport_count     = snap->viewport_count;
    memcpy(dst->lights, snap->lights, sizeof dst->lights);
    dst->light_count        = snap->light_count;
    dst->ambient_color      = snap->ambient_color;
    dst->ambient_intensity  = snap->ambient_intensity;
    dst->fog                = snap->fog;
    dst->saved_camera       = snap->saved_camera;
    dst->has_saved_camera   = snap->has_saved_camera;
    dst->sim_time           = snap->sim_time;
}

void anim__snapshot_free(AnimScene *snap)
{
    if (!snap)
        return;
    free(snap->hot);
    free(snap->cold);
    free(snap->cubics);
    free(snap->subpaths);
    free(snap->surf_verts);
    free(snap->surf_indices);
    free(snap->mob_pool.ranges);
    free(snap);
}

/* ----------------------------------------------------- AnimSpec constructors */

static AnimSpec make_spec(AnimKind k, AnimId target)
{
    return (AnimSpec){
        .kind      = k,
        .target    = target,
        .lag_ratio = -1.0f,
    };
}

AnimSpec anim_anim_create(AnimId target)                { return make_spec(ANIM_KIND_CREATE, target); }
AnimSpec anim_anim_uncreate(AnimId target)              { return make_spec(ANIM_KIND_UNCREATE, target); }
AnimSpec anim_anim_write(AnimId target)                 { return make_spec(ANIM_KIND_WRITE, target); }
AnimSpec anim_anim_draw_border_then_fill(AnimId target) { return make_spec(ANIM_KIND_DRAW_BORDER_THEN_FILL, target); }
AnimSpec anim_anim_grow_from_center(AnimId target)      { return make_spec(ANIM_KIND_GROW_FROM_CENTER, target); }
AnimSpec anim_anim_grow_arrow(AnimId target)            { return make_spec(ANIM_KIND_GROW_ARROW, target); }
AnimSpec anim_anim_transform(AnimId src, AnimId dst)    { AnimSpec sp = make_spec(ANIM_KIND_TRANSFORM, src); sp.other = dst; return sp; }
AnimSpec anim_anim_replacement_transform(AnimId src, AnimId dst) { AnimSpec sp = make_spec(ANIM_KIND_REPLACEMENT_TRANSFORM, src); sp.other = dst; return sp; }
AnimSpec anim_anim_transform_matching_tex(AnimId src, AnimId dst) { AnimSpec sp = make_spec(ANIM_KIND_TRANSFORM_MATCHING_TEX, src); sp.other = dst; return sp; }
AnimSpec anim_anim_transform_matching_shapes(AnimId src, AnimId dst) { AnimSpec sp = make_spec(ANIM_KIND_TRANSFORM_MATCHING_SHAPES, src); sp.other = dst; return sp; }

AnimSpec anim_anim_fade_in(AnimId target, AnimVec3 shift, float scale)
{
    AnimSpec sp = make_spec(ANIM_KIND_FADE_IN, target);
    sp.vec    = shift;
    sp.scalar = (scale > 0.0f) ? scale : 1.0f;
    return sp;
}

AnimSpec anim_anim_fade_out(AnimId target, AnimVec3 shift, float scale)
{
    AnimSpec sp = make_spec(ANIM_KIND_FADE_OUT, target);
    sp.vec    = shift;
    sp.scalar = (scale > 0.0f) ? scale : 1.0f;
    return sp;
}

AnimSpec anim_anim_grow_from_point(AnimId target, AnimVec3 point)
{
    AnimSpec sp = make_spec(ANIM_KIND_GROW_FROM_POINT, target);
    sp.vec = point;
    return sp;
}

AnimSpec anim_anim_indicate(AnimId target, float scale_factor, AnimColor color)
{
    AnimSpec sp = make_spec(ANIM_KIND_INDICATE, target);
    sp.scalar = scale_factor > 0.0f ? scale_factor : 1.2f;
    sp.color  = color;
    return sp;
}

AnimSpec anim_anim_circumscribe(AnimId target, AnimColor color, float buff)
{
    AnimSpec sp = make_spec(ANIM_KIND_CIRCUMSCRIBE, target);
    sp.color  = color;
    sp.scalar = buff >= 0.0f ? buff : 0.14f;
    return sp;
}

AnimSpec anim_anim_show_passing_flash(AnimId target, float flash_width)
{
    AnimSpec sp = make_spec(ANIM_KIND_SHOW_PASSING_FLASH, target);
    sp.scalar = flash_width > 0.0f ? flash_width : 0.22f;
    return sp;
}

AnimSpec anim_anim_wiggle(AnimId target, float scale_factor, float angle_rad)
{
    AnimSpec sp = make_spec(ANIM_KIND_WIGGLE, target);
    sp.scalar = scale_factor > 0.0f ? scale_factor : 1.1f;
    sp.vec.z  = angle_rad != 0.0f ? angle_rad : 0.06f;
    return sp;
}

AnimSpec anim_anim_move_to(AnimId target, AnimVec3 pos)
{
    AnimSpec sp = make_spec(ANIM_KIND_MOVE_TO, target);
    sp.vec = pos;
    return sp;
}

AnimSpec anim_anim_move_arc(AnimId target, AnimVec3 dst_pos, float arc_height)
{
    AnimSpec sp = make_spec(ANIM_KIND_MOVE_ARC, target);
    sp.vec    = dst_pos;
    sp.scalar = arc_height;
    return sp;
}

AnimSpec anim_anim_shift(AnimId target, AnimVec3 delta)
{
    AnimSpec sp = make_spec(ANIM_KIND_SHIFT, target);
    sp.vec = delta;
    return sp;
}

AnimSpec anim_anim_scale(AnimId target, float factor)
{
    AnimSpec sp = make_spec(ANIM_KIND_SCALE, target);
    sp.scalar = factor;
    return sp;
}

AnimSpec anim_anim_rotate(AnimId target, float angle_rad, AnimVec3 axis)
{
    AnimSpec sp = make_spec(ANIM_KIND_ROTATE, target);
    sp.scalar = angle_rad;
    sp.vec    = axis;
    return sp;
}

AnimSpec anim_anim_set_color(AnimId target, AnimColor color)
{
    AnimSpec sp = make_spec(ANIM_KIND_SET_COLOR, target);
    sp.color = color;
    return sp;
}

AnimSpec anim_anim_set_opacity(AnimId target, float opacity)
{
    AnimSpec sp = make_spec(ANIM_KIND_SET_OPACITY, target);
    sp.scalar = opacity;
    return sp;
}

AnimSpec anim_anim_tracker_to(AnimTrackerId tracker, float target_value)
{
    AnimSpec sp = make_spec(ANIM_KIND_TRACKER_TO, ANIM_ID_NIL);
    sp.tracker = tracker;
    sp.scalar  = target_value;
    return sp;
}

AnimSpec anim_anim_camera_to(AnimCamera cam)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_TO, ANIM_ID_NIL);
    sp.camera = cam;
    return sp;
}

AnimSpec anim_anim_camera_frame(const AnimScene *scene, AnimId target, float padding, float aspect)
{
    return anim_anim_camera_to(anim_camera_frame_mob(scene, target, padding, aspect));
}

AnimSpec anim_anim_follow_path(AnimId target, AnimId path_mob, bool rotate_along_tangent)
{
    AnimSpec sp = make_spec(ANIM_KIND_FOLLOW_PATH, target);
    sp.other = path_mob;
    sp.flag  = rotate_along_tangent;
    return sp;
}

AnimSpec anim_anim_dash_flow(AnimId target, float delta_offset_px)
{
    AnimSpec sp = make_spec(ANIM_KIND_DASH_FLOW, target);
    sp.scalar = delta_offset_px;
    return sp;
}

AnimSpec anim_anim_camera_orbit(float d_azimuth_rad, float d_elevation_rad, float zoom_mul)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_ORBIT, ANIM_ID_NIL);
    sp.vec = ANIM_V3(d_azimuth_rad, d_elevation_rad, zoom_mul > 0.0f ? zoom_mul : 1.0f);
    return sp;
}

AnimSpec anim_anim_unwrite(AnimId target) { return make_spec(ANIM_KIND_UNWRITE, target); }

AnimSpec anim_anim_grow_from_edge(AnimId target, AnimVec3 edge_dir)
{
    AnimSpec sp = make_spec(ANIM_KIND_GROW_FROM_EDGE, target);
    sp.vec = edge_dir;
    return sp;
}

AnimSpec anim_anim_spin_in_from_nothing(AnimId target, float angle_rad)
{
    AnimSpec sp = make_spec(ANIM_KIND_SPIN_IN, target);
    sp.scalar = angle_rad != 0.0f ? angle_rad : ANIM_PI * 0.5f;
    return sp;
}

AnimSpec anim_anim_shrink_to_center(AnimId target)
{
    return make_spec(ANIM_KIND_SHRINK_TO_CENTER, target);
}

AnimSpec anim_anim_transform_from_copy(AnimId src, AnimId dst)
{
    AnimSpec sp = make_spec(ANIM_KIND_TRANSFORM_FROM_COPY, src);
    sp.other = dst;
    return sp;
}

AnimSpec anim_anim_apply_warp(AnimId target, AnimWarpFn fn, void *user)
{
    AnimSpec sp = make_spec(ANIM_KIND_APPLY_WARP, target);
    sp.fn_ptr  = (void *)fn;
    sp.fn_user = user;
    return sp;
}

AnimSpec anim_anim_apply_matrix(AnimId target, const float mat4_col_major[16])
{
    AnimSpec sp = make_spec(ANIM_KIND_APPLY_MATRIX, target);
    if (mat4_col_major)
        memcpy(sp.mat, mat4_col_major, sizeof sp.mat);
    else
        mu_mat4_identity(sp.mat);
    return sp;
}

AnimSpec anim_anim_apply_matrix3(AnimId target, const float m[9])
{
    float m4[16];
    mu_mat4_identity(m4);
    if (m)
    {
        /* Convert row-major 3x3 to column-major 4x4 */
        m4[0] = m[0]; m4[4] = m[1]; m4[8]  = m[2];
        m4[1] = m[3]; m4[5] = m[4]; m4[9]  = m[5];
        m4[2] = m[6]; m4[6] = m[7]; m4[10] = m[8];
    }
    return anim_anim_apply_matrix(target, m4);
}

AnimSpec anim_anim_homotopy(AnimId target, AnimHomotopyFn fn, void *user)
{
    AnimSpec sp = make_spec(ANIM_KIND_HOMOTOPY, target);
    sp.fn_ptr  = (void *)fn;
    sp.fn_user = user;
    return sp;
}

AnimSpec anim_anim_apply_wave(AnimId target, AnimVec3 dir, float amplitude)
{
    AnimSpec sp = make_spec(ANIM_KIND_APPLY_WAVE, target);
    sp.vec    = dir;
    sp.scalar = amplitude != 0.0f ? amplitude : 0.25f;
    return sp;
}

AnimSpec anim_anim_flash(AnimVec3 point, AnimColor color, float line_len, uint32_t num_lines, float radius)
{
    AnimSpec sp = make_spec(ANIM_KIND_FLASH, ANIM_ID_NIL);
    sp.vec     = point;
    sp.color   = color.a > 0.0f ? color : ANIM_YELLOW;
    sp.scalar  = line_len > 0.0f ? line_len : 0.25f;
    sp.scalar2 = radius > 0.0f ? radius : 0.65f;
    sp.mat[0]  = (float)(num_lines > 0u ? num_lines : 12u);
    return sp;
}

AnimSpec anim_anim_focus_on(AnimVec3 point, AnimColor color, float radius)
{
    AnimSpec sp = make_spec(ANIM_KIND_FOCUS_ON, ANIM_ID_NIL);
    sp.vec     = point;
    sp.color   = color.a > 0.0f ? color : ANIM_GREY_B;
    sp.scalar  = radius > 0.0f ? radius : 1.5f;
    return sp;
}

AnimSpec anim_anim_broadcast(AnimVec3 point, AnimColor color, uint32_t count, float max_radius)
{
    AnimSpec sp = make_spec(ANIM_KIND_BROADCAST, ANIM_ID_NIL);
    sp.vec     = point;
    sp.color   = color.a > 0.0f ? color : ANIM_TEAL;
    sp.scalar  = (float)(count > 0u ? count : 4u);
    sp.scalar2 = max_radius > 0.0f ? max_radius : 2.2f;
    return sp;
}

AnimSpec anim_anim_spiral_in(AnimId target, float spiral_angle)
{
    AnimSpec sp = make_spec(ANIM_KIND_SPIRAL_IN, target);
    sp.scalar = spiral_angle != 0.0f ? spiral_angle : ANIM_TAU * 1.5f;
    return sp;
}

AnimSpec anim_anim_show_increasing_subsets(AnimId target)
{
    return make_spec(ANIM_KIND_SHOW_SUBSETS, target);
}

AnimSpec anim_anim_show_submobjects_one_by_one(AnimId target)
{
    return make_spec(ANIM_KIND_SHOW_SUBMOBJECTS_ONE_BY_ONE, target);
}

AnimSpec anim_anim_add_text_letter_by_letter(AnimId target)
{
    return make_spec(ANIM_KIND_ADD_TEXT_LETTER_BY_LETTER, target);
}

AnimSpec anim_anim_remove_text_letter_by_letter(AnimId target)
{
    return make_spec(ANIM_KIND_REMOVE_TEXT_LETTER_BY_LETTER, target);
}

AnimSpec anim_anim_type_with_cursor(AnimId target, AnimColor cursor_color)
{
    AnimSpec sp = make_spec(ANIM_KIND_TYPE_CURSOR, target);
    sp.color = cursor_color.a > 0.0f ? cursor_color : ANIM_WHITE;
    return sp;
}

AnimSpec anim_anim_zoom_inset_to(bool enable, AnimCamera cam, const float rect[4])
{
    AnimSpec sp = make_spec(ANIM_KIND_ZOOM_INSET_TO, ANIM_ID_NIL);
    sp.flag   = enable;
    sp.camera = cam;
    if (rect)
    {
        sp.mat[0] = rect[0]; sp.mat[1] = rect[1];
        sp.mat[2] = rect[2]; sp.mat[3] = rect[3];
        sp.scalar = 1.0f;
    }
    return sp;
}

AnimSpec anim_anim_camera_fly_to(AnimCamera cam, float arc_height)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_FLY_TO, ANIM_ID_NIL);
    sp.camera = cam;
    sp.scalar = arc_height;
    return sp;
}

AnimSpec anim_anim_camera_orbit_axis(float angle_rad, AnimVec3 axis, float zoom_mul)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_ORBIT_AXIS, ANIM_ID_NIL);
    sp.scalar  = angle_rad;
    sp.vec     = axis;
    sp.scalar2 = zoom_mul > 0.0f ? zoom_mul : 1.0f;
    return sp;
}

AnimSpec anim_anim_camera_dolly_zoom(float target_fov_deg)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_DOLLY_ZOOM, ANIM_ID_NIL);
    sp.scalar = MU_CLAMP(target_fov_deg, 8.0f, 130.0f);
    return sp;
}

AnimSpec anim_anim_camera_track(AnimId target_mob, AnimVec3 eye_offset, bool move_eye)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_TRACK, target_mob);
    sp.vec  = eye_offset;
    sp.flag = move_eye;
    return sp;
}

AnimSpec anim_anim_camera_shake(float amplitude, float frequency)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_SHAKE, ANIM_ID_NIL);
    sp.scalar  = amplitude;
    sp.scalar2 = frequency > 0.0f ? frequency : 6.0f;
    return sp;
}

AnimSpec anim_anim_camera_path(AnimId path_mob, AnimVec3 target_pt, bool look_ahead)
{
    AnimSpec sp = make_spec(ANIM_KIND_CAMERA_PATH, ANIM_ID_NIL);
    sp.other = path_mob;
    sp.vec   = target_pt;
    sp.flag  = look_ahead;
    return sp;
}

AnimSpec anim_anim_flip_3d(AnimId target, AnimVec3 axis, float turns)
{
    AnimSpec sp = make_spec(ANIM_KIND_FLIP_3D, target);
    sp.vec    = axis;
    sp.scalar = (fabsf(turns) > 1e-4f) ? turns : 1.0f;
    return sp;
}

AnimSpec anim_anim_flip_replace_3d(AnimId src, AnimId dst, AnimVec3 axis)
{
    AnimSpec sp = make_spec(ANIM_KIND_FLIP_REPLACE_3D, src);
    sp.other = dst;
    sp.vec   = axis;
    return sp;
}

AnimSpec anim_anim_tumble_in(AnimId target, AnimVec3 start_offset, float spin_rad, AnimVec3 axis)
{
    AnimSpec sp = make_spec(ANIM_KIND_TUMBLE_IN, target);
    sp.vec    = start_offset;
    sp.scalar = spin_rad;
    sp.mat[0] = axis.x; sp.mat[1] = axis.y; sp.mat[2] = axis.z;
    return sp;
}

AnimSpec anim_anim_tumble_out(AnimId target, AnimVec3 end_offset, float spin_rad, AnimVec3 axis)
{
    AnimSpec sp = make_spec(ANIM_KIND_TUMBLE_OUT, target);
    sp.vec    = end_offset;
    sp.scalar = spin_rad;
    sp.mat[0] = axis.x; sp.mat[1] = axis.y; sp.mat[2] = axis.z;
    return sp;
}

AnimSpec anim_anim_explode_3d(AnimId target, float radius, float spin_rad)
{
    AnimSpec sp = make_spec(ANIM_KIND_EXPLODE_3D, target);
    sp.scalar  = radius;
    sp.scalar2 = spin_rad;
    return sp;
}

AnimSpec anim_anim_swirl_3d(AnimId target, float twist_rad, AnimVec3 axis)
{
    AnimSpec sp = make_spec(ANIM_KIND_SWIRL_3D, target);
    sp.scalar = twist_rad;
    sp.vec    = axis;
    return sp;
}

AnimSpec anim_anim_rotate_around(AnimId target, AnimVec3 pivot, float angle_rad, AnimVec3 axis, bool spin_self)
{
    AnimSpec sp = make_spec(ANIM_KIND_ROTATE_AROUND, target);
    sp.vec    = pivot;
    sp.scalar = angle_rad;
    sp.mat[0] = axis.x; sp.mat[1] = axis.y; sp.mat[2] = axis.z;
    sp.flag   = spin_self;
    return sp;
}

AnimSpec anim_anim_pulse_glow(AnimId target, float peak_glow, float peak_scale)
{
    AnimSpec sp = make_spec(ANIM_KIND_PULSE_GLOW, target);
    sp.scalar  = peak_glow;
    sp.scalar2 = peak_scale > 0.0f ? peak_scale : 1.15f;
    return sp;
}

AnimSpec anim_anim_spring_move(AnimId target, AnimVec3 dst_pos, float damping, float freq)
{
    AnimSpec sp = make_spec(ANIM_KIND_SPRING_MOVE, target);
    sp.vec     = dst_pos;
    sp.scalar  = damping > 0.0f ? damping : 0.55f;
    sp.scalar2 = freq > 0.0f ? freq : 16.0f;
    return sp;
}

AnimSpec anim_anim_viewport_to(uint32_t vp_idx, bool active, AnimCamera cam, const float rect[4])
{
    AnimSpec sp = make_spec(ANIM_KIND_VIEWPORT_TO, ANIM_ID_NIL);
    sp.flag    = active;
    sp.camera  = cam;
    sp.scalar2 = (float)vp_idx;
    if (rect)
    {
        sp.mat[0] = rect[0]; sp.mat[1] = rect[1];
        sp.mat[2] = rect[2]; sp.mat[3] = rect[3];
        sp.scalar = 1.0f;
    }
    return sp;
}

AnimSpec anim_anim_apply_complex_fn(AnimId target, AnimComplexFn fn, void *user)
{
    AnimSpec sp = make_spec(ANIM_KIND_APPLY_COMPLEX_FN, target);
    sp.fn_ptr  = (void *)fn;
    sp.fn_user = user;
    return sp;
}

AnimSpec anim_anim_restore(AnimId target)
{
    return make_spec(ANIM_KIND_RESTORE, target);
}

AnimSpec anim_anim_rotating(AnimId target, float angle_rad, AnimVec3 axis, AnimVec3 about_point)
{
    AnimSpec sp = make_spec(ANIM_KIND_ROTATING, target);
    sp.scalar        = angle_rad;
    sp.vec           = axis;
    sp.camera.target = about_point;
    sp.has_rate      = true;
    sp.rate          = ANIM_RATE_LINEAR;
    return sp;
}

AnimSpec anim_anim_swap(AnimId a, AnimId b, float arc_height)
{
    AnimSpec sp = make_spec(ANIM_KIND_SWAP, a);
    sp.other  = b;
    sp.scalar = arc_height;
    return sp;
}

AnimSpec anim_anim_fade_transform(AnimId src, AnimId dst)
{
    AnimSpec sp = make_spec(ANIM_KIND_FADE_TRANSFORM, src);
    sp.other = dst;
    return sp;
}

AnimSpec anim_anim_counterclockwise_transform(AnimId src, AnimId dst, float path_arc_rad)
{
    AnimSpec sp = make_spec(ANIM_KIND_COUNTERCLOCKWISE_TRANSFORM, src);
    sp.other  = dst;
    sp.scalar = (fabsf(path_arc_rad) > 1e-4f) ? path_arc_rad : ANIM_PI;
    return sp;
}

AnimSpec anim_anim_phase_flow(AnimId target, AnimFieldFn fn, float virtual_time, void *user)
{
    AnimSpec sp = make_spec(ANIM_KIND_PHASE_FLOW, target);
    sp.fn_ptr  = (void *)fn;
    sp.fn_user = user;
    sp.scalar  = virtual_time;
    return sp;
}

AnimSpec anim_anim_blink(AnimId target, uint32_t blinks)
{
    AnimSpec sp = make_spec(ANIM_KIND_BLINK, target);
    sp.scalar = (float)(blinks > 0u ? blinks : 2u);
    return sp;
}

AnimSpec anim_anim_custom(AnimId target, AnimCustomAnimFn fn, void *user)
{
    AnimSpec sp = make_spec(ANIM_KIND_CUSTOM, target);
    sp.fn_ptr  = (void *)fn;
    sp.fn_user = user;
    return sp;
}

AnimSpec anim_spec_with_rate(AnimSpec sp, AnimRateKind rate)
{
    sp.has_rate = true;
    sp.rate     = rate;
    return sp;
}

AnimSpec anim_spec_with_lag(AnimSpec sp, float lag_ratio)
{
    sp.lag_ratio = lag_ratio;
    return sp;
}

AnimSpec anim_spec_with_custom_rate(AnimSpec sp, AnimRateFn fn, void *user)
{
    sp.custom_rate      = fn;
    sp.custom_rate_user = user;
    return sp;
}

/* ----------------------------------------------------- timeline & playback */

typedef struct TimelineStep {
    float        t0;
    float        duration;
    AnimRateKind rate;
    AnimSpec    *specs;
    uint32_t     spec_count;
    AnimScene   *before;
    bool         committed;
} TimelineStep;

typedef struct TimelineSection {
    char  name[64];
    float t_start;
    float t_end;
    bool  skip;
} TimelineSection;

struct AnimTimeline {
    TimelineStep    *steps;
    uint32_t         step_count;
    uint32_t         step_cap;
    TimelineStep    *clips;
    uint32_t         clip_count;
    uint32_t         clip_cap;
    TimelineSection *sections;
    uint32_t         section_count;
    uint32_t         section_cap;
    float            total_duration;
    float            fixed_dt;
    bool             use_fixed_dt;
};

AnimTimeline *anim_timeline_create(void)
{
    AnimTimeline *tl = (AnimTimeline *)calloc(1, sizeof *tl);
    MU_ASSERT(tl != NULL);
    tl->step_cap    = 32u;
    tl->steps       = (TimelineStep *)calloc(tl->step_cap, sizeof *tl->steps);
    tl->clip_cap    = 32u;
    tl->clips       = (TimelineStep *)calloc(tl->clip_cap, sizeof *tl->clips);
    tl->section_cap = 16u;
    tl->sections    = (TimelineSection *)calloc(tl->section_cap, sizeof *tl->sections);
    MU_ASSERT(tl->steps != NULL && tl->clips != NULL && tl->sections != NULL);
    tl->section_count        = 1u;
    tl->sections[0].t_start  = 0.0f;
    tl->sections[0].t_end    = 0.0f;
    tl->sections[0].skip     = false;
    tl->fixed_dt             = 1.0f / 60.0f;
    snprintf(tl->sections[0].name, sizeof tl->sections[0].name, "default");
    return tl;
}

void anim_timeline_destroy(AnimTimeline *tl)
{
    uint32_t i;

    if (!tl)
        return;
    for (i = 0u; i < tl->step_count; ++i)
    {
        free(tl->steps[i].specs);
        anim__snapshot_free(tl->steps[i].before);
    }
    for (i = 0u; i < tl->clip_count; ++i)
    {
        free(tl->clips[i].specs);
        anim__snapshot_free(tl->clips[i].before);
    }
    free(tl->steps);
    free(tl->clips);
    free(tl->sections);
    free(tl);
}

static float submob_lag_u(float u, uint32_t idx, uint32_t total, float lag_ratio)
{
    float lag;
    float start;
    float full;

    if (total <= 1u || lag_ratio <= 1e-4f)
        return u;
    lag   = MU_CLAMP(lag_ratio, 0.0f, 1.0f);
    full  = 1.0f + (float)(total - 1u) * lag;
    start = ((float)idx * lag) / full;
    return MU_CLAMP((u * full - (float)idx * lag), 0.0f, 1.0f) + 0.0f * start;
}

static void eval_transform_hierarchy(AnimScene *s, const AnimScene *before, AnimId src_root, AnimId dst_root, float u, int match_mode)
{
    uint32_t sn_cap = anim__count_subtree(before, src_root) + 1u;
    uint32_t dn_cap = anim__count_subtree(before, dst_root) + 1u;
    AnimId  *src_list = (AnimId *)malloc((size_t)sn_cap * sizeof *src_list);
    AnimId  *dst_list = (AnimId *)malloc((size_t)dn_cap * sizeof *dst_list);
    uint8_t *src_used = (uint8_t *)calloc(sn_cap, sizeof *src_used);
    uint8_t *dst_used = (uint8_t *)calloc(dn_cap, sizeof *dst_used);
    uint32_t sn = 0u, dn = 0u, i, j;

    MU_ASSERT(src_list != NULL && dst_list != NULL && src_used != NULL && dst_used != NULL);
    anim__collect_subtree(before, src_root, src_list, &sn, sn_cap);
    anim__collect_subtree(before, dst_root, dst_list, &dn, dn_cap);
    if (sn == 0u || dn == 0u)
    {
        free(src_list); free(dst_list); free(src_used); free(dst_used);
        return;
    }

    /* Hide dst leaf nodes initially; keep container groups visible so unmatched dst leaves can fade in. */
    s->hot[dst_root.index].visible  = true;
    s->hot[dst_root.index].in_scene = true;
    for (j = 0u; j < dn; ++j)
    {
        uint32_t di = dst_list[j].index;
        if (before->hot[di].subpath_count > 0u || before->hot[di].surf_vert_count > 0u)
            s->hot[di].visible = false;
        else
        {
            s->hot[di].visible  = true;
            s->hot[di].in_scene = true;
            s->hot[di].opacity  = before->hot[di].opacity;
        }
    }

    if (match_mode == 1)
    {
        /* Pass 1: Match tagged subtrees/groups (e.g. Typst/LaTeX {{token}} groups). */
        for (i = 0u; i < sn; ++i)
        {
            const char *stag = before->cold[src_list[i].index].tag;
            if (!stag[0] || before->hot[src_list[i].index].subpath_count > 0u || before->hot[src_list[i].index].surf_vert_count > 0u)
                continue;
            for (j = 0u; j < dn; ++j)
            {
                if (!dst_used[j] && before->hot[dst_list[j].index].subpath_count == 0u &&
                    before->hot[dst_list[j].index].surf_vert_count == 0u &&
                    strcmp(stag, before->cold[dst_list[j].index].tag) == 0)
                {
                    uint32_t sg_cap = anim__count_subtree(before, src_list[i]) + 1u;
                    uint32_t dg_cap = anim__count_subtree(before, dst_list[j]) + 1u;
                    AnimId  *sg_all = (AnimId *)malloc((size_t)sg_cap * sizeof *sg_all);
                    AnimId  *dg_all = (AnimId *)malloc((size_t)dg_cap * sizeof *dg_all);
                    uint32_t sgn = 0u, dgn = 0u, sl = 0u, dl = 0u, k, m;

                    dst_used[j] = 1u;
                    src_used[i] = 1u;
                    anim__collect_subtree(before, src_list[i], sg_all, &sgn, sg_cap);
                    anim__collect_subtree(before, dst_list[j], dg_all, &dgn, dg_cap);
                    for (k = 0u; k < sgn; ++k)
                    {
                        if (before->hot[sg_all[k].index].subpath_count > 0u || before->hot[sg_all[k].index].surf_vert_count > 0u)
                            sg_all[sl++] = sg_all[k];
                        for (m = 0u; m < sn; ++m)
                            if (src_list[m].index == sg_all[k].index)
                                src_used[m] = 1u;
                    }
                    for (k = 0u; k < dgn; ++k)
                    {
                        if (before->hot[dg_all[k].index].subpath_count > 0u || before->hot[dg_all[k].index].surf_vert_count > 0u)
                            dg_all[dl++] = dg_all[k];
                        for (m = 0u; m < dn; ++m)
                            if (dst_list[m].index == dg_all[k].index)
                                dst_used[m] = 1u;
                    }
                    if (sl > 0u && dl > 0u)
                    {
                        for (k = 0u; k < sl; ++k)
                        {
                            uint32_t dj = (k * dl) / sl;
                            anim__morph_vmob(s, sg_all[k], before, sg_all[k], before, dg_all[dj], u);
                        }
                        for (k = 0u; k < dl; ++k)
                        {
                            if ((k * sl) / dl < sl && k != (((k * sl) / dl) * dl) / sl)
                            {
                                AnimId   extra = dg_all[k];
                                uint32_t si_m  = (k * sl) / dl;
                                s->hot[extra.index].visible  = true;
                                s->hot[extra.index].in_scene = true;
                                anim__morph_vmob(s, extra, before, sg_all[si_m], before, extra, u);
                            }
                        }
                    }
                    free(sg_all);
                    free(dg_all);
                    break;
                }
            }
        }

        /* Pass 2: Match directly tagged leaves & fade unmatched leaves. */
        for (i = 0u; i < sn; ++i)
        {
            const char *stag = before->cold[src_list[i].index].tag;
            bool        matched = (src_used[i] != 0u);
            if (before->hot[src_list[i].index].subpath_count == 0u && before->hot[src_list[i].index].surf_vert_count == 0u)
                continue;
            if (!matched && stag[0])
            {
                for (j = 0u; j < dn; ++j)
                {
                    if (!dst_used[j] &&
                        (before->hot[dst_list[j].index].subpath_count > 0u || before->hot[dst_list[j].index].surf_vert_count > 0u) &&
                        strcmp(stag, before->cold[dst_list[j].index].tag) == 0)
                    {
                        dst_used[j] = 1u;
                        anim__morph_vmob(s, src_list[i], before, src_list[i], before, dst_list[j], u);
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched)
                s->hot[src_list[i].index].opacity = before->hot[src_list[i].index].opacity * (1.0f - u);
        }
        for (j = 0u; j < dn; ++j)
        {
            if (!dst_used[j] &&
                (before->hot[dst_list[j].index].subpath_count > 0u || before->hot[dst_list[j].index].surf_vert_count > 0u))
            {
                AnimId fade_id = dst_list[j];
                s->hot[fade_id.index].visible  = true;
                s->hot[fade_id.index].in_scene = true;
                s->hot[fade_id.index].opacity  = before->hot[fade_id.index].opacity * u;
            }
        }
        free(src_list); free(dst_list); free(src_used); free(dst_used);
        return;
    }

    if (match_mode == 2)
    {
        for (i = 0u; i < sn; ++i)
        {
            const VMobHot *sh = &before->hot[src_list[i].index];
            uint32_t       sc = anim_vmob_cubic_count(before, src_list[i]);
            AnimVec3       smn, smx;
            float          sar;
            uint32_t       best_j = UINT32_MAX;
            float          best_score = 1e30f;

            if (sh->subpath_count == 0u && sh->surf_vert_count == 0u)
                continue;
            anim_bbox_world(before, src_list[i], &smn, &smx);
            sar = (smx.x - smn.x + 1e-3f) / (smx.y - smn.y + 1e-3f);

            for (j = 0u; j < dn; ++j)
            {
                const VMobHot *dh = &before->hot[dst_list[j].index];
                uint32_t       dc;
                AnimVec3       dmn, dmx;
                float          dar, dx, dy, score;
                if (dst_used[j] || (dh->subpath_count == 0u && dh->surf_vert_count == 0u))
                    continue;
                dc = anim_vmob_cubic_count(before, dst_list[j]);
                anim_bbox_world(before, dst_list[j], &dmn, &dmx);
                dar = (dmx.x - dmn.x + 1e-3f) / (dmx.y - dmn.y + 1e-3f);
                dx  = 0.5f * ((dmn.x + dmx.x) - (smn.x + smx.x));
                dy  = 0.5f * ((dmn.y + dmx.y) - (smn.y + smx.y));
                score = fabsf((float)sc - (float)dc) * 2.0f + fabsf(sar - dar) * 3.0f + sqrtf(dx * dx + dy * dy) * 0.15f;
                if (score < best_score)
                {
                    best_score = score;
                    best_j = j;
                }
            }
            if (best_j != UINT32_MAX && best_score < 3.5f)
            {
                src_used[i] = 1u;
                dst_used[best_j] = 1u;
                anim__morph_vmob(s, src_list[i], before, src_list[i], before, dst_list[best_j], u);
            }
            else
            {
                s->hot[src_list[i].index].opacity = sh->opacity * (1.0f - u);
            }
        }
        for (j = 0u; j < dn; ++j)
        {
            if (!dst_used[j] &&
                (before->hot[dst_list[j].index].subpath_count > 0u || before->hot[dst_list[j].index].surf_vert_count > 0u))
            {
                AnimId fade_id = dst_list[j];
                s->hot[fade_id.index].visible  = true;
                s->hot[fade_id.index].in_scene = true;
                s->hot[fade_id.index].opacity  = before->hot[fade_id.index].opacity * u;
            }
        }
        free(src_list); free(dst_list); free(src_used); free(dst_used);
        return;
    }

    /* Leaf collection (skip empty container group nodes when pairing). */
    {
        AnimId  *s_leaves = (AnimId *)malloc((size_t)sn * sizeof *s_leaves);
        AnimId  *d_leaves = (AnimId *)malloc((size_t)dn * sizeof *d_leaves);
        uint32_t sl = 0u, dl = 0u;

        MU_ASSERT(s_leaves != NULL && d_leaves != NULL);
        for (i = 0u; i < sn; ++i)
            if (before->hot[src_list[i].index].subpath_count > 0u || before->hot[src_list[i].index].surf_vert_count > 0u)
                s_leaves[sl++] = src_list[i];
        for (i = 0u; i < dn; ++i)
            if (before->hot[dst_list[i].index].subpath_count > 0u || before->hot[dst_list[i].index].surf_vert_count > 0u)
                d_leaves[dl++] = dst_list[i];
        if (sl > 0u && dl > 0u)
        {
            for (i = 0u; i < sl; ++i)
            {
                uint32_t dj = (i * dl) / sl;
                anim__morph_vmob(s, s_leaves[i], before, s_leaves[i], before, d_leaves[dj], u);
            }
            for (j = 0u; j < dl; ++j)
            {
                if ((j * sl) / dl < sl && j != (((j * sl) / dl) * dl) / sl)
                {
                    AnimId   extra = d_leaves[j];
                    uint32_t si    = (j * sl) / dl;
                    s->hot[extra.index].visible  = true;
                    s->hot[extra.index].in_scene = true;
                    anim__morph_vmob(s, extra, before, s_leaves[si], before, extra, u);
                }
            }
        }
        free(s_leaves);
        free(d_leaves);
    }
    free(src_list);
    free(dst_list);
    free(src_used);
    free(dst_used);
}

static void apply_camera_orbit(AnimCamera *cam, float d_az, float d_el, float zoom_mul)
{
    float dx = cam->pos.x - cam->target.x;
    float dy = cam->pos.y - cam->target.y;
    float dz = cam->pos.z - cam->target.z;
    float r  = sqrtf(dx * dx + dy * dy + dz * dz);
    float az, el;

    if (r < 1e-4f)
        return;
    az = atan2f(dx, dz) + d_az;
    el = asinf(MU_CLAMP(dy / r, -0.999f, 0.999f)) + d_el;
    el = MU_CLAMP(el, -1.45f, 1.45f);

    cam->pos.x = cam->target.x + r * cosf(el) * sinf(az);
    cam->pos.y = cam->target.y + r * sinf(el);
    cam->pos.z = cam->target.z + r * cosf(el) * cosf(az);
    if (zoom_mul > 0.0f)
        cam->zoom *= zoom_mul;
}

static float v3_dotf(AnimVec3 a, AnimVec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float v3_lenf(AnimVec3 v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static AnimVec3 slerp_unit_dir(AnimVec3 a, AnimVec3 b, float t)
{
    AnimVec3 ua = v3_norm(a);
    AnimVec3 ub = v3_norm(b);
    float    d  = MU_CLAMP(v3_dotf(ua, ub), -1.0f, 1.0f);
    AnimVec3 ax;
    float    ang, q[4], in_v[3], out_v[3];

    if (d > 0.9999f)
        return v3_norm(v3_lerp(ua, ub, t));
    if (d < -0.9999f)
    {
        AnimVec3 perp = v3_cross(ua, fabsf(ua.y) < 0.9f ? ANIM_UP : ANIM_RIGHT);
        ax = v3_norm(perp);
        ang = ANIM_PI;
    }
    else
    {
        ax = v3_norm(v3_cross(ua, ub));
        ang = acosf(d);
    }
    mu_quat_axis_angle(q, (float[3]){ ax.x, ax.y, ax.z }, ang * t);
    in_v[0] = ua.x; in_v[1] = ua.y; in_v[2] = ua.z;
    mu_quat_rotate_vec3(out_v, q, in_v);
    return v3_norm(ANIM_V3(out_v[0], out_v[1], out_v[2]));
}

static void apply_camera_orbit_axis(AnimCamera *cam, float angle_rad, AnimVec3 axis, float zoom_mul)
{
    AnimVec3 ax = v3_norm(v3_dotf(axis, axis) > 1e-8f ? axis : ANIM_UP);
    float    q[4];
    float    off[3] = { cam->pos.x - cam->target.x, cam->pos.y - cam->target.y, cam->pos.z - cam->target.z };
    float    up[3]  = { cam->up.x, cam->up.y, cam->up.z };
    float    ro[3], ru[3];

    mu_quat_axis_angle(q, (float[3]){ ax.x, ax.y, ax.z }, angle_rad);
    mu_quat_rotate_vec3(ro, q, off);
    mu_quat_rotate_vec3(ru, q, up);
    cam->pos = ANIM_V3(cam->target.x + ro[0], cam->target.y + ro[1], cam->target.z + ro[2]);
    cam->up  = v3_norm(ANIM_V3(ru[0], ru[1], ru[2]));
    if (zoom_mul > 0.0f)
        cam->zoom *= zoom_mul;
}

static void rotate_node_around_pivot(AnimScene *s, const VMobHot *orig, uint32_t idx,
                                     AnimVec3 pivot, AnimVec3 axis, float angle_rad, bool spin_self)
{
    AnimVec3 ax   = v3_norm(v3_dotf(axis, axis) > 1e-8f ? axis : ANIM_UP);
    float    q_rot[4];
    float    rel[3] = { orig->pos.x - pivot.x, orig->pos.y - pivot.y, orig->pos.z - pivot.z };
    float    rel_r[3];

    mu_quat_axis_angle(q_rot, (float[3]){ ax.x, ax.y, ax.z }, angle_rad);
    mu_quat_rotate_vec3(rel_r, q_rot, rel);
    s->hot[idx].pos = ANIM_V3(pivot.x + rel_r[0], pivot.y + rel_r[1], pivot.z + rel_r[2]);
    if (spin_self)
    {
        float q_new[4];
        mu_quat_mul(q_new, q_rot, orig->rot_quat);
        mu_quat_normalize(s->hot[idx].rot_quat, q_new);
    }
    mark_subtree_dirty(s, idx);
}

static float eval_spec_rate(const AnimSpec *sp, AnimRateKind step_rate, float raw_u, bool final_commit)
{
    if (final_commit)
        return 1.0f;
    if (sp->custom_rate)
        return sp->custom_rate(raw_u, sp->custom_rate_user);
    return anim_rate_eval(sp->has_rate ? sp->rate : step_rate, raw_u);
}

static void eval_step_ex(AnimScene *s, const TimelineStep *step, float raw_u, bool final_commit, bool is_overlay, bool allow_transients)
{
    uint32_t i;

    if (!is_overlay && step->before && step->before->ambient_cam_active)
    {
        float dt_step = raw_u * step->duration;
        if (step->before->ambient_cam_illusion)
        {
            float p0  = step->before->ambient_phase;
            float p1  = p0 + step->before->ambient_d_az * dt_step;
            float w   = step->before->ambient_wobble;
            float daz = w * (sinf(p1) - sinf(p0));
            float del = w * 0.65f * (cosf(p1 * 0.85f) - cosf(p0 * 0.85f));
            apply_camera_orbit(&s->camera, daz, del, 1.0f);
            if (final_commit)
                s->ambient_phase = p1;
        }
        else
        {
            apply_camera_orbit(&s->camera,
                               step->before->ambient_d_az * dt_step,
                               step->before->ambient_d_el * dt_step, 1.0f);
        }
    }

    for (i = 0u; i < step->spec_count; ++i)
    {
        const AnimSpec *sp     = &step->specs[i];
        float           u      = eval_spec_rate(sp, step->rate, raw_u, final_commit);
        AnimId          stack_mobs[128];
        AnimId         *mobs   = stack_mobs;
        uint32_t        mcap   = 128u;
        uint32_t        mcount = 0u;
        uint32_t        mi;

        if (sp->kind == ANIM_KIND_TRACKER_TO)
        {
            float v0 = anim_tracker_get(is_overlay ? s : step->before, sp->tracker);
            anim_tracker_set(s, sp->tracker, v0 + (sp->scalar - v0) * u);
            continue;
        }
        if (sp->kind == ANIM_KIND_CAMERA_TO)
        {
            const AnimCamera *c0 = is_overlay ? &s->camera : &step->before->camera;
            const AnimCamera *c1 = &sp->camera;
            AnimCamera        c  = *c0;
            c.pos          = v3_lerp(c0->pos, c1->pos, u);
            c.target       = v3_lerp(c0->target, c1->target, u);
            c.up           = v3_lerp(c0->up, c1->up, u);
            c.fov_deg      = c0->fov_deg + (c1->fov_deg - c0->fov_deg) * u;
            c.frame_height = c0->frame_height + (c1->frame_height - c0->frame_height) * u;
            c.zoom         = c0->zoom + (c1->zoom - c0->zoom) * u;
            c.ortho_blend  = c0->ortho_blend + (c1->ortho_blend - c0->ortho_blend) * u;
            s->camera      = c;
            continue;
        }
        if (sp->kind == ANIM_KIND_CAMERA_ORBIT)
        {
            AnimCamera base = is_overlay ? s->camera : step->before->camera;
            float      zm   = 1.0f + (sp->vec.z - 1.0f) * u;
            apply_camera_orbit(&base, sp->vec.x * u, sp->vec.y * u, zm);
            s->camera = base;
            continue;
        }
        if (sp->kind == ANIM_KIND_CAMERA_FLY_TO)
        {
            const AnimCamera *c0 = is_overlay ? &s->camera : &step->before->camera;
            const AnimCamera *c1 = &sp->camera;
            AnimCamera        c  = *c0;
            AnimVec3          o0 = v3_sub(c0->pos, c0->target);
            AnimVec3          o1 = v3_sub(c1->pos, c1->target);
            float             r0 = v3_lenf(o0);
            float             r1 = v3_lenf(o1);
            float             rm = r0 + (r1 - r0) * u + sp->scalar * sinf(ANIM_PI * u);
            AnimVec3          dir = slerp_unit_dir(o0, o1, u);
            c.target       = v3_lerp(c0->target, c1->target, u);
            c.pos          = ANIM_V3(c.target.x + dir.x * rm, c.target.y + dir.y * rm, c.target.z + dir.z * rm);
            c.up           = slerp_unit_dir(c0->up, c1->up, u);
            c.fov_deg      = c0->fov_deg + (c1->fov_deg - c0->fov_deg) * u;
            c.frame_height = c0->frame_height + (c1->frame_height - c0->frame_height) * u;
            c.zoom         = c0->zoom + (c1->zoom - c0->zoom) * u;
            c.ortho_blend  = c0->ortho_blend + (c1->ortho_blend - c0->ortho_blend) * u;
            s->camera      = c;
            continue;
        }
        if (sp->kind == ANIM_KIND_CAMERA_ORBIT_AXIS)
        {
            AnimCamera base = is_overlay ? s->camera : step->before->camera;
            float      zm   = 1.0f + (sp->scalar2 - 1.0f) * u;
            apply_camera_orbit_axis(&base, sp->scalar * u, sp->vec, zm);
            s->camera = base;
            continue;
        }
        if (sp->kind == ANIM_KIND_CAMERA_DOLLY_ZOOM)
        {
            const AnimCamera *c0   = is_overlay ? &s->camera : &step->before->camera;
            AnimCamera        c    = *c0;
            float             fov0 = MU_CLAMP(c0->fov_deg, 8.0f, 130.0f);
            float             fov1 = MU_CLAMP(sp->scalar, 8.0f, 130.0f);
            float             fov  = fov0 + (fov1 - fov0) * u;
            float             t0   = tanf(0.5f * fov0 * (ANIM_PI / 180.0f));
            float             t1   = tanf(0.5f * fov * (ANIM_PI / 180.0f));
            float             rat  = t0 / MU_MAX(t1, 1e-4f);
            AnimVec3          off  = v3_sub(c0->pos, c0->target);
            c.fov_deg = fov;
            c.pos     = ANIM_V3(c0->target.x + off.x * rat, c0->target.y + off.y * rat, c0->target.z + off.z * rat);
            s->camera = c;
            continue;
        }
        if (sp->kind == ANIM_KIND_CAMERA_TRACK || sp->kind == ANIM_KIND_CAMERA_SHAKE)
            continue;
        if (sp->kind == ANIM_KIND_CAMERA_PATH)
        {
            if (ANIM_ID_VALID(sp->other))
            {
                const AnimScene  *ref = is_overlay ? s : step->before;
                const AnimCamera *c0  = &ref->camera;
                const VMobHot    *ph  = anim__hot_const(ref, sp->other);
                if (ph)
                {
                    AnimCamera c = *c0;
                    AnimVec3   lp = anim_vmob_point_at(ref, sp->other, u);
                    float      pout[3];
                    anim__sync_world_mats((AnimScene *)ref);
                    mu_mat4_transform_point(pout, ph->world_mat, (float[3]){ lp.x, lp.y, lp.z });
                    c.pos = ANIM_V3(pout[0], pout[1], pout[2]);
                    if (sp->flag)
                    {
                        AnimVec3 lp2 = anim_vmob_point_at(ref, sp->other, MU_MIN(u + 0.04f, 1.0f));
                        float    pout2[3];
                        mu_mat4_transform_point(pout2, ph->world_mat, (float[3]){ lp2.x, lp2.y, lp2.z });
                        c.target = ANIM_V3(pout2[0], pout2[1], pout2[2]);
                    }
                    else
                    {
                        c.target = v3_lerp(c0->target, sp->vec, u);
                    }
                    s->camera = c;
                }
            }
            continue;
        }
        if (sp->kind == ANIM_KIND_ZOOM_INSET_TO)
        {
            const AnimScene  *ref = is_overlay ? s : step->before;
            const AnimCamera *c0  = &ref->zoom_inset_cam;
            const AnimCamera *c1  = &sp->camera;
            AnimCamera        c   = *c0;
            c.pos          = v3_lerp(c0->pos, c1->pos, u);
            c.target       = v3_lerp(c0->target, c1->target, u);
            c.up           = v3_lerp(c0->up, c1->up, u);
            c.fov_deg      = c0->fov_deg + (c1->fov_deg - c0->fov_deg) * u;
            c.frame_height = c0->frame_height + (c1->frame_height - c0->frame_height) * u;
            c.zoom         = c0->zoom + (c1->zoom - c0->zoom) * u;
            c.ortho_blend  = c0->ortho_blend + (c1->ortho_blend - c0->ortho_blend) * u;
            s->zoom_inset_cam    = c;
            s->zoom_inset_enable = sp->flag ? (u > 0.0f) : (!final_commit && ref->zoom_inset_enable);
            if (sp->scalar > 0.5f)
            {
                int k;
                for (k = 0; k < 4; ++k)
                    s->zoom_inset_rect[k] = ref->zoom_inset_rect[k] + (sp->mat[k] - ref->zoom_inset_rect[k]) * u;
            }
            continue;
        }
        if (sp->kind == ANIM_KIND_VIEWPORT_TO)
        {
            uint32_t vp_idx = (uint32_t)(sp->scalar2 + 0.5f);
            if (vp_idx < (uint32_t)ANIM_MAX_VIEWPORTS)
            {
                const AnimScene  *ref = is_overlay ? s : step->before;
                const AnimViewport *v0 = &ref->viewports[vp_idx];
                AnimViewport       *vd = &s->viewports[vp_idx];
                const AnimCamera   *c0 = (vp_idx < ref->viewport_count) ? &v0->cam : &ref->camera;
                const AnimCamera   *c1 = &sp->camera;
                AnimCamera          c  = *c0;
                if (vp_idx >= s->viewport_count)
                    s->viewport_count = vp_idx + 1u;
                c.pos          = v3_lerp(c0->pos, c1->pos, u);
                c.target       = v3_lerp(c0->target, c1->target, u);
                c.up           = v3_lerp(c0->up, c1->up, u);
                c.fov_deg      = c0->fov_deg + (c1->fov_deg - c0->fov_deg) * u;
                c.frame_height = c0->frame_height + (c1->frame_height - c0->frame_height) * u;
                c.zoom         = c0->zoom + (c1->zoom - c0->zoom) * u;
                c.ortho_blend  = c0->ortho_blend + (c1->ortho_blend - c0->ortho_blend) * u;
                vd->cam        = c;
                vd->active     = sp->flag ? (u > 0.0f) : (!final_commit && v0->active);
                vd->draw_bg    = true;
                if (vd->border_width <= 0.0f)
                    vd->border_width = 2.0f;
                if (sp->scalar > 0.5f)
                {
                    int k;
                    for (k = 0; k < 4; ++k)
                    {
                        float r0 = (vp_idx < ref->viewport_count && v0->rect[2] > v0->rect[0]) ? v0->rect[k] : sp->mat[k];
                        vd->rect[k] = r0 + (sp->mat[k] - r0) * u;
                    }
                }
            }
            continue;
        }
        if (sp->kind == ANIM_KIND_FLASH)
        {
            if (allow_transients && !final_commit && u > 0.0f && u < 1.0f)
            {
                uint32_t nl   = (uint32_t)MU_MAX(sp->mat[0], 3.0f);
                float    len  = sp->scalar;
                float    rad  = sp->scalar2;
                float    r0   = rad * MU_CLAMP((u - 0.35f) / 0.65f, 0.0f, 1.0f);
                float    r1   = r0 + len * (1.0f - u * 0.6f);
                float    al   = sinf(ANIM_PI * u);
                AnimColor c   = sp->color;
                uint32_t k;
                c.a *= al;
                for (k = 0u; k < nl; ++k)
                {
                    float  th = ANIM_TAU * ((float)k / (float)nl);
                    AnimVec3 a = ANIM_V3(sp->vec.x + r0 * cosf(th), sp->vec.y + r0 * sinf(th), sp->vec.z);
                    AnimVec3 b = ANIM_V3(sp->vec.x + r1 * cosf(th), sp->vec.y + r1 * sinf(th), sp->vec.z);
                    AnimId ray = anim_line(s, a, b);
                    anim_set_stroke(s, ray, c, 3.2f, false);
                    anim_set_glow(s, ray, 0.9f, false);
                    anim_set_z_index(s, ray, 50.0f, false);
                    anim_add(s, ray);
                }
            }
            continue;
        }
        if (sp->kind == ANIM_KIND_FOCUS_ON)
        {
            if (allow_transients && !final_commit && u > 0.0f && u < 1.0f)
            {
                float     rad  = MU_MAX(sp->scalar * (1.0f - u), 0.04f);
                AnimColor c    = sp->color;
                AnimId    ring = anim_circle(s, rad);
                c.a *= (1.0f - 0.5f * u);
                anim_move_to(s, ring, sp->vec);
                anim_set_fill(s, ring, anim_rgba(c.r, c.g, c.b, c.a * 0.22f), false);
                anim_set_stroke(s, ring, c, 2.5f, false);
                anim_set_z_index(s, ring, 45.0f, false);
                anim_add(s, ring);
            }
            continue;
        }
        if (sp->kind == ANIM_KIND_BROADCAST)
        {
            if (allow_transients && !final_commit && u > 0.0f && u < 1.0f)
            {
                uint32_t nw = (uint32_t)MU_MAX(sp->scalar, 1.0f);
                uint32_t k;
                for (k = 0u; k < nw; ++k)
                {
                    float t0 = ((float)k / (float)nw) * 0.55f;
                    float wu = MU_CLAMP((u - t0) / 0.45f, 0.0f, 1.0f);
                    if (wu > 0.01f && wu < 0.99f)
                    {
                        float     rad  = MU_MAX(sp->scalar2 * wu, 0.02f);
                        AnimColor c    = sp->color;
                        AnimId    ring = anim_circle(s, rad);
                        c.a *= (1.0f - wu);
                        anim_move_to(s, ring, sp->vec);
                        anim_set_stroke(s, ring, c, 3.0f * (1.0f - 0.5f * wu), false);
                        anim_set_glow(s, ring, 0.7f, false);
                        anim_set_z_index(s, ring, 45.0f, false);
                        anim_add(s, ring);
                    }
                }
            }
            continue;
        }

        if (!ANIM_ID_VALID(sp->target))
            continue;

        if (sp->kind == ANIM_KIND_CUSTOM)
        {
            if (sp->fn_ptr)
                ((AnimCustomAnimFn)sp->fn_ptr)(s, sp->target, u, sp->fn_user);
            continue;
        }

        if (sp->kind == ANIM_KIND_TRANSFORM_FROM_COPY)
        {
            if (final_commit)
            {
                anim_add(s, sp->other);
                anim_set_visible(s, sp->other, true, true);
            }
            else if (ANIM_ID_VALID(sp->other))
            {
                uint32_t sn = anim__count_subtree(step->before, sp->target);
                uint32_t dn = anim__count_subtree(step->before, sp->other);
                AnimId  *sl_all = (AnimId *)malloc((size_t)sn * sizeof *sl_all);
                AnimId  *dl_all = (AnimId *)malloc((size_t)dn * sizeof *dl_all);
                AnimId  *s_l    = (AnimId *)malloc((size_t)sn * sizeof *s_l);
                AnimId  *d_l    = (AnimId *)malloc((size_t)dn * sizeof *d_l);
                uint32_t sc = 0u, dc = 0u, sl = 0u, dl = 0u, k;
                anim__collect_subtree(step->before, sp->target, sl_all, &sc, sn);
                anim__collect_subtree(step->before, sp->other, dl_all, &dc, dn);
                for (k = 0u; k < sc; ++k)
                    if (step->before->hot[sl_all[k].index].subpath_count > 0u ||
                        step->before->hot[sl_all[k].index].surf_vert_count > 0u)
                        s_l[sl++] = sl_all[k];
                for (k = 0u; k < dc; ++k)
                    if (step->before->hot[dl_all[k].index].subpath_count > 0u ||
                        step->before->hot[dl_all[k].index].surf_vert_count > 0u)
                        d_l[dl++] = dl_all[k];
                anim_add(s, sp->other);
                anim_set_visible(s, sp->other, true, true);
                if (sl > 0u && dl > 0u)
                {
                    for (k = 0u; k < dl; ++k)
                    {
                        uint32_t si = (k * sl) / dl;
                        anim__morph_vmob(s, d_l[k], step->before, s_l[si], step->before, d_l[k], u);
                    }
                }
                free(sl_all); free(dl_all); free(s_l); free(d_l);
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_FLIP_3D)
        {
            AnimVec3 mn, mx, piv;
            const VMobHot *orig = &step->before->hot[sp->target.index];
            float ang  = sp->scalar * ANIM_PI * u;
            float lift = 0.35f * sinf(ANIM_PI * u);
            if (!is_overlay)
                s->hot[sp->target.index] = *orig;
            anim_bbox_world(step->before, sp->target, &mn, &mx);
            piv = ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
            rotate_node_around_pivot(s, &s->hot[sp->target.index], sp->target.index, piv, sp->vec, ang, true);
            s->hot[sp->target.index].pos.z += lift;
            mark_subtree_dirty(s, sp->target.index);
            continue;
        }

        if (sp->kind == ANIM_KIND_FLIP_REPLACE_3D)
        {
            if (final_commit)
            {
                anim_remove(s, sp->target);
                anim_set_visible(s, sp->target, false, true);
                if (ANIM_ID_VALID(sp->other))
                {
                    anim_add(s, sp->other);
                    anim_set_visible(s, sp->other, true, true);
                }
            }
            else
            {
                float lift = 0.35f * sinf(ANIM_PI * u);
                if (u < 0.5f)
                {
                    AnimVec3 mn, mx, piv;
                    float ang = (u * 2.0f) * (0.5f * ANIM_PI);
                    anim_bbox_world(step->before, sp->target, &mn, &mx);
                    piv = ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
                    rotate_node_around_pivot(s, &step->before->hot[sp->target.index], sp->target.index, piv, sp->vec, ang, true);
                    s->hot[sp->target.index].pos.z += lift;
                    anim_set_visible(s, sp->target, true, true);
                    if (ANIM_ID_VALID(sp->other))
                        anim_set_visible(s, sp->other, false, true);
                }
                else
                {
                    anim_set_visible(s, sp->target, false, true);
                    if (ANIM_ID_VALID(sp->other))
                    {
                        AnimVec3 mn, mx, piv;
                        float ang = ((u - 0.5f) * 2.0f - 1.0f) * (0.5f * ANIM_PI);
                        anim_add(s, sp->other);
                        anim_set_visible(s, sp->other, true, true);
                        anim_bbox_world(step->before, sp->other, &mn, &mx);
                        piv = ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
                        rotate_node_around_pivot(s, &step->before->hot[sp->other.index], sp->other.index, piv, sp->vec, ang, true);
                        s->hot[sp->other.index].pos.z += lift;
                    }
                }
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_TUMBLE_IN || sp->kind == ANIM_KIND_TUMBLE_OUT)
        {
            bool     is_in = (sp->kind == ANIM_KIND_TUMBLE_IN);
            AnimVec3 ax    = ANIM_V3(sp->mat[0], sp->mat[1], sp->mat[2]);
            float    t_val = is_in ? (1.0f - u) : u;
            float    sc    = is_in ? (0.22f + 0.78f * u) : (1.0f - 0.78f * u);
            float    op    = is_in ? u : (1.0f - u);
            float    ang   = sp->scalar * t_val;
            AnimVec3 off   = ANIM_V3(sp->vec.x * t_val, sp->vec.y * t_val, sp->vec.z * t_val);
            uint32_t need  = anim__count_subtree(step->before, sp->target);
            AnimId  *list  = (AnimId *)malloc((size_t)need * sizeof *list);
            uint32_t cnt   = 0u, k;

            anim__collect_subtree(step->before, sp->target, list, &cnt, need);
            for (k = 0u; k < cnt; ++k)
            {
                uint32_t       idx = list[k].index;
                const VMobHot *orig = &step->before->hot[idx];
                VMobHot       *h    = &s->hot[idx];
                h->in_scene = is_in ? true : !final_commit;
                h->visible  = is_in ? (u > 0.0f) : (!final_commit && u < 0.999f);
                h->opacity  = orig->opacity * op;
            }
            if (cnt > 0u)
            {
                const VMobHot *orig0 = &step->before->hot[sp->target.index];
                VMobHot       *h0    = &s->hot[sp->target.index];
                h0->pos   = ANIM_V3(orig0->pos.x + off.x, orig0->pos.y + off.y, orig0->pos.z + off.z);
                h0->scale = ANIM_V3(orig0->scale.x * sc, orig0->scale.y * sc, orig0->scale.z * sc);
                if (fabsf(ang) > 1e-6f)
                {
                    AnimVec3 nax = v3_norm(v3_dotf(ax, ax) > 1e-8f ? ax : ANIM_UP);
                    float    q_rot[4], q_new[4];
                    mu_quat_axis_angle(q_rot, (float[3]){ nax.x, nax.y, nax.z }, ang);
                    mu_quat_mul(q_new, q_rot, orig0->rot_quat);
                    mu_quat_normalize(h0->rot_quat, q_new);
                }
                else
                {
                    memcpy(h0->rot_quat, orig0->rot_quat, sizeof h0->rot_quat);
                }
                mark_subtree_dirty(s, sp->target.index);
            }
            free(list);
            continue;
        }

        if (sp->kind == ANIM_KIND_EXPLODE_3D)
        {
            uint32_t need = anim__count_subtree(step->before, sp->target);
            AnimId  *list = (AnimId *)malloc((size_t)need * sizeof *list);
            uint32_t cnt  = 0u, k;
            AnimVec3 mn, mx, gcen;

            anim_bbox_world(step->before, sp->target, &mn, &mx);
            gcen = ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
            anim__collect_subtree(step->before, sp->target, list, &cnt, need);
            for (k = 0u; k < cnt; ++k)
            {
                uint32_t       idx  = list[k].index;
                const VMobHot *orig = &step->before->hot[idx];
                VMobHot       *h    = &s->hot[idx];
                if (cnt > 1u && k > 0u)
                {
                    AnimVec3 cmn, cmx, cc, dir, sax;
                    float    dlen;
                    anim_bbox_world(step->before, list[k], &cmn, &cmx);
                    cc   = ANIM_V3(0.5f * (cmn.x + cmx.x), 0.5f * (cmn.y + cmx.y), 0.5f * (cmn.z + cmx.z));
                    dir  = v3_sub(cc, gcen);
                    dlen = v3_lenf(dir);
                    if (dlen < 1e-4f)
                    {
                        float a1 = (float)k * 2.39996f;
                        float a2 = sinf((float)k * 1.7f);
                        dir = ANIM_V3(cosf(a1), a2, sinf(a1));
                    }
                    dir = v3_norm(dir);
                    h->pos = ANIM_V3(orig->pos.x + dir.x * sp->scalar * u,
                                     orig->pos.y + dir.y * sp->scalar * u,
                                     orig->pos.z + dir.z * sp->scalar * u);
                    sax = v3_norm(ANIM_V3(sinf((float)k * 1.3f + 0.4f), cosf((float)k * 2.1f), sinf((float)k * 0.7f - 0.5f)));
                    if (fabsf(sp->scalar2 * u) > 1e-6f)
                    {
                        float q_rot[4], q_new[4];
                        mu_quat_axis_angle(q_rot, (float[3]){ sax.x, sax.y, sax.z }, sp->scalar2 * u);
                        mu_quat_mul(q_new, q_rot, orig->rot_quat);
                        mu_quat_normalize(h->rot_quat, q_new);
                    }
                    mark_subtree_dirty(s, idx);
                }
                if (orig->surf_vert_count > 0u && orig->surf_idx_count >= 3u && fabsf(sp->scalar * u) > 1e-5f)
                {
                    uint32_t nt = orig->surf_idx_count / 3u;
                    uint32_t vo = s->surf_vert_count;
                    uint32_t io = s->surf_idx_count;
                    uint32_t ti, ci;
                    anim__ensure_surf_vert_cap(s, nt * 3u);
                    anim__ensure_surf_idx_cap(s, nt * 3u);
                    h = &s->hot[idx];
                    for (ti = 0u; ti < nt; ++ti)
                    {
                        SurfaceVert tri[3];
                        AnimVec3    tcent, e1, e2, fn, sax;
                        float       q_rot[4];
                        for (ci = 0u; ci < 3u; ++ci)
                        {
                            uint32_t vi = step->before->surf_indices[orig->surf_idx_offset + ti * 3u + ci];
                            tri[ci] = step->before->surf_verts[orig->surf_vert_offset + vi];
                        }
                        tcent = ANIM_V3((tri[0].pos.x + tri[1].pos.x + tri[2].pos.x) / 3.0f,
                                        (tri[0].pos.y + tri[1].pos.y + tri[2].pos.y) / 3.0f,
                                        (tri[0].pos.z + tri[1].pos.z + tri[2].pos.z) / 3.0f);
                        e1 = v3_sub(tri[1].pos, tri[0].pos);
                        e2 = v3_sub(tri[2].pos, tri[0].pos);
                        fn = v3_cross(e1, e2);
                        fn = (v3_lenf(fn) > 1e-6f) ? v3_norm(fn) : v3_norm(tcent);
                        sax = v3_norm(ANIM_V3(sinf((float)ti * 1.13f + 0.3f), cosf((float)ti * 1.91f), sinf((float)ti * 0.57f)));
                        mu_quat_axis_angle(q_rot, (float[3]){ sax.x, sax.y, sax.z }, sp->scalar2 * u);
                        for (ci = 0u; ci < 3u; ++ci)
                        {
                            float rel[3] = { tri[ci].pos.x - tcent.x, tri[ci].pos.y - tcent.y, tri[ci].pos.z - tcent.z };
                            float r_rot[3], n_rot[3];
                            mu_quat_rotate_vec3(r_rot, q_rot, rel);
                            mu_quat_rotate_vec3(n_rot, q_rot, (float[3]){ tri[ci].norm.x, tri[ci].norm.y, tri[ci].norm.z });
                            tri[ci].pos = ANIM_V3(tcent.x + r_rot[0] + fn.x * sp->scalar * u,
                                                  tcent.y + r_rot[1] + fn.y * sp->scalar * u,
                                                  tcent.z + r_rot[2] + fn.z * sp->scalar * u);
                            tri[ci].norm = v3_norm(ANIM_V3(n_rot[0], n_rot[1], n_rot[2]));
                            s->surf_verts[vo + ti * 3u + ci]   = tri[ci];
                            s->surf_indices[io + ti * 3u + ci] = ti * 3u + ci;
                        }
                    }
                    s->surf_vert_count += nt * 3u;
                    s->surf_idx_count  += nt * 3u;
                    h->surf_vert_offset = vo;
                    h->surf_vert_count  = nt * 3u;
                    h->surf_idx_offset  = io;
                    h->surf_idx_count   = nt * 3u;
                }
            }
            free(list);
            continue;
        }

        if (sp->kind == ANIM_KIND_SWIRL_3D)
        {
            uint32_t need = anim__count_subtree(step->before, sp->target);
            AnimId  *list = (AnimId *)malloc((size_t)need * sizeof *list);
            uint32_t cnt  = 0u, k;
            AnimVec3 mn, mx, cen, ax;
            anim_bbox_world(step->before, sp->target, &mn, &mx);
            cen = ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.5f * (mn.z + mx.z));
            ax  = v3_norm(v3_dotf(sp->vec, sp->vec) > 1e-8f ? sp->vec : ANIM_UP);
            anim__sync_world_mats((AnimScene *)step->before);
            anim__collect_subtree(step->before, sp->target, list, &cnt, need);
            for (k = 0u; k < cnt; ++k)
            {
                uint32_t       idx = list[k].index;
                const VMobHot *b   = &step->before->hot[idx];
                VMobHot       *h   = &s->hot[idx];
                float          w_inv[16];
                uint32_t       sp_i, vi;
                mu_mat4_inverse(b->world_mat, w_inv);
                if (b->subpath_count > 0u && h->subpath_count == b->subpath_count)
                {
                    for (sp_i = 0u; sp_i < b->subpath_count; ++sp_i)
                    {
                        const SubpathMeta *sb = &step->before->subpaths[b->subpath_offset + sp_i];
                        const SubpathMeta *sh = &s->subpaths[h->subpath_offset + sp_i];
                        uint32_t ci;
                        if (sb->cubic_count != sh->cubic_count) continue;
                        for (ci = 0u; ci < sb->cubic_count; ++ci)
                        {
                            const AnimCubic *cb = &step->before->cubics[sb->cubic_offset + ci];
                            AnimCubic       *cd = &s->cubics[sh->cubic_offset + ci];
                            AnimVec3         pts[4] = { cb->p0, cb->p1, cb->p2, cb->p3 };
                            AnimVec3         out[4];
                            int              pi;
                            for (pi = 0; pi < 4; ++pi)
                            {
                                float pw_f[3], rel[3], r_rot[3], p_loc[3];
                                float h_ax, ang, q_rot[4];
                                mu_mat4_transform_point(pw_f, b->world_mat, (float[3]){ pts[pi].x, pts[pi].y, pts[pi].z });
                                rel[0] = pw_f[0] - cen.x; rel[1] = pw_f[1] - cen.y; rel[2] = pw_f[2] - cen.z;
                                h_ax = rel[0] * ax.x + rel[1] * ax.y + rel[2] * ax.z;
                                ang  = sp->scalar * h_ax * u;
                                mu_quat_axis_angle(q_rot, (float[3]){ ax.x, ax.y, ax.z }, ang);
                                mu_quat_rotate_vec3(r_rot, q_rot, rel);
                                mu_mat4_transform_point(p_loc, w_inv, (float[3]){ cen.x + r_rot[0], cen.y + r_rot[1], cen.z + r_rot[2] });
                                out[pi] = ANIM_V3(p_loc[0], p_loc[1], p_loc[2]);
                            }
                            cd->p0 = out[0]; cd->p1 = out[1]; cd->p2 = out[2]; cd->p3 = out[3];
                        }
                    }
                }
                if (b->surf_vert_count > 0u && h->surf_vert_count == b->surf_vert_count)
                {
                    for (vi = 0u; vi < b->surf_vert_count; ++vi)
                    {
                        const SurfaceVert *vb = &step->before->surf_verts[b->surf_vert_offset + vi];
                        SurfaceVert       *vd = &s->surf_verts[h->surf_vert_offset + vi];
                        float pw_f[3], rel[3], r_rot[3], p_loc[3], n_rot[3];
                        float h_ax, ang, q_rot[4];
                        mu_mat4_transform_point(pw_f, b->world_mat, (float[3]){ vb->pos.x, vb->pos.y, vb->pos.z });
                        rel[0] = pw_f[0] - cen.x; rel[1] = pw_f[1] - cen.y; rel[2] = pw_f[2] - cen.z;
                        h_ax = rel[0] * ax.x + rel[1] * ax.y + rel[2] * ax.z;
                        ang  = sp->scalar * h_ax * u;
                        mu_quat_axis_angle(q_rot, (float[3]){ ax.x, ax.y, ax.z }, ang);
                        mu_quat_rotate_vec3(r_rot, q_rot, rel);
                        mu_quat_rotate_vec3(n_rot, q_rot, (float[3]){ vb->norm.x, vb->norm.y, vb->norm.z });
                        mu_mat4_transform_point(p_loc, w_inv, (float[3]){ cen.x + r_rot[0], cen.y + r_rot[1], cen.z + r_rot[2] });
                        vd->pos  = ANIM_V3(p_loc[0], p_loc[1], p_loc[2]);
                        vd->norm = v3_norm(ANIM_V3(n_rot[0], n_rot[1], n_rot[2]));
                    }
                }
            }
            free(list);
            continue;
        }

        if (sp->kind == ANIM_KIND_ROTATE_AROUND)
        {
            const VMobHot *orig = is_overlay ? &s->hot[sp->target.index] : &step->before->hot[sp->target.index];
            AnimVec3 ax = ANIM_V3(sp->mat[0], sp->mat[1], sp->mat[2]);
            rotate_node_around_pivot(s, orig, sp->target.index, sp->vec, ax, sp->scalar * u, sp->flag);
            continue;
        }

        if (sp->kind == ANIM_KIND_PULSE_GLOW)
        {
            float    bell = sinf(ANIM_PI * raw_u);
            uint32_t need = anim__count_subtree(step->before, sp->target);
            AnimId  *list = (AnimId *)malloc((size_t)need * sizeof *list);
            uint32_t cnt  = 0u, k;
            anim__collect_subtree(step->before, sp->target, list, &cnt, need);
            for (k = 0u; k < cnt; ++k)
            {
                uint32_t       idx = list[k].index;
                const VMobHot *b   = is_overlay ? &s->hot[idx] : &step->before->hot[idx];
                s->hot[idx].glow = b->glow + sp->scalar * bell;
            }
            if (cnt > 0u)
            {
                const VMobHot *b0 = is_overlay ? &s->hot[sp->target.index] : &step->before->hot[sp->target.index];
                float sc = 1.0f + (sp->scalar2 - 1.0f) * bell;
                s->hot[sp->target.index].scale = ANIM_V3(b0->scale.x * sc, b0->scale.y * sc, b0->scale.z * sc);
                mark_subtree_dirty(s, sp->target.index);
            }
            free(list);
            continue;
        }

        if (sp->kind == ANIM_KIND_SPRING_MOVE)
        {
            float    su = sp->has_rate ? u : anim_rate_eval_spring(raw_u, sp->scalar, sp->scalar2);
            AnimVec3 c0 = anim_center(step->before, sp->target);
            AnimVec3 p0 = step->before->hot[sp->target.index].pos;
            AnimVec3 d  = ANIM_V3(sp->vec.x - c0.x, sp->vec.y - c0.y, sp->vec.z - c0.z);
            if (final_commit) su = 1.0f;
            anim_set_pos(s, sp->target, ANIM_V3(p0.x + d.x * su, p0.y + d.y * su, p0.z + d.z * su));
            continue;
        }

        if (sp->kind == ANIM_KIND_APPLY_MATRIX || sp->kind == ANIM_KIND_APPLY_WARP ||
            sp->kind == ANIM_KIND_HOMOTOPY || sp->kind == ANIM_KIND_APPLY_WAVE ||
            sp->kind == ANIM_KIND_APPLY_COMPLEX_FN || sp->kind == ANIM_KIND_PHASE_FLOW)
        {
            uint32_t need = anim__count_subtree(step->before, sp->target);
            AnimId  *list = (AnimId *)malloc((size_t)need * sizeof *list);
            uint32_t cnt = 0u, k;
            anim__sync_world_mats((AnimScene *)step->before);
            anim__collect_subtree(step->before, sp->target, list, &cnt, need);
            for (k = 0u; k < cnt; ++k)
            {
                uint32_t       idx = list[k].index;
                const VMobHot *b   = &step->before->hot[idx];
                VMobHot       *h   = &s->hot[idx];
                float          w_inv[16];
                uint32_t       sp_i, vi;
                if ((b->subpath_count == 0u || h->subpath_count != b->subpath_count) &&
                    (b->surf_vert_count == 0u || h->surf_vert_count != b->surf_vert_count))
                    continue;
                mu_mat4_inverse(b->world_mat, w_inv);
                if (b->subpath_count > 0u && h->subpath_count == b->subpath_count)
                for (sp_i = 0u; sp_i < b->subpath_count; ++sp_i)
                {
                    const SubpathMeta *sb = &step->before->subpaths[b->subpath_offset + sp_i];
                    const SubpathMeta *sh = &s->subpaths[h->subpath_offset + sp_i];
                    uint32_t ci;
                    if (sb->cubic_count != sh->cubic_count)
                        continue;
                    for (ci = 0u; ci < sb->cubic_count; ++ci)
                    {
                        const AnimCubic *cb = &step->before->cubics[sb->cubic_offset + ci];
                        AnimCubic       *cd = &s->cubics[sh->cubic_offset + ci];
                        AnimVec3         pts[4] = { cb->p0, cb->p1, cb->p2, cb->p3 };
                        AnimVec3         out[4];
                        int              pi;
                        for (pi = 0; pi < 4; ++pi)
                        {
                            float p_in[3] = { pts[pi].x, pts[pi].y, pts[pi].z };
                            float pw_f[3];
                            AnimVec3 pw, pw2;
                            float p_loc[3];
                            mu_mat4_transform_point(pw_f, b->world_mat, p_in);
                            pw = ANIM_V3(pw_f[0], pw_f[1], pw_f[2]);
                            if (sp->kind == ANIM_KIND_APPLY_MATRIX)
                            {
                                float pm[3];
                                mu_mat4_transform_point(pm, sp->mat, pw_f);
                                pw2 = v3_lerp(pw, ANIM_V3(pm[0], pm[1], pm[2]), u);
                            }
                            else if (sp->kind == ANIM_KIND_APPLY_WARP && sp->fn_ptr)
                            {
                                AnimVec3 target_p = ((AnimWarpFn)sp->fn_ptr)(pw, sp->fn_user);
                                pw2 = v3_lerp(pw, target_p, u);
                            }
                            else if (sp->kind == ANIM_KIND_HOMOTOPY && sp->fn_ptr)
                            {
                                pw2 = ((AnimHomotopyFn)sp->fn_ptr)(pw, u, sp->fn_user);
                            }
                            else if (sp->kind == ANIM_KIND_APPLY_COMPLEX_FN && sp->fn_ptr)
                            {
                                AnimComplex z = ((AnimComplexFn)sp->fn_ptr)((AnimComplex){ pw.x, pw.y }, sp->fn_user);
                                pw2 = v3_lerp(pw, ANIM_V3(z.re, z.im, pw.z), u);
                            }
                            else if (sp->kind == ANIM_KIND_PHASE_FLOW && sp->fn_ptr)
                            {
                                AnimFieldFn ffn = (AnimFieldFn)sp->fn_ptr;
                                float dt_f = (sp->scalar * u) / 6.0f;
                                int stp;
                                pw2 = pw;
                                for (stp = 0; stp < 6; ++stp)
                                {
                                    AnimVec3 k1 = ffn(pw2, sp->fn_user);
                                    AnimVec3 mid = ANIM_V3(pw2.x + 0.5f * dt_f * k1.x, pw2.y + 0.5f * dt_f * k1.y, pw2.z + 0.5f * dt_f * k1.z);
                                    AnimVec3 k2 = ffn(mid, sp->fn_user);
                                    pw2.x += dt_f * k2.x; pw2.y += dt_f * k2.y; pw2.z += dt_f * k2.z;
                                }
                            }
                            else
                            {
                                float bell = sinf(ANIM_PI * u);
                                float ph   = pw.x * 2.2f + pw.y * 1.1f - ANIM_TAU * 1.5f * u;
                                float off  = sp->scalar * sinf(ph) * bell;
                                float vec_sq = sp->vec.x * sp->vec.x + sp->vec.y * sp->vec.y + sp->vec.z * sp->vec.z;
                                AnimVec3 d = v3_norm(vec_sq > 1e-8f ? sp->vec : ANIM_UP);
                                pw2 = ANIM_V3(pw.x + d.x * off, pw.y + d.y * off, pw.z + d.z * off);
                            }
                            mu_mat4_transform_point(p_loc, w_inv, (float[3]){ pw2.x, pw2.y, pw2.z });
                            out[pi] = ANIM_V3(p_loc[0], p_loc[1], p_loc[2]);
                        }
                        cd->p0 = out[0]; cd->p1 = out[1]; cd->p2 = out[2]; cd->p3 = out[3];
                    }
                }
                if (b->surf_vert_count > 0u && h->surf_vert_count == b->surf_vert_count)
                {
                    for (vi = 0u; vi < b->surf_vert_count; ++vi)
                    {
                        const SurfaceVert *vb = &step->before->surf_verts[b->surf_vert_offset + vi];
                        SurfaceVert       *vd = &s->surf_verts[h->surf_vert_offset + vi];
                        float pw_f[3], p_loc[3];
                        AnimVec3 pw, pw2;
                        mu_mat4_transform_point(pw_f, b->world_mat, (float[3]){ vb->pos.x, vb->pos.y, vb->pos.z });
                        pw = ANIM_V3(pw_f[0], pw_f[1], pw_f[2]);
                        if (sp->kind == ANIM_KIND_APPLY_MATRIX)
                        {
                            float pm[3];
                            mu_mat4_transform_point(pm, sp->mat, pw_f);
                            pw2 = v3_lerp(pw, ANIM_V3(pm[0], pm[1], pm[2]), u);
                        }
                        else if (sp->kind == ANIM_KIND_APPLY_WARP && sp->fn_ptr)
                        {
                            AnimVec3 target_p = ((AnimWarpFn)sp->fn_ptr)(pw, sp->fn_user);
                            pw2 = v3_lerp(pw, target_p, u);
                        }
                        else if (sp->kind == ANIM_KIND_HOMOTOPY && sp->fn_ptr)
                        {
                            pw2 = ((AnimHomotopyFn)sp->fn_ptr)(pw, u, sp->fn_user);
                        }
                        else if (sp->kind == ANIM_KIND_APPLY_COMPLEX_FN && sp->fn_ptr)
                        {
                            AnimComplex z = ((AnimComplexFn)sp->fn_ptr)((AnimComplex){ pw.x, pw.y }, sp->fn_user);
                            pw2 = v3_lerp(pw, ANIM_V3(z.re, z.im, pw.z), u);
                        }
                        else if (sp->kind == ANIM_KIND_PHASE_FLOW && sp->fn_ptr)
                        {
                            AnimFieldFn ffn = (AnimFieldFn)sp->fn_ptr;
                            float dt_f = (sp->scalar * u) / 6.0f;
                            int stp;
                            pw2 = pw;
                            for (stp = 0; stp < 6; ++stp)
                            {
                                AnimVec3 k1 = ffn(pw2, sp->fn_user);
                                AnimVec3 mid = ANIM_V3(pw2.x + 0.5f * dt_f * k1.x, pw2.y + 0.5f * dt_f * k1.y, pw2.z + 0.5f * dt_f * k1.z);
                                AnimVec3 k2 = ffn(mid, sp->fn_user);
                                pw2.x += dt_f * k2.x; pw2.y += dt_f * k2.y; pw2.z += dt_f * k2.z;
                            }
                        }
                        else
                        {
                            float bell = sinf(ANIM_PI * u);
                            float ph   = pw.x * 2.2f + pw.y * 1.1f - ANIM_TAU * 1.5f * u;
                            float off  = sp->scalar * sinf(ph) * bell;
                            float vec_sq = sp->vec.x * sp->vec.x + sp->vec.y * sp->vec.y + sp->vec.z * sp->vec.z;
                            AnimVec3 d = v3_norm(vec_sq > 1e-8f ? sp->vec : ANIM_UP);
                            pw2 = ANIM_V3(pw.x + d.x * off, pw.y + d.y * off, pw.z + d.z * off);
                        }
                        mu_mat4_transform_point(p_loc, w_inv, (float[3]){ pw2.x, pw2.y, pw2.z });
                        vd->pos = ANIM_V3(p_loc[0], p_loc[1], p_loc[2]);
                    }
                }
            }
            free(list);
            continue;
        }

        if (sp->kind == ANIM_KIND_TRANSFORM || sp->kind == ANIM_KIND_REPLACEMENT_TRANSFORM ||
            sp->kind == ANIM_KIND_TRANSFORM_MATCHING_TEX || sp->kind == ANIM_KIND_TRANSFORM_MATCHING_SHAPES ||
            sp->kind == ANIM_KIND_COUNTERCLOCKWISE_TRANSFORM)
        {
            int mm = (sp->kind == ANIM_KIND_TRANSFORM_MATCHING_TEX) ? 1
                   : ((sp->kind == ANIM_KIND_TRANSFORM_MATCHING_SHAPES) ? 2 : 0);
            if (final_commit && sp->kind == ANIM_KIND_REPLACEMENT_TRANSFORM)
            {
                anim_remove(s, sp->target);
                anim_set_visible(s, sp->target, false, true);
                anim_add(s, sp->other);
                anim_set_visible(s, sp->other, true, true);
            }
            else if (final_commit)
            {
                eval_transform_hierarchy(s, step->before, sp->target, sp->other, 1.0f, mm);
                anim_remove(s, sp->other);
            }
            else
            {
                eval_transform_hierarchy(s, step->before, sp->target, sp->other, u, mm);
                if (sp->kind == ANIM_KIND_COUNTERCLOCKWISE_TRANSFORM)
                {
                    AnimVec3 c0 = anim_center(step->before, sp->target);
                    AnimVec3 c1 = anim_center(step->before, sp->other);
                    float    dx = c1.x - c0.x, dy = c1.y - c0.y;
                    float    arc_h = 0.5f * tanf(0.25f * sp->scalar) * sinf(ANIM_PI * u);
                    anim_shift(s, sp->target, ANIM_V3(-dy * arc_h, dx * arc_h, 0.0f));
                }
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_FADE_TRANSFORM)
        {
            AnimVec3 c0 = anim_center(step->before, sp->target);
            AnimVec3 c1 = anim_center(step->before, sp->other);
            float    w0 = MU_MAX(anim_width(step->before, sp->target), 1e-3f);
            float    h0 = MU_MAX(anim_height(step->before, sp->target), 1e-3f);
            float    w1 = MU_MAX(anim_width(step->before, sp->other), 1e-3f);
            float    h1 = MU_MAX(anim_height(step->before, sp->other), 1e-3f);
            if (final_commit)
            {
                anim_remove(s, sp->target);
                anim_set_visible(s, sp->target, false, true);
                anim_add(s, sp->other);
                anim_set_visible(s, sp->other, true, true);
            }
            else
            {
                float sx0 = 1.0f + (w1 / w0 - 1.0f) * u;
                float sy0 = 1.0f + (h1 / h0 - 1.0f) * u;
                float sx1 = (w0 / w1) + (1.0f - w0 / w1) * u;
                float sy1 = (h0 / h1) + (1.0f - h0 / h1) * u;
                const VMobHot *hb0 = &step->before->hot[sp->target.index];
                const VMobHot *hb1 = &step->before->hot[sp->other.index];
                s->hot[sp->target.index].scale = ANIM_V3(hb0->scale.x * sx0, hb0->scale.y * sy0, hb0->scale.z);
                mark_subtree_dirty(s, sp->target.index);
                anim_move_to(s, sp->target, v3_lerp(c0, c1, u));
                anim_set_opacity(s, sp->target, hb0->opacity * (1.0f - u), true);

                anim_add(s, sp->other);
                anim_set_visible(s, sp->other, true, true);
                s->hot[sp->other.index].scale = ANIM_V3(hb1->scale.x * sx1, hb1->scale.y * sy1, hb1->scale.z);
                mark_subtree_dirty(s, sp->other.index);
                anim_move_to(s, sp->other, v3_lerp(c0, c1, u));
                anim_set_opacity(s, sp->other, hb1->opacity * u, true);
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_SWAP)
        {
            if (ANIM_ID_VALID(sp->target) && ANIM_ID_VALID(sp->other))
            {
                AnimVec3 c0 = anim_center(step->before, sp->target);
                AnimVec3 c1 = anim_center(step->before, sp->other);
                float    ay = sp->scalar * sinf(ANIM_PI * u);
                anim_move_to(s, sp->target, ANIM_V3(c0.x + (c1.x - c0.x) * u, c0.y + (c1.y - c0.y) * u + ay, c0.z + (c1.z - c0.z) * u));
                anim_move_to(s, sp->other,  ANIM_V3(c1.x + (c0.x - c1.x) * u, c1.y + (c0.y - c1.y) * u - ay, c1.z + (c0.z - c1.z) * u));
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_ROTATING)
        {
            const VMobHot *hb = anim__hot_const(step->before, sp->target);
            if (hb)
            {
                float    dq[4], q_new[4];
                AnimVec3 ax = v3_norm((sp->vec.x*sp->vec.x + sp->vec.y*sp->vec.y + sp->vec.z*sp->vec.z > 1e-8f) ? sp->vec : ANIM_OUT);
                float    ang = sp->scalar * u;
                AnimVec3 piv = sp->camera.target;
                float    dp[3] = { hb->pos.x - piv.x, hb->pos.y - piv.y, hb->pos.z - piv.z };
                float    rot_m[16], rp[3];
                mu_quat_axis_angle(dq, (float[3]){ ax.x, ax.y, ax.z }, ang);
                mu_quat_mul(q_new, dq, hb->rot_quat);
                memcpy(s->hot[sp->target.index].rot_quat, q_new, sizeof q_new);
                mu_mat4_from_quat(rot_m, dq);
                mu_mat4_transform_dir(rp, rot_m, dp);
                s->hot[sp->target.index].pos = ANIM_V3(piv.x + rp[0], piv.y + rp[1], piv.z + rp[2]);
                mark_subtree_dirty(s, sp->target.index);
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_RESTORE)
        {
            uint32_t need = anim__count_subtree(step->before, sp->target);
            AnimId  *list = (AnimId *)malloc((size_t)need * sizeof *list);
            uint32_t cnt = 0u, k;
            MU_ASSERT(list != NULL);
            anim__collect_subtree(step->before, sp->target, list, &cnt, need);
            for (k = 0u; k < cnt; ++k)
            {
                uint32_t        idx  = list[k].index;
                const VMobHot  *bh   = &step->before->hot[idx];
                const VMobCold *bc   = &step->before->cold[idx];
                VMobHot        *dh   = &s->hot[idx];
                float           dq[4];
                if (!bc->has_saved_state) continue;
                dh->pos          = v3_lerp(bh->pos, bc->saved_pos, u);
                dh->scale        = v3_lerp(bh->scale, bc->saved_scale, u);
                mu_quat_nlerp(dq, bh->rot_quat, bc->saved_rot_quat, u);
                memcpy(dh->rot_quat, dq, sizeof dq);
                dh->fill_color   = anim_color_lerp_oklab(bh->fill_color, bc->saved_fill, u);
                dh->stroke_color = anim_color_lerp_oklab(bh->stroke_color, bc->saved_stroke, u);
                dh->stroke_width = bh->stroke_width + (bc->saved_stroke_w - bh->stroke_width) * u;
                dh->opacity      = bh->opacity + (bc->saved_opacity - bh->opacity) * u;
                dh->glow         = bh->glow + (bc->saved_glow - bh->glow) * u;
                mark_subtree_dirty(s, idx);
            }
            free(list);
            continue;
        }

        if (sp->kind == ANIM_KIND_BLINK)
        {
            float op = 0.5f * (1.0f + cosf(ANIM_TAU * sp->scalar * u));
            anim_set_opacity(s, sp->target, step->before->hot[sp->target.index].opacity * (final_commit ? 1.0f : op), true);
            continue;
        }

        if (sp->kind == ANIM_KIND_CIRCUMSCRIBE)
        {
            if (allow_transients && !final_commit && u > 0.0f && u < 1.0f)
            {
                AnimVec3 mn, mx;
                AnimId   box;
                float    buff = sp->scalar;
                anim_bbox_world(step->before, sp->target, &mn, &mx);
                box = anim_rect(s, (mx.x - mn.x) + 2.0f * buff, (mx.y - mn.y) + 2.0f * buff);
                anim_move_to(s, box, ANIM_V3(0.5f * (mn.x + mx.x), 0.5f * (mn.y + mx.y), 0.0f));
                anim_set_stroke(s, box, sp->color.a > 0.0f ? sp->color : ANIM_YELLOW, 4.0f, false);
                anim_set_glow(s, box, 0.8f, false);
                if (u < 0.5f)
                    anim_set_trim(s, box, 0.0f, 2.0f * u, false);
                else
                    anim_set_trim(s, box, 2.0f * (u - 0.5f), 1.0f, false);
                anim_add(s, box);
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_FOLLOW_PATH)
        {
            if (ANIM_ID_VALID(sp->other))
            {
                const AnimScene *ref = is_overlay ? s : step->before;
                AnimVec3         p   = anim_vmob_point_at(ref, sp->other, u);
                const VMobHot   *ph  = anim__hot_const(ref, sp->other);
                if (ph)
                {
                    float pin[3] = { p.x, p.y, p.z };
                    float pout[3];
                    anim__sync_world_mats((AnimScene *)ref);
                    mu_mat4_transform_point(pout, ph->world_mat, pin);
                    anim_move_to(s, sp->target, ANIM_V3(pout[0], pout[1], pout[2]));
                    if (sp->flag)
                    {
                        AnimVec3 p0 = anim_vmob_point_at(ref, sp->other, MU_MAX(u - 0.01f, 0.0f));
                        AnimVec3 p1 = anim_vmob_point_at(ref, sp->other, MU_MIN(u + 0.01f, 1.0f));
                        float    dx = p1.x - p0.x;
                        float    dy = p1.y - p0.y;
                        if (dx * dx + dy * dy > 1e-8f)
                        {
                            float ang = atan2f(dy, dx);
                            mu_quat_axis_angle(s->hot[sp->target.index].rot_quat, (float[3]){ 0.0f, 0.0f, 1.0f }, ang);
                            mark_subtree_dirty(s, sp->target.index);
                        }
                    }
                }
            }
            continue;
        }

        if (sp->kind == ANIM_KIND_MOVE_TO)
        {
            AnimVec3 c0 = anim_center(step->before, sp->target);
            AnimVec3 p0 = step->before->hot[sp->target.index].pos;
            AnimVec3 d  = ANIM_V3(sp->vec.x - c0.x, sp->vec.y - c0.y, sp->vec.z - c0.z);
            anim_set_pos(s, sp->target, ANIM_V3(p0.x + d.x * u, p0.y + d.y * u, p0.z + d.z * u));
            continue;
        }
        if (sp->kind == ANIM_KIND_MOVE_ARC)
        {
            AnimVec3 c0 = anim_center(step->before, sp->target);
            AnimVec3 p0 = step->before->hot[sp->target.index].pos;
            AnimVec3 d  = ANIM_V3(sp->vec.x - c0.x, sp->vec.y - c0.y, sp->vec.z - c0.z);
            float    ay = sp->scalar * sinf(ANIM_PI * u);
            anim_set_pos(s, sp->target, ANIM_V3(p0.x + d.x * u, p0.y + d.y * u + ay, p0.z + d.z * u));
            continue;
        }
        if (sp->kind == ANIM_KIND_SHIFT)
        {
            AnimVec3 p0 = is_overlay ? s->hot[sp->target.index].pos : step->before->hot[sp->target.index].pos;
            anim_set_pos(s, sp->target, ANIM_V3(p0.x + sp->vec.x * u, p0.y + sp->vec.y * u, p0.z + sp->vec.z * u));
            continue;
        }
        if (sp->kind == ANIM_KIND_SCALE)
        {
            AnimVec3 sc0 = is_overlay ? s->hot[sp->target.index].scale : step->before->hot[sp->target.index].scale;
            float    f   = 1.0f + (sp->scalar - 1.0f) * u;
            s->hot[sp->target.index].scale = ANIM_V3(sc0.x * f, sc0.y * f, sc0.z * f);
            mark_subtree_dirty(s, sp->target.index);
            continue;
        }
        if (sp->kind == ANIM_KIND_ROTATE)
        {
            if (!is_overlay)
                s->hot[sp->target.index] = step->before->hot[sp->target.index];
            anim_rotate(s, sp->target, sp->scalar * u, sp->vec);
            continue;
        }

        {
            uint32_t need = anim__count_subtree(step->before, sp->target);
            if (need > mcap)
            {
                mobs = (AnimId *)malloc((size_t)need * sizeof *mobs);
                MU_ASSERT(mobs != NULL);
                mcap = need;
            }
        }
        anim__collect_subtree(step->before, sp->target, mobs, &mcount, mcap);
        for (mi = 0u; mi < mcount; ++mi)
        {
            uint32_t       idx = mobs[mi].index;
            const VMobHot *b   = is_overlay ? &s->hot[idx] : &step->before->hot[idx];
            VMobHot       *h   = &s->hot[idx];
            float          lag = sp->lag_ratio >= 0.0f ? sp->lag_ratio
                                                       : (sp->kind == ANIM_KIND_WRITE ? 0.14f : (sp->kind == ANIM_KIND_CREATE ? 0.08f : 0.04f));
            float          lu  = submob_lag_u(u, mi, mcount, lag);

            switch (sp->kind)
            {
            case ANIM_KIND_CREATE:
            case ANIM_KIND_WRITE:
            case ANIM_KIND_DRAW_BORDER_THEN_FILL:
            {
                const VMobHot *orig = &step->before->hot[idx];
                h->in_scene = true;
                h->visible  = (lu > 0.0f);
                if (lu < 0.72f)
                {
                    h->trim_start   = 0.0f;
                    h->trim_end     = lu / 0.72f;
                    h->fill_color.a = orig->fill_color.a * MU_CLAMP((lu - 0.35f) / 0.37f, 0.0f, 1.0f);
                }
                else
                {
                    h->trim_start   = 0.0f;
                    h->trim_end     = 1.0f;
                    h->fill_color.a = orig->fill_color.a * MU_CLAMP((lu - 0.50f) / 0.50f, 0.0f, 1.0f);
                }
                if (sp->kind == ANIM_KIND_DRAW_BORDER_THEN_FILL)
                {
                    h->trim_end     = MU_CLAMP(lu * 2.0f, 0.0f, 1.0f);
                    h->fill_color.a = orig->fill_color.a * MU_CLAMP((lu - 0.5f) * 2.0f, 0.0f, 1.0f);
                }
                if (final_commit)
                {
                    h->trim_start   = orig->trim_start;
                    h->trim_end     = orig->trim_end;
                    h->fill_color.a = orig->fill_color.a;
                    h->visible      = true;
                }
                break;
            }

            case ANIM_KIND_UNCREATE:
            case ANIM_KIND_UNWRITE:
            {
                const VMobHot *orig = &step->before->hot[idx];
                h->trim_start   = 0.0f;
                h->trim_end     = 1.0f - lu;
                h->fill_color.a = orig->fill_color.a * MU_CLAMP(1.0f - lu * 2.0f, 0.0f, 1.0f);
                h->visible      = !final_commit && (lu < 0.999f);
                if (final_commit)
                    h->in_scene = false;
                break;
            }

            case ANIM_KIND_GROW_FROM_EDGE:
            {
                const VMobHot *orig = &step->before->hot[idx];
                h->in_scene = true;
                h->visible  = (lu > 0.0f);
                if (mi == 0u)
                {
                    AnimVec3 mn, mx;
                    AnimVec3 anchor;
                    float    sc = MU_MAX(lu, 0.001f);
                    anim_bbox_world(step->before, sp->target, &mn, &mx);
                    anchor = ANIM_V3(
                        sp->vec.x < -0.2f ? mn.x : (sp->vec.x > 0.2f ? mx.x : 0.5f * (mn.x + mx.x)),
                        sp->vec.y < -0.2f ? mn.y : (sp->vec.y > 0.2f ? mx.y : 0.5f * (mn.y + mx.y)),
                        0.5f * (mn.z + mx.z));
                    h->scale = ANIM_V3(orig->scale.x * sc, orig->scale.y * sc, orig->scale.z * sc);
                    h->pos   = ANIM_V3(anchor.x + (orig->pos.x - anchor.x) * sc,
                                       anchor.y + (orig->pos.y - anchor.y) * sc,
                                       anchor.z + (orig->pos.z - anchor.z) * sc);
                    mark_subtree_dirty(s, idx);
                }
                break;
            }

            case ANIM_KIND_SPIN_IN:
            {
                const VMobHot *orig = &step->before->hot[idx];
                h->in_scene = true;
                h->visible  = (lu > 0.0f);
                if (mi == 0u)
                {
                    float sc = MU_MAX(lu, 0.001f);
                    *h = *orig;
                    h->in_scene = true;
                    h->visible  = (lu > 0.0f);
                    h->scale    = ANIM_V3(orig->scale.x * sc, orig->scale.y * sc, orig->scale.z * sc);
                    anim_rotate(s, sp->target, sp->scalar * (1.0f - lu), ANIM_OUT);
                }
                break;
            }

            case ANIM_KIND_SHRINK_TO_CENTER:
            {
                const VMobHot *orig = &step->before->hot[idx];
                h->visible  = !final_commit && (lu < 0.999f);
                h->opacity  = orig->opacity * (1.0f - lu);
                if (mi == 0u)
                {
                    AnimVec3 center = anim_center(step->before, sp->target);
                    float    sc     = MU_MAX(1.0f - lu, 0.001f);
                    h->scale = ANIM_V3(orig->scale.x * sc, orig->scale.y * sc, orig->scale.z * sc);
                    h->pos   = ANIM_V3(center.x + (orig->pos.x - center.x) * sc,
                                       center.y + (orig->pos.y - center.y) * sc,
                                       center.z + (orig->pos.z - center.z) * sc);
                    mark_subtree_dirty(s, idx);
                }
                if (final_commit)
                    h->in_scene = false;
                break;
            }

            case ANIM_KIND_SPIRAL_IN:
            {
                const VMobHot *orig = &step->before->hot[idx];
                AnimVec3 center     = anim_center(step->before, sp->target);
                float    ang        = sp->scalar * (1.0f - lu);
                float    ca = cosf(ang), sa = sinf(ang);
                float    dx = orig->pos.x - center.x;
                float    dy = orig->pos.y - center.y;
                h->in_scene = true;
                h->visible  = (lu > 0.0f);
                h->opacity  = orig->opacity * lu;
                if (mi > 0u || mcount == 1u)
                {
                    h->pos = ANIM_V3(center.x + (dx * ca - dy * sa) * lu,
                                     center.y + (dx * sa + dy * ca) * lu,
                                     orig->pos.z);
                    h->scale = ANIM_V3(orig->scale.x * MU_MAX(lu, 0.01f),
                                       orig->scale.y * MU_MAX(lu, 0.01f),
                                       orig->scale.z * MU_MAX(lu, 0.01f));
                    mark_subtree_dirty(s, idx);
                }
                break;
            }

            case ANIM_KIND_SHOW_SUBSETS:
            {
                float thr = mcount > 1u ? ((float)mi / (float)mcount) : 0.0f;
                h->in_scene = true;
                h->visible  = final_commit || (u >= thr);
                break;
            }

            case ANIM_KIND_SHOW_SUBMOBJECTS_ONE_BY_ONE:
            {
                if (mi == 0u)
                {
                    uint32_t cc = anim_child_count(s, sp->target);
                    h->in_scene = true;
                    h->visible  = true;
                    if (cc > 0u)
                    {
                        uint32_t active = final_commit ? (cc - 1u) : (uint32_t)MU_CLAMP(floorf(u * (float)cc), 0.0f, (float)(cc - 1u));
                        uint32_t k;
                        for (k = 0u; k < cc; ++k)
                            anim_set_visible(s, anim_child_at(s, sp->target, k), (k == active), true);
                    }
                }
                break;
            }

            case ANIM_KIND_ADD_TEXT_LETTER_BY_LETTER:
            {
                bool is_leaf = (h->subpath_count > 0u || h->surf_vert_count > 0u);
                h->in_scene = true;
                if (!is_leaf)
                {
                    h->visible = true;
                }
                else
                {
                    float thr = (mcount > 1u) ? ((float)mi / (float)mcount) : 0.0f;
                    float span = (mcount > 1u) ? (1.0f / (float)mcount) : 1.0f;
                    float loc = final_commit ? 1.0f : MU_CLAMP((u - thr) / MU_MAX(span, 1e-4f), 0.0f, 1.0f);
                    h->visible = final_commit || (u > thr);
                    h->opacity = step->before->hot[idx].opacity * loc;
                }
                break;
            }

            case ANIM_KIND_REMOVE_TEXT_LETTER_BY_LETTER:
            {
                bool is_leaf = (h->subpath_count > 0u || h->surf_vert_count > 0u);
                h->in_scene = true;
                if (!is_leaf)
                {
                    h->visible = !final_commit;
                }
                else
                {
                    uint32_t rev = (mcount > mi) ? (mcount - mi) : 1u;
                    float    thr = (mcount > 1u) ? ((float)rev / (float)mcount) : 1.0f;
                    h->visible = !final_commit && (u < thr);
                }
                break;
            }

            case ANIM_KIND_TYPE_CURSOR:
            {
                float thr = mcount > 1u ? ((float)mi / (float)mcount) : 0.0f;
                h->in_scene = true;
                h->visible  = final_commit || (u >= thr);
                if (allow_transients && !final_commit && u > 0.0f && u < 1.0f && mi == mcount - 1u)
                {
                    uint32_t last_shown = (uint32_t)MU_CLAMP(floorf(u * (float)mcount), 0.0f, (float)(mcount - 1u));
                    AnimVec3 mn, mx;
                    float    ch;
                    AnimId   cur;
                    anim_bbox_world(step->before, mobs[last_shown], &mn, &mx);
                    ch = MU_MAX(mx.y - mn.y, 0.28f);
                    cur = anim_rect(s, ch * 0.18f, ch * 1.15f);
                    anim_move_to(s, cur, ANIM_V3(mx.x + ch * 0.16f, 0.5f * (mn.y + mx.y), mx.z));
                    anim_set_fill(s, cur, sp->color, false);
                    anim_set_stroke(s, cur, anim_rgba(0.0f, 0.0f, 0.0f, 0.0f), 0.0f, false);
                    anim_set_glow(s, cur, 0.7f, false);
                    anim_add(s, cur);
                }
                break;
            }

            case ANIM_KIND_FADE_IN:
            {
                const VMobHot *orig = &step->before->hot[idx];
                h->in_scene = true;
                h->visible  = (lu > 0.0f);
                h->opacity  = orig->opacity * lu;
                if (mi == 0u)
                {
                    float sc = sp->scalar + (1.0f - sp->scalar) * lu;
                    h->pos   = ANIM_V3(orig->pos.x - sp->vec.x * (1.0f - lu), orig->pos.y - sp->vec.y * (1.0f - lu), orig->pos.z - sp->vec.z * (1.0f - lu));
                    h->scale = ANIM_V3(orig->scale.x * sc, orig->scale.y * sc, orig->scale.z * sc);
                    mark_subtree_dirty(s, idx);
                }
                break;
            }

            case ANIM_KIND_FADE_OUT:
                h->opacity = b->opacity * (1.0f - lu);
                h->visible = !final_commit && (lu < 0.999f);
                if (mi == 0u)
                {
                    float sc = 1.0f + (sp->scalar - 1.0f) * lu;
                    h->pos   = ANIM_V3(b->pos.x + sp->vec.x * lu, b->pos.y + sp->vec.y * lu, b->pos.z + sp->vec.z * lu);
                    h->scale = ANIM_V3(b->scale.x * sc, b->scale.y * sc, b->scale.z * sc);
                    mark_subtree_dirty(s, idx);
                }
                if (final_commit)
                    h->in_scene = false;
                break;

            case ANIM_KIND_GROW_FROM_CENTER:
            case ANIM_KIND_GROW_FROM_POINT:
            case ANIM_KIND_GROW_ARROW:
            {
                const VMobHot *orig = &step->before->hot[idx];
                h->in_scene = true;
                h->visible  = (lu > 0.0f);
                if (mi == 0u)
                {
                    AnimVec3 origin = (sp->kind == ANIM_KIND_GROW_FROM_POINT) ? sp->vec : anim_center(step->before, sp->target);
                    float    sc     = MU_MAX(lu, 0.001f);
                    h->scale = ANIM_V3(orig->scale.x * sc, orig->scale.y * sc, orig->scale.z * sc);
                    h->pos   = ANIM_V3(origin.x + (orig->pos.x - origin.x) * sc, origin.y + (orig->pos.y - origin.y) * sc, origin.z + (orig->pos.z - origin.z) * sc);
                    mark_subtree_dirty(s, idx);
                }
                break;
            }

            case ANIM_KIND_INDICATE:
            {
                float tb = anim_rate_eval(ANIM_RATE_THERE_AND_BACK, raw_u);
                float sc = 1.0f + (sp->scalar - 1.0f) * tb;
                AnimColor tgt = (sp->color.a > 0.0f) ? sp->color : ANIM_YELLOW;
                AnimColor b_sc = b->stroke_color;
                AnimColor b_fc = b->fill_color;
                h->stroke_color = anim_color_lerp_oklab(b_sc, tgt, tb);
                if (b_fc.a > 0.0f)
                    h->fill_color = anim_color_lerp_oklab(b_fc, tgt, tb);
                h->glow = b->glow + 0.8f * tb;
                if (mi == 0u)
                {
                    h->scale = ANIM_V3(b->scale.x * sc, b->scale.y * sc, b->scale.z * sc);
                    mark_subtree_dirty(s, idx);
                }
                break;
            }

            case ANIM_KIND_SHOW_PASSING_FLASH:
            {
                float w = sp->scalar;
                if (final_commit)
                {
                    h->trim_start = b->trim_start;
                    h->trim_end   = b->trim_end;
                    h->glow       = b->glow;
                }
                else
                {
                    h->in_scene   = true;
                    h->visible    = true;
                    h->trim_start = MU_CLAMP(lu * (1.0f + w) - w, 0.0f, 1.0f);
                    h->trim_end   = MU_CLAMP(lu * (1.0f + w), 0.0f, 1.0f);
                    h->glow       = b->glow + 1.1f;
                }
                break;
            }

            case ANIM_KIND_WIGGLE:
            {
                float wg = anim_rate_eval(ANIM_RATE_WIGGLE, raw_u);
                float sc = 1.0f + (sp->scalar - 1.0f) * fabsf(wg);
                if (mi == 0u)
                {
                    AnimVec3 sc0 = b->scale;
                    if (!is_overlay)
                        *h = *b;
                    h->scale = ANIM_V3(sc0.x * sc, sc0.y * sc, sc0.z * sc);
                    anim_rotate(s, sp->target, sp->vec.z * wg, ANIM_OUT);
                }
                break;
            }

            case ANIM_KIND_SET_COLOR:
                if (b->fill_color.a > 0.0f)
                    h->fill_color = anim_color_lerp_oklab(b->fill_color, sp->color, u);
                if (b->stroke_color.a > 0.0f)
                    h->stroke_color = anim_color_lerp_oklab(b->stroke_color, sp->color, u);
                break;

            case ANIM_KIND_SET_OPACITY:
                h->opacity = b->opacity + (sp->scalar - b->opacity) * u;
                break;

            case ANIM_KIND_DASH_FLOW:
                h->dash_offset_px = b->dash_offset_px + sp->scalar * u;
                break;

            default:
                break;
            }
        }
        if (mobs != stack_mobs)
            free(mobs);
    }

    for (i = 0u; i < step->spec_count; ++i)
    {
        const AnimSpec *sp = &step->specs[i];
        float           u  = eval_spec_rate(sp, step->rate, raw_u, final_commit);
        if (sp->kind == ANIM_KIND_CAMERA_TRACK)
        {
            const AnimCamera *c0  = is_overlay ? &s->camera : &step->before->camera;
            AnimCamera        c   = *c0;
            AnimVec3          sub = ANIM_ID_VALID(sp->target) ? anim_center(s, sp->target) : c0->target;
            c.target = v3_lerp(c0->target, sub, u);
            if (sp->flag)
            {
                AnimVec3 eye_dst = ANIM_V3(sub.x + sp->vec.x, sub.y + sp->vec.y, sub.z + sp->vec.z);
                c.pos = v3_lerp(c0->pos, eye_dst, u);
            }
            s->camera = c;
        }
        else if (sp->kind == ANIM_KIND_CAMERA_SHAKE)
        {
            if (!final_commit && raw_u > 0.0f && raw_u < 1.0f)
            {
                AnimCamera *c    = &s->camera;
                AnimVec3    fwd  = v3_norm(v3_sub(c->target, c->pos));
                AnimVec3    rt   = v3_norm(v3_cross(fwd, c->up));
                AnimVec3    cup  = v3_norm(v3_cross(rt, fwd));
                float       env  = sinf(ANIM_PI * raw_u) * (1.0f - 0.55f * raw_u);
                float       ph   = ANIM_TAU * sp->scalar2 * raw_u;
                float       sx   = sp->scalar * env * sinf(ph);
                float       sy   = sp->scalar * 0.72f * env * cosf(ph * 1.37f);
                c->pos    = ANIM_V3(c->pos.x + rt.x * sx + cup.x * sy,
                                    c->pos.y + rt.y * sx + cup.y * sy,
                                    c->pos.z + rt.z * sx + cup.z * sy);
                c->target = ANIM_V3(c->target.x + rt.x * sx + cup.x * sy,
                                    c->target.y + rt.y * sx + cup.y * sy,
                                    c->target.z + rt.z * sx + cup.z * sy);
            }
        }
    }
}

static void eval_step(AnimScene *s, const TimelineStep *step, float raw_u, bool final_commit, bool is_overlay)
{
    eval_step_ex(s, step, raw_u, final_commit, is_overlay, true);
}

void anim_play(AnimTimeline *tl, AnimScene *scene, const AnimSpec *specs, uint32_t count, float duration, AnimRateKind rate)
{
    TimelineStep *st;
    float         dur = MU_MAX(duration, 1e-4f);
    uint32_t      k;

    for (k = 0u; k < tl->clip_count; ++k)
    {
        TimelineStep *cl = &tl->clips[k];
        if (!cl->committed && cl->t0 + cl->duration <= tl->total_duration + 1e-5f)
        {
            eval_step(scene, cl, 1.0f, true, true);
            cl->committed = true;
        }
    }

    if (tl->step_count >= tl->step_cap)
    {
        uint32_t      new_cap = tl->step_cap * 2u;
        TimelineStep *buf     = (TimelineStep *)realloc(tl->steps, (size_t)new_cap * sizeof *buf);
        MU_ASSERT(buf != NULL);
        tl->steps    = buf;
        tl->step_cap = new_cap;
    }

    st             = &tl->steps[tl->step_count++];
    st->t0         = tl->total_duration;
    st->duration   = dur;
    st->rate       = rate;
    st->spec_count = count;
    st->specs      = count ? (AnimSpec *)malloc((size_t)count * sizeof *st->specs) : NULL;
    if (count > 0u)
        memcpy(st->specs, specs, (size_t)count * sizeof *st->specs);
    st->before     = anim__snapshot_clone(scene);
    st->committed  = true;

    tl->total_duration += dur;

    {
        float fdt = (tl->fixed_dt > 1e-4f) ? tl->fixed_dt : (1.0f / 60.0f);
        if (tl->use_fixed_dt && (scene->has_traced_paths || scene->updater_count > 0u) && dur > fdt)
        {
            uint32_t sub = (uint32_t)MU_CLAMP(ceilf(dur / fdt), 1.0f, 240.0f);
            float    h   = dur / (float)sub;
            uint32_t sk;
            for (sk = 1u; sk < sub; ++sk)
            {
                if (count > 0u)
                    eval_step_ex(scene, st, (float)sk / (float)sub, false, false, false);
                anim_eval_updaters(scene, h);
            }
            eval_step(scene, st, 1.0f, true, false);
            anim_eval_updaters(scene, h);
        }
        else if (scene->has_traced_paths && count > 0u && dur > 0.02f)
        {
            uint32_t sub = 64u, sk;
            for (sk = 1u; sk < sub; ++sk)
            {
                eval_step_ex(scene, st, (float)sk / (float)sub, false, false, false);
                anim_eval_updaters(scene, dur / (float)sub);
            }
            eval_step(scene, st, 1.0f, true, false);
            anim_eval_updaters(scene, dur / (float)sub);
        }
        else
        {
            eval_step(scene, st, 1.0f, true, false);
            anim_eval_updaters(scene, dur);
        }
    }
}

void anim_play_lagged(AnimTimeline *tl, AnimScene *scene, const AnimSpec *specs, uint32_t count, float total_duration, float lag_ratio, AnimRateKind rate)
{
    float    t0 = tl->total_duration;
    float    lag = MU_CLAMP(lag_ratio, 0.0f, 1.0f);
    float    tot = MU_MAX(total_duration, 1e-4f);
    float    item_dur;
    uint32_t i;

    if (count == 0u || !specs)
    {
        anim_wait(tl, scene, tot);
        return;
    }
    if (count == 1u)
    {
        anim_play(tl, scene, specs, 1u, tot, rate);
        return;
    }
    item_dur = tot / (1.0f + lag * (float)(count - 1u));
    for (i = 0u; i < count; ++i)
    {
        float start_i = t0 + (float)i * lag * item_dur;
        anim_play_at(tl, scene, start_i, &specs[i], 1u, item_dur, rate);
    }
    anim_wait(tl, scene, 1e-4f);
}

void anim_play_sequence(AnimTimeline *tl, AnimScene *scene, const AnimSpec *specs, uint32_t count, float step_duration, AnimRateKind rate)
{
    uint32_t i;
    if (count == 0u || !specs)
    {
        anim_wait(tl, scene, step_duration);
        return;
    }
    for (i = 0u; i < count; ++i)
        anim_play(tl, scene, &specs[i], 1u, step_duration, rate);
}

void anim_play_at(AnimTimeline *tl, AnimScene *scene, float start_time_sec, const AnimSpec *specs, uint32_t count, float duration, AnimRateKind rate)
{
    TimelineStep *cl;
    float         t0  = MU_MAX(start_time_sec, 0.0f);
    float         dur = MU_MAX(duration, 1e-4f);
    uint32_t      k;

    if (tl->step_count == 0u)
        anim_play(tl, scene, NULL, 0u, 1e-4f, ANIM_RATE_LINEAR);

    if (tl->clip_count >= tl->clip_cap)
    {
        uint32_t      new_cap = tl->clip_cap * 2u;
        TimelineStep *buf     = (TimelineStep *)realloc(tl->clips, (size_t)new_cap * sizeof *buf);
        MU_ASSERT(buf != NULL);
        tl->clips    = buf;
        tl->clip_cap = new_cap;
    }

    cl             = &tl->clips[tl->clip_count++];
    cl->t0         = t0;
    cl->duration   = dur;
    cl->rate       = rate;
    cl->spec_count = count;
    cl->specs      = count ? (AnimSpec *)malloc((size_t)count * sizeof *cl->specs) : NULL;
    if (count > 0u)
        memcpy(cl->specs, specs, (size_t)count * sizeof *cl->specs);
    cl->before     = anim__snapshot_clone(scene);
    cl->committed  = false;

    /* Keep clips ordered by t0 via insertion swap */
    k = tl->clip_count - 1u;
    while (k > 0u && tl->clips[k - 1u].t0 > tl->clips[k].t0)
    {
        TimelineStep tmp   = tl->clips[k - 1u];
        tl->clips[k - 1u]  = tl->clips[k];
        tl->clips[k]       = tmp;
        k--;
    }

    if (t0 + dur > tl->total_duration)
        tl->total_duration = t0 + dur;
}

void anim_wait(AnimTimeline *tl, AnimScene *scene, float duration)
{
    anim_play(tl, scene, NULL, 0u, duration, ANIM_RATE_LINEAR);
}

void anim_timeline_next_section(AnimTimeline *tl, const char *name, bool skip_animations)
{
    TimelineSection *sec;
    if (!tl)
        return;
    if (tl->section_count > 0u)
        tl->sections[tl->section_count - 1u].t_end = tl->total_duration;
    if (tl->section_count >= tl->section_cap)
    {
        uint32_t         new_cap = tl->section_cap * 2u;
        TimelineSection *buf     = (TimelineSection *)realloc(tl->sections, (size_t)new_cap * sizeof *buf);
        MU_ASSERT(buf != NULL);
        tl->sections    = buf;
        tl->section_cap = new_cap;
    }
    sec          = &tl->sections[tl->section_count++];
    sec->t_start = tl->total_duration;
    sec->t_end   = tl->total_duration;
    sec->skip    = skip_animations;
    snprintf(sec->name, sizeof sec->name, "%s", (name && name[0]) ? name : "section");
}

void anim_timeline_set_fixed_dt(AnimTimeline *tl, float fixed_dt)
{
    if (!tl)
        return;
    if (fixed_dt > 1e-4f)
    {
        tl->fixed_dt     = fixed_dt;
        tl->use_fixed_dt = true;
    }
    else
    {
        tl->fixed_dt     = 1.0f / 60.0f;
        tl->use_fixed_dt = false;
    }
}

uint32_t anim_timeline_section_count(const AnimTimeline *tl)
{
    return tl ? tl->section_count : 0u;
}

bool anim_timeline_section_info(const AnimTimeline *tl, uint32_t idx, const char **out_name, float *out_start, float *out_end, bool *out_skip)
{
    const TimelineSection *sec;
    if (!tl || idx >= tl->section_count)
        return false;
    sec = &tl->sections[idx];
    if (out_name)  *out_name  = sec->name;
    if (out_start) *out_start = sec->t_start;
    if (out_end)   *out_end   = (idx + 1u == tl->section_count) ? tl->total_duration : sec->t_end;
    if (out_skip)  *out_skip  = sec->skip;
    return true;
}

float anim_timeline_duration(const AnimTimeline *tl)
{
    return tl ? tl->total_duration : 0.0f;
}

static void eval_active_clips_at(const AnimTimeline *tl, AnimScene *scene, uint32_t step_idx, float tk)
{
    uint32_t k;
    for (k = 0u; k < tl->clip_count; ++k)
    {
        const TimelineStep *cl = &tl->clips[k];
        float               cu;
        bool                cdone;
        if (cl->t0 > tk)
            break;
        if (cl->committed && cl->t0 + cl->duration <= tl->steps[step_idx].t0 + 1e-5f)
            continue;
        cu    = (cl->duration > 1e-6f) ? MU_CLAMP((tk - cl->t0) / cl->duration, 0.0f, 1.0f) : 1.0f;
        cdone = (tk >= cl->t0 + cl->duration - 1e-6f);
        {
            TimelineStep live = *cl;
            AnimScene   *snap = anim__snapshot_clone(scene);
            live.before       = snap;
            eval_step(scene, &live, cu, cdone, true);
            anim__snapshot_free(snap);
        }
    }
}

void anim_seek(AnimTimeline *tl, AnimScene *scene, float time_sec)
{
    float    t = MU_CLAMP(time_sec, 0.0f, tl->total_duration);
    uint32_t lo, hi, idx;
    float    raw_u, dt_rem, fdt;
    bool     step_done;

    if (tl->step_count == 0u)
    {
        anim_eval_updaters(scene, 0.0f);
        return;
    }

    lo = 0u;
    hi = tl->step_count - 1u;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo + 1u) / 2u;
        if (tl->steps[mid].t0 <= t)
            lo = mid;
        else
            hi = mid - 1u;
    }
    idx = lo;

    anim__snapshot_restore(scene, tl->steps[idx].before);
    raw_u     = (tl->steps[idx].duration > 1e-6f) ? MU_CLAMP((t - tl->steps[idx].t0) / tl->steps[idx].duration, 0.0f, 1.0f) : 1.0f;
    step_done = (t >= tl->steps[idx].t0 + tl->steps[idx].duration - 1e-6f);
    dt_rem    = MU_MAX(t - tl->steps[idx].t0, 0.0f);
    fdt       = (tl->fixed_dt > 1e-4f) ? tl->fixed_dt : (1.0f / 60.0f);

    if (tl->use_fixed_dt && (scene->has_traced_paths || scene->updater_count > 0u) && dt_rem > fdt)
    {
        uint32_t nsub = (uint32_t)MU_CLAMP(ceilf(dt_rem / fdt), 1.0f, 240.0f);
        float    h    = dt_rem / (float)nsub;
        uint32_t sk;
        for (sk = 1u; sk < nsub; ++sk)
        {
            float frac = (float)sk / (float)nsub;
            float tk   = tl->steps[idx].t0 + dt_rem * frac;
            if (tl->steps[idx].spec_count > 0u)
                eval_step_ex(scene, &tl->steps[idx], raw_u * frac, false, false, false);
            eval_active_clips_at(tl, scene, idx, tk);
            anim_eval_updaters(scene, h);
        }
        eval_step(scene, &tl->steps[idx], raw_u, step_done, false);
        eval_active_clips_at(tl, scene, idx, t);
        anim_eval_updaters(scene, h);
    }
    else
    {
        if (scene->has_traced_paths && tl->steps[idx].spec_count > 0u && raw_u > 0.0f)
        {
            uint32_t nsub = (uint32_t)MU_MAX(ceilf(raw_u * 64.0f), 1.0f);
            uint32_t sk;
            for (sk = 1u; sk < nsub; ++sk)
            {
                eval_step_ex(scene, &tl->steps[idx], raw_u * ((float)sk / (float)nsub), false, false, false);
                anim_eval_updaters(scene, 0.0f);
            }
        }
        eval_step(scene, &tl->steps[idx], raw_u, step_done, false);
        eval_active_clips_at(tl, scene, idx, t);
        anim_eval_updaters(scene, dt_rem);
    }
}
