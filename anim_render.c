/*
 * anim_render.c — GPU band Bezier rasterizer, Z-buffered lit 3D surfaces,
 *                 temporal motion blur, PNG/MP4 export & interactive preview.
 * Follows mu_lib/AGENTS.md conventions (C11, aligned block decls, zero hot malloc).
 */
#include "anim_internal.h"
#include "third_party/mugfx/desktop/platform.h"
#include "third_party/mugfx/desktop/vk.h"
#include <ctype.h>
#include <sys/stat.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/mugfx/desktop/external/glfw/deps/stb_image_write.h"

/* ----------------------------------------------------- GPU ABI structs */

typedef struct GpuAnimBand {
    float    bounds[4];
    uint32_t meta[4];
} GpuAnimBand;
_Static_assert(sizeof(GpuAnimBand) == 32, "GpuAnimBand ABI mismatch");

typedef struct GpuAnimShape {
    float fill_color[4];
    float fill_color2[4];
    float stroke_color[4];
    float style[4];
    float box[4];
    float grad_params[4];
    float dash_params[4];
    float clip_box[4];
    float sheen_color[4];
    float extra[4];
} GpuAnimShape;
_Static_assert(sizeof(GpuAnimShape) == 160, "GpuAnimShape ABI mismatch");

typedef struct GpuAnimGrad {
    float    stops[8][4];
    uint32_t meta[4];
} GpuAnimGrad;
_Static_assert(sizeof(GpuAnimGrad) == 144, "GpuAnimGrad ABI mismatch");

typedef struct GpuAnimQuadSeg {
    float    p0_p1[4];
    float    p2_w[4];
    float    c0[4];
    float    c1[4];
    float    arc_len[4];
    uint32_t meta[4];
} GpuAnimQuadSeg;
_Static_assert(sizeof(GpuAnimQuadSeg) == 96, "GpuAnimQuadSeg ABI mismatch");

typedef struct GpuSurfaceVert {
    float pos_u[4];
    float norm_v[4];
    float color[4];
} GpuSurfaceVert;
_Static_assert(sizeof(GpuSurfaceVert) == 48, "GpuSurfaceVert ABI mismatch");

typedef struct GpuLight {
    float pos_type[4];
    float color_int[4];
} GpuLight;
_Static_assert(sizeof(GpuLight) == 32, "GpuLight ABI mismatch");

typedef struct GpuSceneLighting {
    GpuLight lights[4];
    float    ambient[4];
    float    fog_color[4];
    float    fog_params[4];
} GpuSceneLighting;
_Static_assert(sizeof(GpuSceneLighting) == 176, "GpuSceneLighting ABI mismatch");

PUSH(AnimPush,
    uint64_t bands_addr;
    uint64_t shapes_addr;
    uint64_t segs_addr;
    uint64_t grads_addr;
    float    viewport[4];
    float    bg_top[4];
    float    bg_bot[4];
    float    hud[4];
);

PUSH(SurfacePush,
    uint64_t verts_addr;
    uint64_t lighting_addr;
    float    mvp[16];
    float    cam_pos_grid[4];
    float    style_params[4];
    uint32_t tex_params[4];
    float    checker_c1[4];
    float    checker_c2[4];
    float    checker_uv[4];
);
_Static_assert(sizeof(SurfacePush_data) == 176, "SurfacePush ABI mismatch");

PUSH(PostPush,
    float    texel[4];
    float    grade[4];
    float    bloom[4];
    float    hud[4];
    float    fx[4];
    uint32_t color_map;
    uint32_t bloom_map;
    uint32_t linear_sampler;
    uint32_t depth_map;
    float    dof[4];
);
_Static_assert(sizeof(PostPush_data) == 112, "PostPush ABI mismatch");

enum {
    BLOOM_LEVELS      = 4,
    MAX_RENDER_SHAPES = 32768,
    MAX_RENDER_BANDS  = 131072,
    MAX_RENDER_SEGS   = 524288,
    MAX_GPU_TEXTURES  = 256
};

typedef struct GpuTexCacheEntry {
    bool     used;
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    Image    img;
} GpuTexCacheEntry;

struct AnimRenderer {
    Gpu     *gpu;
    uint32_t width;
    uint32_t height;

    ShaderId vs_screen;
    ShaderId fs_bg;
    ShaderId vs_surface3d;
    ShaderId fs_surface3d;
    ShaderId vs_band;
    ShaderId fs_band;
    ShaderId fs_band_depth;
    ShaderId fs_accum_blit;
    ShaderId fs_prefilter;
    ShaderId fs_down;
    ShaderId fs_up;
    ShaderId fs_composite;

    Pipe     pipe_bg;
    Pipe     pipe_surface3d;
    Pipe     pipe_band;
    Pipe     pipe_band_depth;
    Pipe     pipe_accum_blit;
    Pipe     pipe_prefilter;
    Pipe     pipe_down;
    Pipe     pipe_up;
    Pipe     pipe_composite;

    Sampler  linear;
    Sampler  nearest;
    Image    hdr_color;
    Image    hdr_msaa;
    Image    depth_msaa;
    bool     has_msaa;
    Image    accum_color;
    Image    depth_buf;
    Image    bloom;
    Image    shot;

    GpuTexCacheEntry tex_cache[MAX_GPU_TEXTURES];

    mu_vm_arena scratch;
    mu_vm_arena shape_scratch;

    AnimFrameStats last_stats;
};

static Image ensure_gpu_texture(AnimRenderer *r, const AnimScene *s, uint32_t tex_id)
{
    uint32_t           i;
    const AnimTexture *src;
    GpuTexCacheEntry  *slot = NULL;
    Upload            *up;
    Image              img;

    if (!r || !s || tex_id == 0u || tex_id > s->texture_count)
        return (Image){ 0 };
    src = &s->textures[tex_id - 1u];
    if (!src->rgba || src->width == 0u || src->height == 0u)
        return (Image){ 0 };

    for (i = 0u; i < MAX_GPU_TEXTURES; ++i)
    {
        if (r->tex_cache[i].used &&
            r->tex_cache[i].hash == src->hash &&
            r->tex_cache[i].width == src->width &&
            r->tex_cache[i].height == src->height)
        {
            return r->tex_cache[i].img;
        }
        if (!r->tex_cache[i].used && !slot)
            slot = &r->tex_cache[i];
    }
    if (!slot)
    {
        /* Evict slot 0 if cache full */
        slot = &r->tex_cache[0];
        if (HANDLE_VALID(slot->img))
            image_destroy(r->gpu, slot->img);
        slot->used = false;
    }

    img = image_create(r->gpu, &(ImageDesc){
        .name    = "anim_user_tex",
        .width   = src->width,
        .height  = src->height,
        .mips    = 1,
        .layers  = 1,
        .format  = FMT_RGBA8_SRGB,
        .usage   = IMG_SAMPLED | IMG_XFER_DST,
        .samples = SC_1,
    });
    up = upload_begin(r->gpu);
    upload_image(up, SUB(img), (DataSpan){ src->rgba, (size_t)src->width * (size_t)src->height * 4u });
    upload_end(r->gpu, up);

    slot->used   = true;
    slot->hash   = src->hash;
    slot->width  = src->width;
    slot->height = src->height;
    slot->img    = img;
    return img;
}

/* ------------------------------------------------------- quadratic lowering */

typedef struct Quad2D {
    float    p0[2];
    float    p1[2];
    float    p2[2];
    float    w0;
    float    w1;
    float    c0[4];
    float    c1[4];
    float    s0;
    float    s1;
    float    z0;
    float    z1;
    uint32_t flags;
} Quad2D;

static float quad2d_approx_len(const float p0[2], const float p1[2], const float p2[2])
{
    float d0x = p1[0] - p0[0], d0y = p1[1] - p0[1];
    float d1x = p2[0] - p1[0], d1y = p2[1] - p1[1];
    float d2x = p2[0] - p0[0], d2y = p2[1] - p0[1];
    float poly = sqrtf(d0x * d0x + d0y * d0y) + sqrtf(d1x * d1x + d1y * d1y);
    float chord = sqrtf(d2x * d2x + d2y * d2y);
    return 0.5f * (poly + chord);
}

static void split_cubic_2d_half(const float p[4][2], float l[4][2], float r[4][2])
{
    float a[2], b[2], c[2], d[2], e[2], m[2];
    int   k;

    for (k = 0; k < 2; ++k)
    {
        a[k] = 0.5f * (p[0][k] + p[1][k]);
        b[k] = 0.5f * (p[1][k] + p[2][k]);
        c[k] = 0.5f * (p[2][k] + p[3][k]);
        d[k] = 0.5f * (a[k] + b[k]);
        e[k] = 0.5f * (b[k] + c[k]);
        m[k] = 0.5f * (d[k] + e[k]);
        l[0][k] = p[0][k]; l[1][k] = a[k]; l[2][k] = d[k]; l[3][k] = m[k];
        r[0][k] = m[k];    r[1][k] = e[k]; r[2][k] = c[k]; r[3][k] = p[3][k];
    }
}

static void lower_cubic_2d_rec(const float p[4][2], int depth,
                               float w0, float w1, float z0, float z1, const float c0[4], const float c1[4],
                               uint32_t flags_start, uint32_t flags_end, uint32_t cap_bits,
                               float *io_arc_s, Quad2D *out, uint32_t *io_n, uint32_t cap)
{
    float ex  = (p[0][0] - 3.0f * p[1][0] + 3.0f * p[2][0] - p[3][0]) * 0.125f;
    float ey  = (p[0][1] - 3.0f * p[1][1] + 3.0f * p[2][1] - p[3][1]) * 0.125f;
    float err = sqrtf(ex * ex + ey * ey);

    if ((err <= 0.35f || depth >= 4) && *io_n < cap)
    {
        Quad2D q;
        float  qlen;
        int    k;

        q.p0[0] = p[0][0]; q.p0[1] = p[0][1];
        q.p2[0] = p[3][0]; q.p2[1] = p[3][1];
        q.p1[0] = (3.0f * (p[1][0] + p[2][0]) - (p[0][0] + p[3][0])) * 0.25f;
        q.p1[1] = (3.0f * (p[1][1] + p[2][1]) - (p[0][1] + p[3][1])) * 0.25f;
        q.w0    = w0;
        q.w1    = w1;
        q.z0    = z0;
        q.z1    = z1;
        for (k = 0; k < 4; ++k)
        {
            q.c0[k] = c0[k];
            q.c1[k] = c1[k];
        }
        qlen     = quad2d_approx_len(q.p0, q.p1, q.p2);
        q.s0     = *io_arc_s;
        q.s1     = *io_arc_s + qlen;
        *io_arc_s = q.s1;
        q.flags  = flags_start | flags_end | (cap_bits << 2u);
        out[(*io_n)++] = q;
        return;
    }

    if (depth < 4)
    {
        float    l[4][2], r[4][2];
        float    wm = 0.5f * (w0 + w1);
        float    zm = 0.5f * (z0 + z1);
        float    cm[4];
        int      k;

        for (k = 0; k < 4; ++k)
            cm[k] = 0.5f * (c0[k] + c1[k]);
        split_cubic_2d_half(p, l, r);
        lower_cubic_2d_rec(l, depth + 1, w0, wm, z0, zm, c0, cm, flags_start, 0u, cap_bits, io_arc_s, out, io_n, cap);
        lower_cubic_2d_rec(r, depth + 1, wm, w1, zm, z1, cm, c1, 0u, flags_end, cap_bits, io_arc_s, out, io_n, cap);
    }
}

static void split_quad_y_monotonic(Quad2D q, Quad2D *out, uint32_t *io_n, uint32_t cap)
{
    float denom = q.p0[1] - 2.0f * q.p1[1] + q.p2[1];
    float t_ext = (fabsf(denom) > 1e-6f) ? ((q.p0[1] - q.p1[1]) / denom) : -1.0f;

    if (t_ext > 1e-4f && t_ext < 1.0f - 1e-4f && *io_n + 2u <= cap)
    {
        float  u  = 1.0f - t_ext;
        float  a0 = u * q.p0[0] + t_ext * q.p1[0];
        float  a1 = u * q.p0[1] + t_ext * q.p1[1];
        float  b0 = u * q.p1[0] + t_ext * q.p2[0];
        float  b1 = u * q.p1[1] + t_ext * q.p2[1];
        float  m0 = u * a0 + t_ext * b0;
        float  m1 = u * a1 + t_ext * b1;
        Quad2D q0 = q;
        Quad2D q1 = q;

        q0.p1[0] = a0; q0.p1[1] = a1; q0.p2[0] = m0; q0.p2[1] = m1;
        q1.p0[0] = m0; q1.p0[1] = m1; q1.p1[0] = b0; q1.p1[1] = b1;
        out[(*io_n)++] = q0;
        out[(*io_n)++] = q1;
        return;
    }
    if (*io_n < cap)
        out[(*io_n)++] = q;
}

/* ----------------------------------------------------- renderer lifecycle */

static void create_render_targets(AnimRenderer *r, uint32_t w, uint32_t h)
{
    r->width  = w;
    r->height = h;

    r->hdr_color = image_create(r->gpu, &(ImageDesc){
        .name    = "anim_hdr",
        .width   = w,
        .height  = h,
        .mips    = 1,
        .layers  = 1,
        .format  = FMT_RGBA16F,
        .usage   = IMG_ATTACH | IMG_SAMPLED,
        .samples = SC_1,
    });
    r->accum_color = image_create(r->gpu, &(ImageDesc){
        .name    = "anim_accum",
        .width   = w,
        .height  = h,
        .mips    = 1,
        .layers  = 1,
        .format  = FMT_RGBA16F,
        .usage   = IMG_ATTACH | IMG_SAMPLED,
        .samples = SC_1,
    });
    r->depth_buf = image_depth(r->gpu, w, h);
    r->has_msaa  = (gpu_info(r->gpu).sample_counts & SC_4) != 0u;
    if (r->has_msaa)
    {
        r->hdr_msaa = image_create(r->gpu, &(ImageDesc){
            .name    = "anim_hdr_msaa",
            .width   = w,
            .height  = h,
            .mips    = 1,
            .layers  = 1,
            .format  = FMT_RGBA16F,
            .usage   = IMG_ATTACH,
            .samples = SC_4,
        });
        r->depth_msaa = image_create(r->gpu, &(ImageDesc){
            .name    = "anim_depth_msaa",
            .width   = w,
            .height  = h,
            .mips    = 1,
            .layers  = 1,
            .format  = FMT_D32F,
            .usage   = IMG_ATTACH,
            .samples = SC_4,
        });
    }
    r->bloom = image_create(r->gpu, &(ImageDesc){
        .name    = "anim_bloom",
        .width   = MU_MAX(w >> 1, 1u),
        .height  = MU_MAX(h >> 1, 1u),
        .mips    = BLOOM_LEVELS,
        .layers  = 1,
        .format  = FMT_RGBA16F,
        .usage   = IMG_ATTACH | IMG_SAMPLED,
        .samples = SC_1,
    });
    r->shot = image_create(r->gpu, &(ImageDesc){
        .name    = "anim_shot",
        .width   = w,
        .height  = h,
        .mips    = 1,
        .layers  = 1,
        .format  = FMT_RGBA8_UNORM,
        .usage   = IMG_ATTACH | IMG_SAMPLED | IMG_XFER_SRC,
        .samples = SC_1,
    });
}

static void destroy_render_targets(AnimRenderer *r)
{
    if (HANDLE_VALID(r->hdr_color))   image_destroy(r->gpu, r->hdr_color);
    if (HANDLE_VALID(r->hdr_msaa))    image_destroy(r->gpu, r->hdr_msaa);
    if (HANDLE_VALID(r->depth_msaa))  image_destroy(r->gpu, r->depth_msaa);
    if (HANDLE_VALID(r->accum_color)) image_destroy(r->gpu, r->accum_color);
    if (HANDLE_VALID(r->depth_buf))   image_destroy(r->gpu, r->depth_buf);
    if (HANDLE_VALID(r->bloom))       image_destroy(r->gpu, r->bloom);
    if (HANDLE_VALID(r->shot))        image_destroy(r->gpu, r->shot);
    r->hdr_color   = IMAGE_NIL;
    r->hdr_msaa    = IMAGE_NIL;
    r->depth_msaa  = IMAGE_NIL;
    r->accum_color = IMAGE_NIL;
    r->depth_buf   = IMAGE_NIL;
    r->bloom       = IMAGE_NIL;
    r->shot        = IMAGE_NIL;
}

AnimRenderOpts anim_render_opts_default(uint32_t width, uint32_t height)
{
    return (AnimRenderOpts){
        .width               = width ? width : ANIM_FRAME_PX_W,
        .height              = height ? height : ANIM_FRAME_PX_H,
        .fps                 = ANIM_DEFAULT_FPS,
        .bloom               = true,
        .bloom_strength      = 0.38f,
        .bloom_threshold     = 1.0f,
        .bloom_knee          = 0.45f,
        .exposure            = 1.0f,
        .saturation          = 1.02f,
        .vignette_radius     = 0.0f,
        .dof_aperture_px     = 0.0f,
        .dof_focus_dist      = 8.0f,
        .dof_focus_range     = 1.5f,
        .motion_blur_samples = 1u,
        .shutter_angle       = 0.5f,
        .transparent_bg      = false,
        .show_hud            = false,
        .hud_progress        = 0.0f,
        .hud_playing         = true,
        .zoom_inset_enable   = false,
        .zoom_inset_cam      = anim_camera_default(),
        .zoom_inset_rect     = { 0.64f, 0.04f, 0.96f, 0.36f },
        .zoom_inset_border   = ANIM_YELLOW,
    };
}

AnimRenderOpts anim_render_opts_preset(AnimResolutionPreset preset)
{
    switch (preset)
    {
    case ANIM_RES_1080P:
    {
        AnimRenderOpts o = anim_render_opts_default(1920u, 1080u);
        o.fps = 60u;
        return o;
    }
    case ANIM_RES_4K:
    {
        AnimRenderOpts o = anim_render_opts_default(3840u, 2160u);
        o.fps = 60u;
        return o;
    }
    case ANIM_RES_VERTICAL_1080X1920:
    {
        AnimRenderOpts o = anim_render_opts_default(1080u, 1920u);
        o.fps = 60u;
        return o;
    }
    case ANIM_RES_SQUARE_1080:
    {
        AnimRenderOpts o = anim_render_opts_default(1080u, 1080u);
        o.fps = 60u;
        return o;
    }
    case ANIM_RES_720P:
    default:
        return anim_render_opts_default(1280u, 720u);
    }
}

static bool anim_file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

static ShaderId load_anim_shader(Gpu *gpu, const char *entry, ShaderStage stage)
{
    const char *env_dir = getenv("MU_ANIM_SHADER_DIR");
    const char *dirs[4];
    uint32_t    n = 0u, i;
    char        path[1024];

    if (env_dir && env_dir[0])
        dirs[n++] = env_dir;
    dirs[n++] = "shaders";
#ifdef MU_ANIM_SHADER_DIR
    dirs[n++] = MU_ANIM_SHADER_DIR;
#endif
    dirs[n++] = "/usr/local/share/mu_anim/shaders";

    for (i = 0u; i < n; ++i)
    {
        snprintf(path, sizeof path, "%s/anim.slang", dirs[i]);
        if (anim_file_exists(path))
            return shader_load(gpu, &(ShaderDesc){ .path = path, .entry = entry, .stage = stage });
        snprintf(path, sizeof path, "%s/anim.%s.spv", dirs[i], entry);
        if (anim_file_exists(path))
            return shader_load(gpu, &(ShaderDesc){ .path = path, .entry = entry, .stage = stage });
    }
    return shader_load(gpu, &(ShaderDesc){ .path = "shaders/anim.slang", .entry = entry, .stage = stage });
}

AnimRenderer *anim_renderer_create(Gpu *gpu, uint32_t width, uint32_t height)
{
    AnimRenderer *r = (AnimRenderer *)calloc(1, sizeof *r);
    uint32_t      w = width ? width : ANIM_FRAME_PX_W;
    uint32_t      h = height ? height : ANIM_FRAME_PX_H;

    MU_ASSERT(r != NULL && gpu != NULL);
    r->gpu = gpu;
    mu_vm_arena_init(&r->scratch, MU_MB(128), MU_MB(4));
    mu_vm_arena_init(&r->shape_scratch, MU_MB(64), MU_MB(2));

    r->vs_screen     = load_anim_shader(gpu, "vs_screen",          STAGE_VERTEX);
    r->fs_bg         = load_anim_shader(gpu, "fs_bg",              STAGE_FRAGMENT);
    r->vs_surface3d  = load_anim_shader(gpu, "vs_surface3d",       STAGE_VERTEX);
    r->fs_surface3d  = load_anim_shader(gpu, "fs_surface3d",       STAGE_FRAGMENT);
    r->vs_band       = load_anim_shader(gpu, "vs_band",            STAGE_VERTEX);
    r->fs_band       = load_anim_shader(gpu, "fs_band",            STAGE_FRAGMENT);
    r->fs_band_depth = load_anim_shader(gpu, "fs_band_depth",      STAGE_FRAGMENT);
    r->fs_accum_blit = load_anim_shader(gpu, "fs_accum_blit",      STAGE_FRAGMENT);
    r->fs_prefilter  = load_anim_shader(gpu, "fs_bloom_prefilter", STAGE_FRAGMENT);
    r->fs_down       = load_anim_shader(gpu, "fs_bloom_down",      STAGE_FRAGMENT);
    r->fs_up         = load_anim_shader(gpu, "fs_bloom_up",        STAGE_FRAGMENT);
    r->fs_composite  = load_anim_shader(gpu, "fs_composite",       STAGE_FRAGMENT);

    r->pipe_bg         = pipe_create(gpu, &(PipeDesc){ .name = "anim_bg",         .vertex = r->vs_screen,    .fragment = r->fs_bg });
    r->pipe_surface3d  = pipe_create(gpu, &(PipeDesc){ .name = "anim_surface3d",  .vertex = r->vs_surface3d, .fragment = r->fs_surface3d });
    r->pipe_band       = pipe_create(gpu, &(PipeDesc){ .name = "anim_band",       .vertex = r->vs_band,      .fragment = r->fs_band });
    r->pipe_band_depth = pipe_create(gpu, &(PipeDesc){ .name = "anim_band_depth", .vertex = r->vs_band,      .fragment = r->fs_band_depth });
    r->pipe_accum_blit = pipe_create(gpu, &(PipeDesc){ .name = "anim_accum_blit", .vertex = r->vs_screen,    .fragment = r->fs_accum_blit });
    r->pipe_prefilter  = pipe_create(gpu, &(PipeDesc){ .name = "anim_prefilter",  .vertex = r->vs_screen,    .fragment = r->fs_prefilter });
    r->pipe_down       = pipe_create(gpu, &(PipeDesc){ .name = "anim_down",       .vertex = r->vs_screen,    .fragment = r->fs_down });
    r->pipe_up         = pipe_create(gpu, &(PipeDesc){ .name = "anim_up",         .vertex = r->vs_screen,    .fragment = r->fs_up });
    r->pipe_composite  = pipe_create(gpu, &(PipeDesc){ .name = "anim_composite",  .vertex = r->vs_screen,    .fragment = r->fs_composite });

    r->linear = sampler_linear_clamp(gpu);
    r->nearest = sampler_nearest_clamp(gpu);
    create_render_targets(r, w, h);
    return r;
}

void anim_renderer_destroy(AnimRenderer *r)
{
    uint32_t i;
    if (!r)
        return;
    for (i = 0u; i < MAX_GPU_TEXTURES; ++i)
    {
        if (r->tex_cache[i].used && HANDLE_VALID(r->tex_cache[i].img))
            image_destroy(r->gpu, r->tex_cache[i].img);
    }
    destroy_render_targets(r);
    if (HANDLE_VALID(r->linear))          sampler_destroy(r->gpu, r->linear);
    if (HANDLE_VALID(r->nearest))         sampler_destroy(r->gpu, r->nearest);
    if (HANDLE_VALID(r->pipe_bg))         pipe_destroy(r->gpu, r->pipe_bg);
    if (HANDLE_VALID(r->pipe_surface3d))  pipe_destroy(r->gpu, r->pipe_surface3d);
    if (HANDLE_VALID(r->pipe_band))       pipe_destroy(r->gpu, r->pipe_band);
    if (HANDLE_VALID(r->pipe_band_depth)) pipe_destroy(r->gpu, r->pipe_band_depth);
    if (HANDLE_VALID(r->pipe_accum_blit)) pipe_destroy(r->gpu, r->pipe_accum_blit);
    if (HANDLE_VALID(r->pipe_prefilter))  pipe_destroy(r->gpu, r->pipe_prefilter);
    if (HANDLE_VALID(r->pipe_down))       pipe_destroy(r->gpu, r->pipe_down);
    if (HANDLE_VALID(r->pipe_up))         pipe_destroy(r->gpu, r->pipe_up);
    if (HANDLE_VALID(r->pipe_composite))  pipe_destroy(r->gpu, r->pipe_composite);
    mu_vm_arena_destroy(&r->scratch);
    mu_vm_arena_destroy(&r->shape_scratch);
    free(r);
}

/* ----------------------------------------------------- frame graph & scene */

typedef struct VisibleLeaf {
    uint32_t mob_idx;
    float    eff_opacity;
    float    sort_key;
} VisibleLeaf;

static void collect_visible_node(const AnimScene *s, uint32_t idx, bool parent_vis, float parent_op,
                                 const float view[16],
                                 VisibleLeaf *out_bands, uint32_t *io_nb, uint32_t cap_b,
                                 VisibleLeaf *out_surfs, uint32_t *io_ns, uint32_t cap_s)
{
    const VMobHot *h   = &s->hot[idx];
    bool           vis = parent_vis && h->visible;
    float          op  = parent_op * h->opacity;
    uint32_t       c;

    if (!h->alive || !vis || op <= 1e-4f)
        return;

    if (h->subpath_count > 0u && *io_nb < cap_b)
    {
        float p[3] = { h->world_mat[12], h->world_mat[13], h->world_mat[14] };
        float v[3];
        float cam_z;
        mu_mat4_transform_point(v, view, p);
        cam_z = -v[2];
        out_bands[(*io_nb)++] = (VisibleLeaf){
            .mob_idx     = idx,
            .eff_opacity = op,
            .sort_key    = h->z_index * 1000.0f - cam_z,
        };
    }

    if (h->surf_vert_count > 0u && h->surf_idx_count > 0u && *io_ns < cap_s)
    {
        float p[3] = { h->world_mat[12], h->world_mat[13], h->world_mat[14] };
        float v[3];
        mu_mat4_transform_point(v, view, p);
        out_surfs[(*io_ns)++] = (VisibleLeaf){
            .mob_idx     = idx,
            .eff_opacity = op,
            .sort_key    = h->z_index * 1000.0f + v[2],
        };
    }

    c = h->first_child;
    while (c != 0u)
    {
        collect_visible_node(s, c, vis, op, view, out_bands, io_nb, cap_b, out_surfs, io_ns, cap_s);
        c = s->hot[c].next_sibling;
    }
}

static bool project_world_to_px_rect_z(const float mvp[16], const float view[16], AnimVec3 wp,
                                       float vx, float vy, float vw, float vh,
                                       float out_px[2], float *out_cam_z, float *out_z_ndc)
{
    float x  = mvp[0] * wp.x + mvp[4] * wp.y + mvp[8]  * wp.z + mvp[12];
    float y  = mvp[1] * wp.x + mvp[5] * wp.y + mvp[9]  * wp.z + mvp[13];
    float z  = mvp[2] * wp.x + mvp[6] * wp.y + mvp[10] * wp.z + mvp[14];
    float w  = mvp[3] * wp.x + mvp[7] * wp.y + mvp[11] * wp.z + mvp[15];
    float vz = -(view[2] * wp.x + view[6] * wp.y + view[10] * wp.z + view[14]);

    if (w <= 1e-4f)
        return false;
    out_px[0] = vx + (0.5f + 0.5f * (x / w)) * vw;
    out_px[1] = vy + (0.5f - 0.5f * (y / w)) * vh;
    if (out_cam_z)
        *out_cam_z = MU_MAX(vz, 0.1f);
    if (out_z_ndc)
        *out_z_ndc = MU_CLAMP(z / w, 0.0f, 1.0f);
    return true;
}

static bool project_world_to_px_rect(const float mvp[16], const float view[16], AnimVec3 wp,
                                     float vx, float vy, float vw, float vh,
                                     float out_px[2], float *out_cam_z)
{
    return project_world_to_px_rect_z(mvp, view, wp, vx, vy, vw, vh, out_px, out_cam_z, NULL);
}

static void emit_bands_for_quads(const Quad2D *quads, uint32_t qcount, uint32_t shape_idx,
                                 bool is_stroke, bool even_odd, float extra_pad, float vp_w, float vp_h,
                                 GpuAnimBand *bands, uint32_t *io_band_count,
                                 GpuAnimQuadSeg *segs, uint32_t *io_seg_count)
{
    float    s_min_x = 1e30f, s_min_y = 1e30f, s_max_x = -1e30f, s_max_y = -1e30f;
    uint32_t num_bands, b, qi;
    float    band_h, pad;

    if (qcount == 0u)
        return;

    for (qi = 0u; qi < qcount; ++qi)
    {
        const Quad2D *q = &quads[qi];
        float         hw = is_stroke ? MU_MAX(q->w0, q->w1) : 0.0f;
        s_min_x = MU_MIN(s_min_x, MU_MIN(q->p0[0], MU_MIN(q->p1[0], q->p2[0])) - hw);
        s_min_y = MU_MIN(s_min_y, MU_MIN(q->p0[1], MU_MIN(q->p1[1], q->p2[1])) - hw);
        s_max_x = MU_MAX(s_max_x, MU_MAX(q->p0[0], MU_MAX(q->p1[0], q->p2[0])) + hw);
        s_max_y = MU_MAX(s_max_y, MU_MAX(q->p0[1], MU_MAX(q->p1[1], q->p2[1])) + hw);
    }

    pad     = 2.0f + extra_pad;
    s_min_x = MU_MAX(s_min_x - pad, 0.0f);
    s_min_y = MU_MAX(s_min_y - pad, 0.0f);
    s_max_x = MU_MIN(s_max_x + pad, vp_w);
    s_max_y = MU_MIN(s_max_y + pad, vp_h);
    if (s_max_x <= s_min_x || s_max_y <= s_min_y)
        return;

    num_bands = (qcount <= 8u) ? 1u : (uint32_t)MU_CLAMP((int)ceilf((s_max_y - s_min_y) / 16.0f), 1, ANIM_BANDS_MAX);
    band_h    = (s_max_y - s_min_y) / (float)num_bands;

    for (b = 0u; b < num_bands; ++b)
    {
        float    by0     = s_min_y + (float)b * band_h;
        float    by1     = (b + 1u == num_bands) ? s_max_y : (s_min_y + (float)(b + 1u) * band_h);
        float    bx0     = 1e30f;
        float    bx1     = -1e30f;
        uint32_t seg_off = *io_seg_count;
        uint32_t seg_cnt = 0u;

        for (qi = 0u; qi < qcount; ++qi)
        {
            const Quad2D   *q   = &quads[qi];
            float           hw  = is_stroke ? (MU_MAX(q->w0, q->w1) + pad) : pad;
            float           qy0 = is_stroke ? (MU_MIN(q->p0[1], MU_MIN(q->p1[1], q->p2[1])) - hw)
                                            : (MU_MIN(q->p0[1], q->p2[1]) - hw);
            float           qy1 = is_stroke ? (MU_MAX(q->p0[1], MU_MAX(q->p1[1], q->p2[1])) + hw)
                                            : (MU_MAX(q->p0[1], q->p2[1]) + hw);
            float           qx0, qx1;
            GpuAnimQuadSeg *gs;

            if (qy1 < by0 || qy0 > by1)
                continue;
            if (*io_seg_count >= MAX_RENDER_SEGS)
            {
                anim__set_error("anim_render: exceeded MAX_RENDER_SEGS (%u)", MAX_RENDER_SEGS);
                break;
            }

            qx0 = MU_MIN(q->p0[0], MU_MIN(q->p1[0], q->p2[0])) - hw;
            qx1 = MU_MAX(q->p0[0], MU_MAX(q->p1[0], q->p2[0])) + hw;
            bx0 = MU_MIN(bx0, qx0);
            bx1 = MU_MAX(bx1, qx1);

            gs           = &segs[(*io_seg_count)++];
            gs->p0_p1[0] = q->p0[0]; gs->p0_p1[1] = q->p0[1];
            gs->p0_p1[2] = q->p1[0]; gs->p0_p1[3] = q->p1[1];
            gs->p2_w[0]  = q->p2[0]; gs->p2_w[1]  = q->p2[1];
            gs->p2_w[2]  = q->w0;    gs->p2_w[3]  = q->w1;
            memcpy(gs->c0, q->c0, sizeof gs->c0);
            memcpy(gs->c1, q->c1, sizeof gs->c1);
            gs->arc_len[0] = q->s0;
            gs->arc_len[1] = q->s1;
            gs->arc_len[2] = q->z0;
            gs->arc_len[3] = q->z1;
            gs->meta[0]  = q->flags;
            gs->meta[1]  = 0u;
            gs->meta[2]  = 0u;
            gs->meta[3]  = 0u;
            seg_cnt++;
        }

        if (seg_cnt > 0u && *io_band_count < MAX_RENDER_BANDS)
        {
            bx0 = MU_MAX(bx0, 0.0f);
            bx1 = MU_MIN(bx1, vp_w);
            if (bx1 > bx0)
            {
                GpuAnimBand *gb = &bands[(*io_band_count)++];
                gb->bounds[0] = bx0; gb->bounds[1] = by0;
                gb->bounds[2] = bx1; gb->bounds[3] = by1;
                gb->meta[0]   = seg_off;
                gb->meta[1]   = seg_cnt;
                gb->meta[2]   = shape_idx;
                gb->meta[3]   = (is_stroke ? 1u : 0u) | (even_odd ? 2u : 0u);
            }
        }
    }
}

typedef struct PreparedSurfaceDraw {
    uint32_t  vert_off;
    uint32_t  vert_cnt;
    float     opacity;
    float     glow;
    float     grid_density;
    float     grid_alpha;
    float     roughness;
    uint32_t  tex_id;
    bool      unlit;
    bool      tex_nearest;
    bool      checker;
    AnimColor checker_c1;
    AnimColor checker_c2;
    uint32_t  checker_u;
    uint32_t  checker_v;
} PreparedSurfaceDraw;

typedef struct VpDrawCfg {
    bool      is_sub_vp;
    bool      draw_bg;
    AnimColor border;
    float     border_width;
} VpDrawCfg;

static void render_scene_camera_to_hdr(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts,
                                       Image hdr_target, const AnimCamera *cam,
                                       float vx, float vy, float vw, float vh, const VpDrawCfg *vp_cfg)
{
    bool           is_inset     = vp_cfg && vp_cfg->is_sub_vp;
    bool           draw_bg      = !vp_cfg || vp_cfg->draw_bg;
    AnimColor      vp_border    = vp_cfg ? vp_cfg->border : anim_rgba(0.0f, 0.0f, 0.0f, 0.0f);
    float          vp_bw        = (vp_cfg && vp_cfg->border_width > 0.0f) ? vp_cfg->border_width : 2.0f;
    uint64_t       t_begin_ns   = mu_clock_ns();
    uint32_t       w            = r->width;
    uint32_t       h            = r->height;
    float          vp_w         = (float)w;
    float          vp_h         = (float)h;
    float          aspect       = vw / MU_MAX(vh, 1.0f);
    float          vp_mat[16];
    float          view_mat[16];
    float          ref_dist;
    VisibleLeaf   *leaves;
    VisibleLeaf   *surf_leaves;
    float         *sort_keys;
    uint32_t      *sort_idx;
    uint32_t       leaf_count   = 0u;
    uint32_t       surf_count   = 0u;
    GpuAnimBand   *bands, *bg_bands;
    GpuAnimShape  *shapes;
    GpuAnimQuadSeg *segs;
    GpuAnimGrad   *grads;
    uint32_t       band_count       = 0u;
    uint32_t       depth_band_count = 0u;
    uint32_t       world_band_start = 0u;
    uint32_t       world_band_count = 0u;
    uint32_t       hud_band_count   = 0u;
    uint32_t       shape_count      = 0u;
    uint32_t       seg_count        = 0u;
    uint32_t       grad_count       = 0u;
    uint32_t       culled_count     = 0u;
    uint32_t       surf_tris        = 0u;
    uint32_t       i, pass_kind;
    Pass          *pass;
    AnimPush       apush;
    Range          r_bands, r_shapes, r_segs, r_grads;
    AnimCamera     hud_cam = anim_camera_default();
    float          hud_vp[16], hud_view[16], hud_ref_dist;

    PreparedSurfaceDraw *surf_draws = NULL;
    uint32_t             surf_draw_n = 0u;
    Range                r_surf_verts = RANGE_NIL;
    uint32_t             total_surf_verts = 0u;

    bool          use_hud_dof_pin = (!is_inset && opts->dof_aperture_px > 0.01f);
    float         hud_focus_ndc   = 0.9862f;

    if (use_hud_dof_pin)
    {
        float fdist = (opts->dof_focus_dist > 0.05f) ? opts->dof_focus_dist : 8.0f;
        float z_persp = (200.0f - 20.0f / MU_CLAMP(fdist, 0.15f, 199.0f)) / 199.9f;
        float z_ortho = (MU_CLAMP(fdist, 0.15f, 199.0f) - 0.1f) / 199.9f;
        hud_focus_ndc = MU_CLAMP(z_persp + (z_ortho - z_persp) * cam->ortho_blend, 0.0f, 1.0f);
    }

    mu_vm_arena_reset(&r->scratch);
    leaves      = (VisibleLeaf *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_SHAPES * sizeof *leaves, 16);
    surf_leaves = (VisibleLeaf *)mu_vm_arena_alloc(&r->scratch, 2048u * sizeof *surf_leaves, 16);
    sort_keys   = (float *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_SHAPES * sizeof *sort_keys, 16);
    sort_idx    = (uint32_t *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_SHAPES * sizeof *sort_idx, 16);
    bands       = (GpuAnimBand *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_BANDS * sizeof *bands, 16);
    bg_bands    = (GpuAnimBand *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_BANDS * sizeof *bg_bands, 16);
    shapes      = (GpuAnimShape *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_SHAPES * sizeof *shapes, 16);
    segs        = (GpuAnimQuadSeg *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_SEGS * sizeof *segs, 16);
    grads       = (GpuAnimGrad *)mu_vm_arena_alloc(&r->scratch, MAX_RENDER_SHAPES * sizeof *grads, 16);

    anim__sync_world_mats(scene);
    anim_camera_view_proj(cam, aspect, vp_mat, view_mat);
    anim_camera_view_proj(&hud_cam, aspect, hud_vp, hud_view);
    {
        float fov_rad  = cam->fov_deg * (ANIM_PI / 180.0f) / MU_MAX(cam->zoom, 0.05f);
        float hfov_rad = hud_cam.fov_deg * (ANIM_PI / 180.0f);
        ref_dist       = (cam->frame_height * 0.5f) / tanf(0.5f * fov_rad);
        hud_ref_dist   = (hud_cam.frame_height * 0.5f) / tanf(0.5f * hfov_rad);
    }

    for (i = 1u; i < scene->mob_cap; ++i)
    {
        if (scene->hot[i].alive && scene->hot[i].parent == 0u && scene->hot[i].in_scene)
            collect_visible_node(scene, i, true, 1.0f, view_mat,
                                 leaves, &leaf_count, MAX_RENDER_SHAPES,
                                 surf_leaves, &surf_count, 2048u);
    }

    /* Prepare 3D lit surfaces */
    if (surf_count > 0u)
    {
        for (i = 0u; i < surf_count; ++i)
            total_surf_verts += scene->hot[surf_leaves[i].mob_idx].surf_idx_count;

        if (total_surf_verts > 0u)
        {
            GpuSurfaceVert *sverts = (GpuSurfaceVert *)mu_vm_arena_alloc(&r->scratch, total_surf_verts * sizeof *sverts, 16);
            uint32_t        sv_cursor = 0u;
            surf_draws = (PreparedSurfaceDraw *)mu_vm_arena_alloc(&r->scratch, surf_count * sizeof *surf_draws, 16);

            for (i = 0u; i < surf_count; ++i)
            {
                const VisibleLeaf *lf  = &surf_leaves[i];
                const VMobHot     *hm  = &scene->hot[lf->mob_idx];
                const VMobCold    *cm  = &scene->cold[lf->mob_idx];
                float              inv_m[16];
                uint32_t           k;
                uint32_t           vstart = sv_cursor;

                mu_mat4_inverse(hm->world_mat, inv_m);
                for (k = 0u; k < hm->surf_idx_count; ++k)
                {
                    uint32_t           vi  = scene->surf_indices[hm->surf_idx_offset + k];
                    const SurfaceVert *src = &scene->surf_verts[hm->surf_vert_offset + vi];
                    float              pin[3]  = { src->pos.x, src->pos.y, src->pos.z };
                    float              pout[3];
                    float              nx, ny, nz, nlen;
                    GpuSurfaceVert    *dst = &sverts[sv_cursor++];

                    mu_mat4_transform_point(pout, hm->world_mat, pin);
                    /* Inverse-transpose upper 3x3 for normal */
                    nx = inv_m[0] * src->norm.x + inv_m[1] * src->norm.y + inv_m[2]  * src->norm.z;
                    ny = inv_m[4] * src->norm.x + inv_m[5] * src->norm.y + inv_m[6]  * src->norm.z;
                    nz = inv_m[8] * src->norm.x + inv_m[9] * src->norm.y + inv_m[10] * src->norm.z;
                    nlen = sqrtf(nx * nx + ny * ny + nz * nz);
                    if (nlen > 1e-6f) { nx /= nlen; ny /= nlen; nz /= nlen; }

                    dst->pos_u[0] = pout[0]; dst->pos_u[1] = pout[1]; dst->pos_u[2] = pout[2]; dst->pos_u[3] = src->u;
                    dst->norm_v[0] = nx;     dst->norm_v[1] = ny;     dst->norm_v[2] = nz;     dst->norm_v[3] = src->v;
                    if (cm->surf_use_fill_color)
                    {
                        dst->color[0] = src->color.r * hm->fill_color.r;
                        dst->color[1] = src->color.g * hm->fill_color.g;
                        dst->color[2] = src->color.b * hm->fill_color.b;
                        dst->color[3] = src->color.a * hm->fill_color.a;
                    }
                    else
                    {
                        dst->color[0] = src->color.r; dst->color[1] = src->color.g;
                        dst->color[2] = src->color.b; dst->color[3] = src->color.a;
                    }
                }

                surf_draws[surf_draw_n++] = (PreparedSurfaceDraw){
                    .vert_off     = vstart,
                    .vert_cnt     = hm->surf_idx_count,
                    .opacity      = lf->eff_opacity,
                    .glow         = hm->glow,
                    .grid_density = cm->surf_grid_density,
                    .grid_alpha   = cm->surf_grid_alpha,
                    .roughness    = cm->surf_roughness,
                    .tex_id       = cm->surf_tex_id,
                    .unlit        = cm->surf_unlit,
                    .tex_nearest  = cm->surf_tex_nearest,
                    .checker      = cm->surf_checker,
                    .checker_c1   = cm->surf_checker_c1,
                    .checker_c2   = cm->surf_checker_c2,
                    .checker_u    = cm->surf_checker_u,
                    .checker_v    = cm->surf_checker_v,
                };
                surf_tris += hm->surf_idx_count / 3u;
            }

            r_surf_verts = arena_alloc(r->gpu, ARENA_TRANSIENT, (uint64_t)total_surf_verts * sizeof(GpuSurfaceVert), 16);
            memcpy((uint8_t *)buffer_info(r->gpu, r_surf_verts.buffer).mapped + r_surf_verts.offset,
                   sverts, (size_t)total_surf_verts * sizeof(GpuSurfaceVert));
        }
    }

    for (i = 0u; i < leaf_count; ++i)
    {
        sort_keys[i] = leaves[i].sort_key;
        sort_idx[i]  = i;
    }
    if (leaf_count > 1u)
        mu_radix_sort_f32(sort_keys, sort_idx, leaf_count);

    for (pass_kind = 0u; pass_kind < 3u; ++pass_kind)
    {
        const float      *cur_vp       = (pass_kind == 2u) ? hud_vp : vp_mat;
        const float      *cur_view     = (pass_kind == 2u) ? hud_view : view_mat;
        const AnimCamera *cur_cam      = (pass_kind == 2u) ? &hud_cam : cam;
        float             cur_ref      = (pass_kind == 2u) ? hud_ref_dist : ref_dist;
        uint32_t          b_start      = band_count;
        uint32_t          grp_b_start  = band_count;
        uint32_t          grp_parent   = UINT32_MAX;
        uint32_t          bg_band_cnt  = 0u;

        if (pass_kind == 2u && is_inset)
            continue;

        for (i = 0u; i < leaf_count && shape_count < MAX_RENDER_SHAPES; ++i)
        {
            const VisibleLeaf *lf         = &leaves[sort_idx[i]];
            const VMobHot     *hm         = &scene->hot[lf->mob_idx];
            const VMobCold    *cm         = &scene->cold[lf->mob_idx];
            bool               is_fixed   = false;
            bool               is_dtest   = false;
            {
                uint32_t anc = lf->mob_idx;
                while (anc != 0u)
                {
                    if (scene->hot[anc].fixed_in_frame) is_fixed = true;
                    if (scene->hot[anc].depth_tested)   is_dtest = true;
                    anc = scene->hot[anc].parent;
                }
            }
            if (pass_kind == 0u && (is_fixed || !is_dtest)) continue;
            if (pass_kind == 1u && (is_fixed || is_dtest))  continue;
            if (pass_kind == 2u && !is_fixed)               continue;

            if (hm->parent != grp_parent)
            {
                if (bg_band_cnt > 0u && band_count + bg_band_cnt <= MAX_RENDER_BANDS)
                {
                    uint32_t fg_cnt = band_count - grp_b_start;
                    if (fg_cnt > 0u)
                        memmove(&bands[grp_b_start + bg_band_cnt], &bands[grp_b_start], (size_t)fg_cnt * sizeof(GpuAnimBand));
                    memcpy(&bands[grp_b_start], bg_bands, (size_t)bg_band_cnt * sizeof(GpuAnimBand));
                    band_count += bg_band_cnt;
                }
                grp_b_start = band_count;
                grp_parent  = hm->parent;
                bg_band_cnt = 0u;
            }

            const SubpathMeta *subs       = &scene->subpaths[hm->subpath_offset];
            const AnimCubic   *cubs       = scene->cubics;
            uint32_t           sub_n      = hm->subpath_count;
            uint32_t           total_c    = 0u;
            uint32_t           raw_n      = 0u;
            uint32_t           mono_n     = 0u;
            uint32_t           raw_cap, mono_cap;
            Quad2D            *raw_quads, *mono_quads;
            uint32_t           si, ci, qi;
            bool               has_fill   = (hm->fill_color.a * lf->eff_opacity) > 1e-4f || (cm->fill_grad_mode > 0u && cm->fill_grad_c2.a * lf->eff_opacity > 1e-4f);
            bool               has_stroke = (hm->stroke_color.a * lf->eff_opacity) > 1e-4f && hm->stroke_width > 1e-3f;
            bool               has_bg_st  = (hm->bg_stroke_width > 1e-3f) && (cm->bg_stroke_color.a * lf->eff_opacity > 1e-4f);
            float              box_min_x  = 1e30f, box_min_y = 1e30f, box_max_x = -1e30f, box_max_y = -1e30f;
            float              max_hw_px  = 0.0f;
            float              z_ndc_sum  = 0.0f;
            uint32_t           z_ndc_cnt  = 0u;
            uint32_t           shape_idx;
            GpuAnimShape      *gs;
            uint32_t           c_seen     = 0u;
            float              bg_pad_px  = 0.0f;

            if (!has_fill && !has_stroke)
                continue;

            for (si = 0u; si < sub_n; ++si)
                total_c += subs[si].cubic_count;
            if (total_c == 0u)
                continue;

            mu_vm_arena_reset(&r->shape_scratch);

            if (hm->trim_start > 1e-4f || hm->trim_end < 1.0f - 1e-4f)
            {
                AnimCubic   *trim_cubics = (AnimCubic *)mu_vm_arena_alloc(&r->shape_scratch, total_c * 2u * sizeof *trim_cubics, 16);
                SubpathMeta *trim_subs   = (SubpathMeta *)mu_vm_arena_alloc(&r->shape_scratch, sub_n * sizeof *trim_subs, 16);
                uint32_t     tc = 0u, ts = 0u;
                anim__vmob_trim_range(scene, lf->mob_idx, hm->trim_start, hm->trim_end,
                                      trim_cubics, &tc, trim_subs, &ts, total_c * 2u, sub_n);
                subs    = trim_subs;
                cubs    = trim_cubics;
                sub_n   = ts;
                total_c = tc;
                if (total_c == 0u)
                    continue;
            }

            raw_cap    = MU_MAX(total_c * 16u, 64u);
            mono_cap   = raw_cap * 2u;
            raw_quads  = (Quad2D *)mu_vm_arena_alloc(&r->shape_scratch, raw_cap * sizeof *raw_quads, 16);
            mono_quads = (Quad2D *)mu_vm_arena_alloc(&r->shape_scratch, mono_cap * sizeof *mono_quads, 16);

            float eff_world[16];
            memcpy(eff_world, hm->world_mat, sizeof eff_world);
            if (!is_fixed)
            {
                uint32_t bb = lf->mob_idx;
                while (bb != 0u && !scene->cold[bb].is_billboard)
                    bb = scene->hot[bb].parent;
                if (bb != 0u)
                {
                    const float *bw = scene->hot[bb].world_mat;
                    float        inv_b[16], rel[16], bb_m[16];
                    float        sx = sqrtf(bw[0]*bw[0] + bw[1]*bw[1] + bw[2]*bw[2]);
                    float        sy = sqrtf(bw[4]*bw[4] + bw[5]*bw[5] + bw[6]*bw[6]);
                    float        sz = sqrtf(bw[8]*bw[8] + bw[9]*bw[9] + bw[10]*bw[10]);
                    memcpy(bb_m, bw, sizeof bb_m);
                    bb_m[0] = cur_view[0] * sx; bb_m[1] = cur_view[4] * sx; bb_m[2]  = cur_view[8]  * sx;
                    bb_m[4] = cur_view[1] * sy; bb_m[5] = cur_view[5] * sy; bb_m[6]  = cur_view[9]  * sy;
                    bb_m[8] = cur_view[2] * sz; bb_m[9] = cur_view[6] * sz; bb_m[10] = cur_view[10] * sz;
                    if (bb == lf->mob_idx)
                    {
                        memcpy(eff_world, bb_m, sizeof eff_world);
                    }
                    else
                    {
                        mu_mat4_inverse(bw, inv_b);
                        mu_mat4_mul(inv_b, hm->world_mat, rel);
                        mu_mat4_mul(bb_m, rel, eff_world);
                    }
                }
            }

            for (si = 0u; si < sub_n; ++si)
            {
                SubpathMeta sm    = subs[si];
                float       arc_s = 0.0f;

                for (ci = 0u; ci < sm.cubic_count; ++ci)
                {
                    AnimCubic cb     = cubs[sm.cubic_offset + ci];
                    float     p2d[4][2];
                    float     cz[4];
                    float     zndc[4];
                    AnimVec3  pts[4] = { cb.p0, cb.p1, cb.p2, cb.p3 };
                    bool      ok     = true;
                    int       k;
                    float     w0, w1;
                    float     c0[4], c1[4];
                    uint32_t  f_start, f_end;

                    for (k = 0; k < 4; ++k)
                    {
                        float pin[3] = { pts[k].x, pts[k].y, pts[k].z };
                        float pout[3];
                        mu_mat4_transform_point(pout, eff_world, pin);
                        if (!project_world_to_px_rect_z(cur_vp, cur_view, ANIM_V3(pout[0], pout[1], pout[2]),
                                                        vx, vy, vw, vh, p2d[k], &cz[k], &zndc[k]))
                        {
                            ok = false;
                            break;
                        }
                        box_min_x = MU_MIN(box_min_x, p2d[k][0]);
                        box_min_y = MU_MIN(box_min_y, p2d[k][1]);
                        box_max_x = MU_MAX(box_max_x, p2d[k][0]);
                        box_max_y = MU_MAX(box_max_y, p2d[k][1]);
                    }
                    if (!ok)
                    {
                        c_seen++;
                        continue;
                    }
                    z_ndc_sum += 0.5f * (zndc[0] + zndc[3]);
                    z_ndc_cnt++;

                    {
                        float px_scale = (vh / 720.0f) * (1.0f + (cur_ref / cz[0] - 1.0f) * (1.0f - cur_cam->ortho_blend));
                        float px_sc1   = (vh / 720.0f) * (1.0f + (cur_ref / cz[3] - 1.0f) * (1.0f - cur_cam->ortho_blend));
                        w0        = 0.5f * hm->stroke_width * MU_CLAMP(px_scale, 0.2f, 6.0f);
                        w1        = 0.5f * hm->stroke_width * MU_CLAMP(px_sc1, 0.2f, 6.0f);
                        bg_pad_px = hm->bg_stroke_width * MU_CLAMP(px_scale, 0.2f, 6.0f);
                        max_hw_px = MU_MAX(max_hw_px, MU_MAX(w0, w1) + (has_bg_st ? bg_pad_px : 0.0f));
                    }

                    if (cm->has_stroke_grad)
                    {
                        float     u0 = (float)c_seen / (float)total_c;
                        float     u1 = (float)(c_seen + 1u) / (float)total_c;
                        AnimColor a0 = anim_color_lerp_oklab(cm->grad_start, cm->grad_end, u0);
                        AnimColor a1 = anim_color_lerp_oklab(cm->grad_start, cm->grad_end, u1);
                        c0[0] = a0.r; c0[1] = a0.g; c0[2] = a0.b; c0[3] = a0.a * lf->eff_opacity;
                        c1[0] = a1.r; c1[1] = a1.g; c1[2] = a1.b; c1[3] = a1.a * lf->eff_opacity;
                    }
                    else
                    {
                        c0[0] = 0.0f; c0[1] = 0.0f; c0[2] = 0.0f; c0[3] = -1.0f;
                        c1[0] = 0.0f; c1[1] = 0.0f; c1[2] = 0.0f; c1[3] = -1.0f;
                    }

                    f_start = (!sm.closed && ci == 0u) ? 1u : 0u;
                    f_end   = (!sm.closed && ci + 1u == sm.cubic_count) ? 2u : 0u;
                    if (pass_kind == 2u && use_hud_dof_pin)
                    {
                        lower_cubic_2d_rec(p2d, 0, w0, w1, hud_focus_ndc + 0.0004f, hud_focus_ndc + 0.0004f,
                                           c0, c1, f_start, f_end, (uint32_t)cm->cap, &arc_s, raw_quads, &raw_n, raw_cap);
                    }
                    else
                    {
                        lower_cubic_2d_rec(p2d, 0, w0, w1, zndc[0], zndc[3], c0, c1, f_start, f_end, (uint32_t)cm->cap, &arc_s, raw_quads, &raw_n, raw_cap);
                    }
                    c_seen++;
                }
            }

            {
                float cull_pad = max_hw_px + hm->shadow_px * 2.5f + 4.0f;
                if (raw_n == 0u || box_max_x + cull_pad < vx || box_min_x - cull_pad > vx + vw ||
                    box_max_y + cull_pad < vy || box_min_y - cull_pad > vy + vh)
                {
                    culled_count++;
                    continue;
                }
            }

            if (has_bg_st && shape_count + 1u < MAX_RENDER_SHAPES)
            {
                uint32_t      bg_shape_idx = shape_count++;
                GpuAnimShape *bgs          = &shapes[bg_shape_idx];
                Quad2D       *bg_quads     = (Quad2D *)mu_vm_arena_alloc(&r->shape_scratch, raw_n * sizeof *bg_quads, 16);
                float         bg_z         = (pass_kind == 2u && use_hud_dof_pin)
                    ? MU_CLAMP(hud_focus_ndc + 0.0005f, 0.0f, 1.0f)
                    : (z_ndc_cnt ? MU_CLAMP((z_ndc_sum / (float)z_ndc_cnt) + 0.0005f, 0.0f, 1.0f) : 0.5f);
                memset(bgs, 0, sizeof *bgs);
                bgs->fill_color2[3]  = -1.0f;
                bgs->stroke_color[0] = cm->bg_stroke_color.r;
                bgs->stroke_color[1] = cm->bg_stroke_color.g;
                bgs->stroke_color[2] = cm->bg_stroke_color.b;
                bgs->stroke_color[3] = cm->bg_stroke_color.a * lf->eff_opacity;
                bgs->box[0] = box_min_x; bgs->box[1] = box_min_y; bgs->box[2] = box_max_x; bgs->box[3] = box_max_y;
                bgs->clip_box[0] = is_inset ? vx : -1e9f; bgs->clip_box[1] = is_inset ? vy : -1e9f;
                bgs->clip_box[2] = is_inset ? (vx + vw) : 1e9f; bgs->clip_box[3] = is_inset ? (vy + vh) : 1e9f;
                bgs->extra[1] = bg_z;
                for (qi = 0u; qi < raw_n; ++qi)
                {
                    bg_quads[qi]       = raw_quads[qi];
                    bg_quads[qi].w0    += bg_pad_px;
                    bg_quads[qi].w1    += bg_pad_px;
                    bg_quads[qi].z0     = MU_CLAMP(bg_quads[qi].z0 + 0.0009f, 0.0f, 1.0f);
                    bg_quads[qi].z1     = MU_CLAMP(bg_quads[qi].z1 + 0.0009f, 0.0f, 1.0f);
                    bg_quads[qi].c0[3] = -1.0f;
                    bg_quads[qi].c1[3] = -1.0f;
                }
                emit_bands_for_quads(bg_quads, raw_n, bg_shape_idx, true, false,
                                     0.0f, vp_w, vp_h, bg_bands, &bg_band_cnt, segs, &seg_count);
            }

            shape_idx = shape_count++;
            gs        = &shapes[shape_idx];
            memset(gs, 0, sizeof *gs);
            gs->fill_color[0]   = hm->fill_color.r;
            gs->fill_color[1]   = hm->fill_color.g;
            gs->fill_color[2]   = hm->fill_color.b;
            gs->fill_color[3]   = hm->fill_color.a * lf->eff_opacity;
            if (cm->fill_grad_mode > 0u)
            {
                float g0_px[2] = { vx, vy }, g1_px[2] = { vx + vw, vy + vh };
                float g_rad_px = 100.0f;
                project_world_to_px_rect(cur_vp, cur_view, cm->fill_grad_p0, vx, vy, vw, vh, g0_px, NULL);
                if (cm->fill_grad_mode == 1u)
                {
                    project_world_to_px_rect(cur_vp, cur_view, cm->fill_grad_p1, vx, vy, vw, vh, g1_px, NULL);
                }
                else
                {
                    float edge_px[2] = { g0_px[0] + 100.0f, g0_px[1] };
                    AnimVec3 edge_w  = ANIM_V3(cm->fill_grad_p0.x + cm->fill_grad_radius, cm->fill_grad_p0.y, cm->fill_grad_p0.z);
                    project_world_to_px_rect(cur_vp, cur_view, edge_w, vx, vy, vw, vh, edge_px, NULL);
                    g_rad_px = fabsf(edge_px[0] - g0_px[0]);
                }
                gs->fill_color2[0] = cm->fill_grad_c2.r;
                gs->fill_color2[1] = cm->fill_grad_c2.g;
                gs->fill_color2[2] = cm->fill_grad_c2.b;
                gs->fill_color2[3] = cm->fill_grad_c2.a * lf->eff_opacity;
                gs->grad_params[0] = g0_px[0]; gs->grad_params[1] = g0_px[1];
                gs->grad_params[2] = (cm->fill_grad_mode == 1u) ? g1_px[0] : g_rad_px;
                gs->grad_params[3] = (cm->fill_grad_mode == 1u) ? g1_px[1] : 0.0f;
                if (cm->fill_grad_stop_count >= 2u && grad_count < MAX_RENDER_SHAPES)
                {
                    GpuAnimGrad *gg = &grads[grad_count++];
                    uint32_t     st;
                    memset(gg, 0, sizeof *gg);
                    gg->meta[0] = cm->fill_grad_stop_count;
                    for (st = 0u; st < cm->fill_grad_stop_count && st < 8u; ++st)
                    {
                        gg->stops[st][0] = cm->fill_grad_stops[st].r;
                        gg->stops[st][1] = cm->fill_grad_stops[st].g;
                        gg->stops[st][2] = cm->fill_grad_stops[st].b;
                        gg->stops[st][3] = cm->fill_grad_stops[st].a * lf->eff_opacity;
                    }
                    gs->extra[0] = (float)grad_count;
                }
            }
            else
            {
                gs->fill_color2[0] = 0.0f; gs->fill_color2[1] = 0.0f; gs->fill_color2[2] = 0.0f; gs->fill_color2[3] = -1.0f;
            }
            gs->stroke_color[0] = hm->stroke_color.r;
            gs->stroke_color[1] = hm->stroke_color.g;
            gs->stroke_color[2] = hm->stroke_color.b;
            gs->stroke_color[3] = hm->stroke_color.a * lf->eff_opacity;
            gs->style[0]        = hm->glow;
            gs->style[1]        = hm->sheen;
            gs->style[2]        = hm->sheen_angle;
            gs->style[3]        = hm->shadow_px * (vh / 720.0f);
            gs->sheen_color[0]  = cm->sheen_color.r;
            gs->sheen_color[1]  = cm->sheen_color.g;
            gs->sheen_color[2]  = cm->sheen_color.b;
            gs->sheen_color[3]  = cm->sheen_color.a;
            gs->extra[1]        = (pass_kind == 2u && use_hud_dof_pin)
                ? hud_focus_ndc
                : (z_ndc_cnt ? MU_CLAMP((z_ndc_sum / (float)z_ndc_cnt) - 0.0004f, 0.0f, 1.0f) : 0.5f);
            gs->box[0]          = box_min_x;
            gs->box[1]          = box_min_y;
            gs->box[2]          = box_max_x;
            gs->box[3]          = box_max_y;
            gs->dash_params[0]  = hm->dash_px * (vh / 720.0f);
            gs->dash_params[1]  = hm->gap_px * (vh / 720.0f);
            gs->dash_params[2]  = hm->dash_offset_px * (vh / 720.0f);
            gs->dash_params[3]  = (float)cm->fill_grad_mode;

            if (cm->has_clip)
            {
                float c0_px[2] = { vx, vy }, c1_px[2] = { vx + vw, vy + vh };
                project_world_to_px_rect(cur_vp, cur_view, cm->clip_min, vx, vy, vw, vh, c0_px, NULL);
                project_world_to_px_rect(cur_vp, cur_view, cm->clip_max, vx, vy, vw, vh, c1_px, NULL);
                gs->clip_box[0] = MU_MAX(MU_MIN(c0_px[0], c1_px[0]), vx);
                gs->clip_box[1] = MU_MAX(MU_MIN(c0_px[1], c1_px[1]), vy);
                gs->clip_box[2] = MU_MIN(MU_MAX(c0_px[0], c1_px[0]), vx + vw);
                gs->clip_box[3] = MU_MIN(MU_MAX(c0_px[1], c1_px[1]), vy + vh);
            }
            else if (is_inset)
            {
                gs->clip_box[0] = vx;      gs->clip_box[1] = vy;
                gs->clip_box[2] = vx + vw; gs->clip_box[3] = vy + vh;
            }
            else
            {
                gs->clip_box[0] = -1e9f; gs->clip_box[1] = -1e9f;
                gs->clip_box[2] =  1e9f; gs->clip_box[3] =  1e9f;
            }

            if (has_fill)
            {
                mono_n = 0u;
                for (qi = 0u; qi < raw_n; ++qi)
                    split_quad_y_monotonic(raw_quads[qi], mono_quads, &mono_n, mono_cap);
                emit_bands_for_quads(mono_quads, mono_n, shape_idx, false, cm->fill_rule == ANIM_FILL_EVEN_ODD,
                                     gs->style[3] * 2.5f, vp_w, vp_h, bands, &band_count, segs, &seg_count);
            }
            if (has_stroke)
            {
                emit_bands_for_quads(raw_quads, raw_n, shape_idx, true, false,
                                     0.0f, vp_w, vp_h, bands, &band_count, segs, &seg_count);
            }
        }

        if (bg_band_cnt > 0u && band_count + bg_band_cnt <= MAX_RENDER_BANDS)
        {
            uint32_t fg_cnt = band_count - grp_b_start;
            if (fg_cnt > 0u)
                memmove(&bands[grp_b_start + bg_band_cnt], &bands[grp_b_start], (size_t)fg_cnt * sizeof(GpuAnimBand));
            memcpy(&bands[grp_b_start], bg_bands, (size_t)bg_band_cnt * sizeof(GpuAnimBand));
            band_count += bg_band_cnt;
            bg_band_cnt = 0u;
        }

        if (pass_kind == 0u)
            depth_band_count = band_count - b_start;
        else if (pass_kind == 1u)
        {
            world_band_start = b_start;
            world_band_count = band_count - b_start;
        }
        else
        {
            hud_band_count = band_count - b_start;
        }
    }

    if (is_inset && vp_border.a > 0.0f && shape_count < MAX_RENDER_SHAPES &&
        seg_count + 4u <= MAX_RENDER_SEGS && band_count + 16u <= MAX_RENDER_BANDS)
    {
        uint32_t     bidx  = shape_count++;
        GpuAnimShape *bs   = &shapes[bidx];
        Quad2D       bquads[4];
        float        bw = vp_bw;
        AnimColor    bc = vp_border;
        memset(bs, 0, sizeof *bs);
        bs->stroke_color[0] = bc.r; bs->stroke_color[1] = bc.g; bs->stroke_color[2] = bc.b; bs->stroke_color[3] = bc.a;
        bs->style[0]        = 0.25f;
        bs->box[0] = vx; bs->box[1] = vy; bs->box[2] = vx + vw; bs->box[3] = vy + vh;
        bs->clip_box[0] = -1e9f; bs->clip_box[1] = -1e9f; bs->clip_box[2] = 1e9f; bs->clip_box[3] = 1e9f;
        bs->fill_color2[3] = -1.0f;
        bquads[0] = (Quad2D){ { vx, vy },       { 0.5f*(vx+vx+vw), vy },      { vx+vw, vy },    bw, bw, { bc.r, bc.g, bc.b, bc.a }, { bc.r, bc.g, bc.b, bc.a }, 0.0f, 1.0f, 0.5f, 0.5f, 0u };
        bquads[1] = (Quad2D){ { vx+vw, vy },    { vx+vw, 0.5f*(vy+vy+vh) },   { vx+vw, vy+vh }, bw, bw, { bc.r, bc.g, bc.b, bc.a }, { bc.r, bc.g, bc.b, bc.a }, 0.0f, 1.0f, 0.5f, 0.5f, 0u };
        bquads[2] = (Quad2D){ { vx+vw, vy+vh }, { 0.5f*(vx+vx+vw), vy+vh },   { vx, vy+vh },    bw, bw, { bc.r, bc.g, bc.b, bc.a }, { bc.r, bc.g, bc.b, bc.a }, 0.0f, 1.0f, 0.5f, 0.5f, 0u };
        bquads[3] = (Quad2D){ { vx, vy+vh },    { vx, 0.5f*(vy+vy+vh) },      { vx, vy },       bw, bw, { bc.r, bc.g, bc.b, bc.a }, { bc.r, bc.g, bc.b, bc.a }, 0.0f, 1.0f, 0.5f, 0.5f, 0u };
        {
            uint32_t pre_b = band_count;
            emit_bands_for_quads(bquads, 4u, bidx, true, false, 0.0f, vp_w, vp_h, bands, &band_count, segs, &seg_count);
            world_band_count += (band_count - pre_b);
        }
    }

    r_bands  = arena_alloc(r->gpu, ARENA_TRANSIENT, MU_MAX((uint64_t)band_count * sizeof *bands, 64ull), 16);
    r_shapes = arena_alloc(r->gpu, ARENA_TRANSIENT, MU_MAX((uint64_t)shape_count * sizeof *shapes, 64ull), 16);
    r_segs   = arena_alloc(r->gpu, ARENA_TRANSIENT, MU_MAX((uint64_t)seg_count * sizeof *segs, 64ull), 16);
    r_grads  = arena_alloc(r->gpu, ARENA_TRANSIENT, MU_MAX((uint64_t)grad_count * sizeof *grads, 64ull), 16);

    if (band_count > 0u)
        memcpy((uint8_t *)buffer_info(r->gpu, r_bands.buffer).mapped + r_bands.offset, bands, (size_t)band_count * sizeof *bands);
    if (shape_count > 0u)
        memcpy((uint8_t *)buffer_info(r->gpu, r_shapes.buffer).mapped + r_shapes.offset, shapes, (size_t)shape_count * sizeof *shapes);
    if (seg_count > 0u)
        memcpy((uint8_t *)buffer_info(r->gpu, r_segs.buffer).mapped + r_segs.offset, segs, (size_t)seg_count * sizeof *segs);
    if (grad_count > 0u)
        memcpy((uint8_t *)buffer_info(r->gpu, r_grads.buffer).mapped + r_grads.offset, grads, (size_t)grad_count * sizeof *grads);

    {
        uint64_t band_base = buffer_info(r->gpu, r_bands.buffer).address + r_bands.offset;
        memset(&apush, 0, sizeof apush);
        apush.bands_addr  = band_base;
        apush.shapes_addr = buffer_info(r->gpu, r_shapes.buffer).address + r_shapes.offset;
        apush.segs_addr   = buffer_info(r->gpu, r_segs.buffer).address + r_segs.offset;
        apush.grads_addr  = buffer_info(r->gpu, r_grads.buffer).address + r_grads.offset;
        apush.viewport[0] = vp_w;
        apush.viewport[1] = vp_h;
        apush.viewport[2] = 1.0f / vp_w;
        apush.viewport[3] = 1.0f / vp_h;
        apush.bg_top[0]   = scene->bg_top.r; apush.bg_top[1] = scene->bg_top.g; apush.bg_top[2] = scene->bg_top.b;
        apush.bg_top[3]   = opts->transparent_bg ? 0.0f : scene->bg_top.a;
        apush.bg_bot[0]   = scene->bg_bot.r; apush.bg_bot[1] = scene->bg_bot.g; apush.bg_bot[2] = scene->bg_bot.b;
        apush.bg_bot[3]   = opts->transparent_bg ? 0.0f : scene->bg_bot.a;

        if (surf_draw_n == 0u)
        {
            if (draw_bg || !is_inset)
            {
                pass = begin_pass(r->gpu, &(PassDesc){
                    .name        = is_inset ? "anim_inset_bg" : "anim_bg",
                    .pipe        = r->pipe_bg,
                    .state       = (DrawState){ .samples = SC_1, .blend = BLEND_OPAQUE, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                    .colors      = { is_inset ? attach_keep(hdr_target) : attach_clear(hdr_target, 0.0f, 0.0f, 0.0f, 0.0f) },
                    .color_count = 1,
                });
                if (is_inset)
                {
                    set_viewport(pass, vx, vy, vw, vh);
                    set_scissor(pass, (int32_t)vx, (int32_t)vy, (uint32_t)vw, (uint32_t)vh);
                }
                draw(pass, BYTE_SPAN(apush), 3, 1);
                end_pass(pass);
            }
        }
        else
        {
            uint64_t  base_addr   = buffer_info(r->gpu, r_surf_verts.buffer).address + r_surf_verts.offset;
            bool      has_lbuf    = (scene->light_count > 0u || scene->fog.enabled);
            uint64_t  light_addr  = 0ull;
            Image    *draw_imgs   = (Image *)mu_vm_arena_alloc(&r->scratch, surf_draw_n * sizeof *draw_imgs, 8);
            uint32_t *draw_img_id = (uint32_t *)mu_vm_arena_alloc(&r->scratch, surf_draw_n * sizeof *draw_img_id, 4);
            uint32_t  samp_id     = sampler_id(r->gpu, r->linear);
            uint32_t  nearest_id  = sampler_id(r->gpu, r->nearest);
            uint32_t  chunk_start = 0u;
            bool      use_msaa    = r->has_msaa && !is_inset;
            Image     surf_col    = use_msaa ? r->hdr_msaa : hdr_target;
            Image     surf_dep    = use_msaa ? r->depth_msaa : r->depth_buf;
            SampleCount surf_sc   = use_msaa ? SC_4 : SC_1;

            if (has_lbuf)
            {
                Range r_light = arena_alloc(r->gpu, ARENA_TRANSIENT, sizeof(GpuSceneLighting), 16);
                GpuSceneLighting *gl = (GpuSceneLighting *)((uint8_t *)buffer_info(r->gpu, r_light.buffer).mapped + r_light.offset);
                uint32_t li;
                memset(gl, 0, sizeof *gl);
                for (li = 0u; li < scene->light_count && li < 4u; ++li)
                {
                    const AnimLight *al = &scene->lights[li];
                    gl->lights[li].pos_type[0]  = al->pos_or_dir.x;
                    gl->lights[li].pos_type[1]  = al->pos_or_dir.y;
                    gl->lights[li].pos_type[2]  = al->pos_or_dir.z;
                    gl->lights[li].pos_type[3]  = (al->kind == ANIM_LIGHT_POINT) ? 1.0f : 0.0f;
                    gl->lights[li].color_int[0] = al->color.r;
                    gl->lights[li].color_int[1] = al->color.g;
                    gl->lights[li].color_int[2] = al->color.b;
                    gl->lights[li].color_int[3] = al->intensity;
                }
                {
                    float ai = scene->ambient_intensity > 0.0f ? scene->ambient_intensity : 0.25f;
                    gl->ambient[0] = scene->ambient_color.r * ai;
                    gl->ambient[1] = scene->ambient_color.g * ai;
                    gl->ambient[2] = scene->ambient_color.b * ai;
                    gl->ambient[3] = (float)(scene->light_count > 4u ? 4u : scene->light_count);
                }
                if (scene->fog.enabled)
                {
                    gl->fog_color[0]  = scene->fog.color.r;
                    gl->fog_color[1]  = scene->fog.color.g;
                    gl->fog_color[2]  = scene->fog.color.b;
                    gl->fog_color[3]  = scene->fog.density;
                    gl->fog_params[0] = scene->fog.start;
                    gl->fog_params[1] = scene->fog.end;
                    gl->fog_params[2] = scene->fog.exponential ? 2.0f : 1.0f;
                }
                light_addr = buffer_info(r->gpu, r_light.buffer).address + r_light.offset;
            }

            for (i = 0u; i < surf_draw_n; ++i)
            {
                draw_imgs[i]   = (Image){ 0 };
                draw_img_id[i] = 0u;
                if (surf_draws[i].tex_id != 0u)
                {
                    draw_imgs[i] = ensure_gpu_texture(r, scene, surf_draws[i].tex_id);
                    if (HANDLE_VALID(draw_imgs[i]))
                        draw_img_id[i] = image_sampled_id(r->gpu, draw_imgs[i]);
                }
            }

            if (draw_bg || !is_inset)
            {
                pass = begin_pass(r->gpu, &(PassDesc){
                    .name        = is_inset ? "anim_inset_bg" : "anim_bg",
                    .pipe        = r->pipe_bg,
                    .state       = (DrawState){ .samples = surf_sc, .blend = BLEND_OPAQUE, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                    .colors      = { (is_inset && !use_msaa) ? attach_keep(surf_col) : attach_clear(surf_col, 0.0f, 0.0f, 0.0f, 0.0f) },
                    .color_count = 1,
                });
                if (is_inset)
                {
                    set_viewport(pass, vx, vy, vw, vh);
                    set_scissor(pass, (int32_t)vx, (int32_t)vy, (uint32_t)vw, (uint32_t)vh);
                }
                draw(pass, BYTE_SPAN(apush), 3, 1);
                end_pass(pass);
            }

            while (chunk_start < surf_draw_n)
            {
                PassDesc pdesc;
                uint32_t chunk_end = chunk_start;
                uint32_t used_tid[MAX_IMAGE_USES];
                uint32_t used_n = 0u;

                while (chunk_end < surf_draw_n)
                {
                    uint32_t tid = surf_draws[chunk_end].tex_id;
                    if (tid != 0u && HANDLE_VALID(draw_imgs[chunk_end]))
                    {
                        bool found = false;
                        uint32_t u;
                        for (u = 0u; u < used_n; ++u)
                        {
                            if (used_tid[u] == tid) { found = true; break; }
                        }
                        if (!found)
                        {
                            if (used_n >= MAX_IMAGE_USES)
                                break;
                            used_tid[used_n] = tid;
                            used_n++;
                        }
                    }
                    chunk_end++;
                }

                memset(&pdesc, 0, sizeof pdesc);
                pdesc.name        = is_inset ? "anim_inset_surfaces3d" : "anim_surfaces3d";
                pdesc.pipe        = r->pipe_surface3d;
                pdesc.state       = (DrawState){ .samples = surf_sc, .blend = BLEND_PREMUL, .depth_compare = CMP_LESS, .depth_write = 1, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff };
                pdesc.colors[0]   = attach_keep(surf_col);
                pdesc.color_count = 1;
                pdesc.depth       = (chunk_start == 0u) ? attach_clear_depth(surf_dep, 1.0f) : attach_keep(surf_dep);
                pdesc.has_depth   = true;
                if (use_msaa && chunk_end == surf_draw_n)
                {
                    pdesc.colors[0].resolve = SUB(hdr_target);
                    pdesc.depth.resolve     = SUB(r->depth_buf);
                }
                pdesc.image_count = used_n;
                for (i = 0u; i < used_n; ++i)
                {
                    uint32_t k;
                    for (k = chunk_start; k < chunk_end; ++k)
                    {
                        if (surf_draws[k].tex_id == used_tid[i])
                        {
                            pdesc.images[i] = use_sampled(draw_imgs[k]);
                            break;
                        }
                    }
                }

                pass = begin_pass(r->gpu, &pdesc);
                if (is_inset)
                {
                    set_viewport(pass, vx, vy, vw, vh);
                    set_scissor(pass, (int32_t)vx, (int32_t)vy, (uint32_t)vw, (uint32_t)vh);
                }
                for (i = chunk_start; i < chunk_end; ++i)
                {
                    SurfacePush spush;
                    memset(&spush, 0, sizeof spush);
                    spush.verts_addr      = base_addr + (uint64_t)surf_draws[i].vert_off * sizeof(GpuSurfaceVert);
                    spush.lighting_addr   = light_addr;
                    memcpy(spush.mvp, vp_mat, sizeof vp_mat);
                    spush.cam_pos_grid[0] = cam->pos.x;
                    spush.cam_pos_grid[1] = cam->pos.y;
                    spush.cam_pos_grid[2] = cam->pos.z;
                    spush.cam_pos_grid[3] = surf_draws[i].grid_density;
                    spush.style_params[0] = surf_draws[i].opacity;
                    spush.style_params[1] = surf_draws[i].glow;
                    spush.style_params[2] = surf_draws[i].grid_alpha;
                    spush.style_params[3] = surf_draws[i].roughness;
                    if (surf_draws[i].tex_id != 0u && HANDLE_VALID(draw_imgs[i]))
                    {
                        spush.tex_params[0] = draw_img_id[i];
                        spush.tex_params[1] = surf_draws[i].tex_nearest ? nearest_id : samp_id;
                        spush.tex_params[2] = 1u;
                    }
                    spush.tex_params[3] = surf_draws[i].unlit ? 1u : 0u;
                    if (surf_draws[i].checker)
                    {
                        spush.checker_c1[0] = surf_draws[i].checker_c1.r;
                        spush.checker_c1[1] = surf_draws[i].checker_c1.g;
                        spush.checker_c1[2] = surf_draws[i].checker_c1.b;
                        spush.checker_c1[3] = surf_draws[i].checker_c1.a;
                        spush.checker_c2[0] = surf_draws[i].checker_c2.r;
                        spush.checker_c2[1] = surf_draws[i].checker_c2.g;
                        spush.checker_c2[2] = surf_draws[i].checker_c2.b;
                        spush.checker_c2[3] = surf_draws[i].checker_c2.a;
                        spush.checker_uv[0] = (float)surf_draws[i].checker_u;
                        spush.checker_uv[1] = (float)surf_draws[i].checker_v;
                        spush.checker_uv[2] = 1.0f;
                    }
                    spush.checker_uv[3] = has_lbuf ? 1.0f : 0.0f;
                    draw(pass, BYTE_SPAN(spush), surf_draws[i].vert_cnt, 1u);
                }
                end_pass(pass);
                chunk_start = chunk_end;
            }
        }

        if (depth_band_count > 0u)
        {
            apush.bands_addr = band_base;
            pass = begin_pass(r->gpu, &(PassDesc){
                .name        = is_inset ? "anim_inset_bands_depth" : "anim_bands_depth",
                .pipe        = r->pipe_band_depth,
                .state       = (DrawState){ .samples = SC_1, .blend = BLEND_PREMUL, .depth_compare = CMP_LEQUAL, .depth_write = 1, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                .colors      = { attach_keep(hdr_target) },
                .color_count = 1,
                .depth       = (surf_draw_n == 0u) ? attach_clear_depth(r->depth_buf, 1.0f) : attach_keep(r->depth_buf),
                .has_depth   = true,
            });
            if (is_inset)
            {
                set_scissor(pass, (int32_t)MU_MAX(vx - vp_bw, 0.0f), (int32_t)MU_MAX(vy - vp_bw, 0.0f),
                            (uint32_t)MU_MIN(vw + 2.0f * vp_bw, vp_w), (uint32_t)MU_MIN(vh + 2.0f * vp_bw, vp_h));
            }
            draw(pass, BYTE_SPAN(apush), depth_band_count * 6u, 1u);
            end_pass(pass);
        }
        else if (!is_inset && surf_draw_n == 0u && opts->dof_aperture_px > 0.01f && hud_band_count == 0u)
        {
            pass = begin_pass(r->gpu, &(PassDesc){
                .name        = "anim_depth_clear",
                .pipe        = r->pipe_band_depth,
                .colors      = { attach_keep(hdr_target) },
                .color_count = 1,
                .depth       = attach_clear_depth(r->depth_buf, 1.0f),
                .has_depth   = true,
            });
            end_pass(pass);
        }

        if (world_band_count > 0u)
        {
            uint32_t non_depth_start = (depth_band_count > 0u) ? world_band_start : 0u;
            apush.bands_addr = band_base + (uint64_t)non_depth_start * sizeof(GpuAnimBand);
            pass = begin_pass(r->gpu, &(PassDesc){
                .name        = is_inset ? "anim_inset_bands" : "anim_bands",
                .pipe        = r->pipe_band,
                .state       = (DrawState){ .samples = SC_1, .blend = BLEND_PREMUL, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                .colors      = { attach_keep(hdr_target) },
                .color_count = 1,
            });
            if (is_inset)
            {
                set_scissor(pass, (int32_t)MU_MAX(vx - vp_bw, 0.0f), (int32_t)MU_MAX(vy - vp_bw, 0.0f),
                            (uint32_t)MU_MIN(vw + 2.0f * vp_bw, vp_w), (uint32_t)MU_MIN(vh + 2.0f * vp_bw, vp_h));
            }
            draw(pass, BYTE_SPAN(apush), world_band_count * 6u, 1u);
            end_pass(pass);
        }

        if (hud_band_count > 0u)
        {
            uint32_t hud_start = world_band_start + world_band_count;
            apush.bands_addr = band_base + (uint64_t)hud_start * sizeof(GpuAnimBand);
            if (use_hud_dof_pin)
            {
                pass = begin_pass(r->gpu, &(PassDesc){
                    .name        = "anim_hud_bands_dof_pin",
                    .pipe        = r->pipe_band_depth,
                    .state       = (DrawState){ .samples = SC_1, .blend = BLEND_PREMUL, .depth_compare = CMP_ALWAYS, .depth_write = 1, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                    .colors      = { attach_keep(hdr_target) },
                    .color_count = 1,
                    .depth       = (surf_draw_n == 0u && depth_band_count == 0u) ? attach_clear_depth(r->depth_buf, 1.0f) : attach_keep(r->depth_buf),
                    .has_depth   = true,
                });
            }
            else
            {
                pass = begin_pass(r->gpu, &(PassDesc){
                    .name        = "anim_hud_bands",
                    .pipe        = r->pipe_band,
                    .state       = (DrawState){ .samples = SC_1, .blend = BLEND_PREMUL, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                    .colors      = { attach_keep(hdr_target) },
                    .color_count = 1,
                });
            }
            draw(pass, BYTE_SPAN(apush), hud_band_count * 6u, 1u);
            end_pass(pass);
        }
    }

    if (!is_inset)
    {
        r->last_stats = (AnimFrameStats){
            .shape_count        = shape_count,
            .band_count         = band_count,
            .quad_seg_count     = seg_count,
            .surface_tri_count  = surf_tris,
            .culled_shape_count = culled_count,
            .cpu_build_ms       = (double)(mu_clock_ns() - t_begin_ns) * 1e-6,
        };
    }
}

static void render_sub_viewport(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts,
                                Image hdr_target, const AnimCamera *cam, const float rect[4],
                                AnimColor border, float border_w, bool draw_bg)
{
    float x0 = MU_CLAMP(rect[0], 0.0f, 0.96f);
    float y0 = MU_CLAMP(rect[1], 0.0f, 0.96f);
    float x1 = MU_CLAMP(rect[2], x0 + 0.04f, 1.0f);
    float y1 = MU_CLAMP(rect[3], y0 + 0.04f, 1.0f);
    float ix = floorf(x0 * (float)r->width);
    float iy = floorf(y0 * (float)r->height);
    float iw = MU_MAX(floorf((x1 - x0) * (float)r->width), 32.0f);
    float ih = MU_MAX(floorf((y1 - y0) * (float)r->height), 32.0f);
    VpDrawCfg cfg = {
        .is_sub_vp    = true,
        .draw_bg      = draw_bg,
        .border       = border,
        .border_width = border_w,
    };
    render_scene_camera_to_hdr(r, scene, opts, hdr_target, cam, ix, iy, iw, ih, &cfg);
}

static void render_scene_to_hdr(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts, Image hdr_target)
{
    VpDrawCfg main_cfg = { .is_sub_vp = false, .draw_bg = true, .border = anim_rgba(0,0,0,0), .border_width = 0.0f };
    uint32_t  i;

    render_scene_camera_to_hdr(r, scene, opts, hdr_target, &scene->camera,
                               0.0f, 0.0f, (float)r->width, (float)r->height, &main_cfg);

    if (opts->zoom_inset_enable || scene->zoom_inset_enable)
    {
        const float      *zr = opts->zoom_inset_enable ? opts->zoom_inset_rect : scene->zoom_inset_rect;
        const AnimCamera *zc = opts->zoom_inset_enable ? &opts->zoom_inset_cam : &scene->zoom_inset_cam;
        AnimColor         bc = opts->zoom_inset_enable
            ? (opts->zoom_inset_border.a > 0.0f ? opts->zoom_inset_border : ANIM_YELLOW)
            : (scene->zoom_inset_border.a > 0.0f ? scene->zoom_inset_border : ANIM_YELLOW);
        render_sub_viewport(r, scene, opts, hdr_target, zc, zr, bc, 2.0f, true);
    }

    for (i = 0u; i < scene->viewport_count && i < (uint32_t)ANIM_MAX_VIEWPORTS; ++i)
    {
        const AnimViewport *vp = &scene->viewports[i];
        if (vp->active)
            render_sub_viewport(r, scene, opts, hdr_target, &vp->cam, vp->rect,
                                vp->border, vp->border_width, vp->draw_bg);
    }
}

static void render_bloom_and_composite(AnimRenderer *r, const AnimScene *scene, const AnimRenderOpts *opts, Image hdr_src, Image final_dst)
{
    uint32_t w          = r->width;
    uint32_t h          = r->height;
    float    vp_w       = (float)w;
    float    vp_h       = (float)h;
    uint32_t bloom_mips = MU_MIN((uint32_t)BLOOM_LEVELS, 32u - mu_leading_zeroes_u32(MU_MAX(w >> 1, h >> 1)));
    PostPush ppush;
    Pass    *pass;
    uint32_t i;

    bloom_mips = MU_MAX(bloom_mips, 1u);
    memset(&ppush, 0, sizeof ppush);
    ppush.linear_sampler = sampler_id(r->gpu, r->linear);
    ppush.bloom[0]       = opts->bloom_threshold;
    ppush.bloom[1]       = opts->bloom_knee > 0.0f ? opts->bloom_knee : 0.45f;

    if (opts->bloom && opts->bloom_strength > 1e-4f)
    {
        for (i = 0u; i < bloom_mips; ++i)
        {
            ImageSub src   = (i == 0u) ? SUB(hdr_src) : SUB_MIP(r->bloom, i - 1u, 1);
            uint32_t src_w = MU_MAX(w >> i, 1u);
            uint32_t src_h = MU_MAX(h >> i, 1u);
            ppush.texel[0]  = 1.0f / (float)src_w;
            ppush.texel[1]  = 1.0f / (float)src_h;
            ppush.color_map = image_sub_sampled_id(r->gpu, src);
            pass = begin_pass(r->gpu, &(PassDesc){
                .name        = (i == 0u) ? "bloom_prefilter" : "bloom_down",
                .pipe        = (i == 0u) ? r->pipe_prefilter : r->pipe_down,
                .state       = (DrawState){ .samples = SC_1, .blend = BLEND_OPAQUE, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                .colors      = { attach_sub(SUB_MIP(r->bloom, i, 1), DONT_CARE, STORE) },
                .color_count = 1,
                .images      = { use_image(src, ACC_READ, IMAGE_SAMPLED) },
                .image_count = 1,
            });
            draw(pass, BYTE_SPAN(ppush), 3, 1);
            end_pass(pass);
        }
        for (i = bloom_mips - 1u; i > 0u; --i)
        {
            ImageSub src = SUB_MIP(r->bloom, i, 1);
            ppush.texel[0]  = 1.0f / (float)MU_MAX(w >> (i + 1u), 1u);
            ppush.texel[1]  = 1.0f / (float)MU_MAX(h >> (i + 1u), 1u);
            ppush.color_map = image_sub_sampled_id(r->gpu, src);
            pass = begin_pass(r->gpu, &(PassDesc){
                .name        = "bloom_up",
                .pipe        = r->pipe_up,
                .state       = (DrawState){ .samples = SC_1, .blend = BLEND_ADD, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
                .colors      = { attach_sub(SUB_MIP(r->bloom, i - 1u, 1), LOAD, STORE) },
                .color_count = 1,
                .images      = { use_image(src, ACC_READ, IMAGE_SAMPLED) },
                .image_count = 1,
            });
            draw(pass, BYTE_SPAN(ppush), 3, 1);
            end_pass(pass);
        }
    }
    else
    {
        pass = begin_pass(r->gpu, &(PassDesc){
            .name        = "bloom_clear",
            .pipe        = r->pipe_prefilter,
            .colors      = { attach_clear(r->bloom, 0.0f, 0.0f, 0.0f, 0.0f) },
            .color_count = 1,
        });
        end_pass(pass);
    }

    ppush.texel[0]  = 1.0f / vp_w;
    ppush.texel[1]  = 1.0f / vp_h;
    ppush.texel[2]  = 1.0f / vp_w;
    ppush.texel[3]  = 1.0f / vp_h;
    ppush.grade[0]  = opts->exposure;
    ppush.grade[1]  = opts->bloom ? opts->bloom_strength : 0.0f;
    ppush.grade[2]  = opts->saturation;
    ppush.grade[3]  = opts->vignette_radius;
    ppush.bloom[2]  = opts->transparent_bg ? 1.0f : 0.0f;
    ppush.hud[0]    = opts->hud_progress;
    ppush.hud[1]    = opts->hud_playing ? 1.0f : 0.0f;
    ppush.hud[2]    = opts->show_hud ? 1.0f : 0.0f;
    ppush.fx[0]     = opts->aberration_px;
    ppush.fx[1]     = opts->grain_amount;
    ppush.fx[2]     = (float)opts->tonemap;
    ppush.fx[3]     = opts->hud_progress;
    ppush.color_map = image_sampled_id(r->gpu, hdr_src);
    ppush.bloom_map = image_sub_sampled_id(r->gpu, SUB(r->bloom));
    ppush.dof[0]    = opts->dof_aperture_px;
    ppush.dof[1]    = opts->dof_focus_dist > 0.05f ? opts->dof_focus_dist : 8.0f;
    ppush.dof[2]    = opts->dof_focus_range > 0.05f ? opts->dof_focus_range : 1.5f;
    ppush.dof[3]    = scene ? scene->camera.ortho_blend : 0.0f;

    {
        bool use_dof = opts->dof_aperture_px > 0.01f;
        PassDesc cdesc = {
            .name        = "anim_composite",
            .pipe        = r->pipe_composite,
            .state       = (DrawState){ .samples = SC_1, .blend = BLEND_OPAQUE, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
            .colors      = { attach_write(final_dst) },
            .color_count = 1,
            .images      = { use_sampled(hdr_src), use_sampled(r->bloom) },
            .image_count = 2,
        };
        if (use_dof)
        {
            ppush.depth_map = image_sampled_id(r->gpu, r->depth_buf);
            cdesc.images[2] = use_sampled(r->depth_buf);
            cdesc.image_count = 3;
        }
        pass = begin_pass(r->gpu, &cdesc);
    }
    draw(pass, BYTE_SPAN(ppush), 3, 1);
    end_pass(pass);
}

void anim_render_to_frame(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts)
{
    GpuInfo info = gpu_info(r->gpu);
    if (info.width != r->width || info.height != r->height)
    {
        destroy_render_targets(r);
        create_render_targets(r, info.width, info.height);
    }
    render_scene_to_hdr(r, scene, opts, r->hdr_color);
    render_bloom_and_composite(r, scene, opts, r->hdr_color, frame_image(r->gpu));
}

void anim_render_to_image(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts, Image dst)
{
    render_scene_to_hdr(r, scene, opts, r->hdr_color);
    render_bloom_and_composite(r, scene, opts, r->hdr_color, dst);
}

AnimFrameStats anim_renderer_last_stats(const AnimRenderer *r)
{
    return r ? r->last_stats : (AnimFrameStats){ 0 };
}

/* ------------------------------------------- motion blur, PNG & MP4 export */

static void render_subframes_to_shot(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts,
                                     float center_t, float frame_dt)
{
    uint32_t samples = MU_MAX(opts->motion_blur_samples, 1u);
    float    shutter = MU_CLAMP(opts->shutter_angle, 0.0f, 1.0f);
    float    dur     = anim_timeline_duration(tl);
    uint32_t si;

    if (samples <= 1u || !tl || shutter <= 1e-4f)
    {
        if (frame_begin(r->gpu))
        {
            anim_render_to_image(r, scene, opts, r->shot);
            frame_end(r->gpu);
        }
        return;
    }

    for (si = 0u; si < samples; ++si)
    {
        float    u  = ((float)si + 0.5f) / (float)samples - 0.5f;
        float    ts = MU_CLAMP(center_t + u * shutter * frame_dt, 0.0f, dur);
        PostPush ap;
        Pass    *p;

        anim_seek(tl, scene, ts);
        if (!frame_begin(r->gpu))
            continue;

        render_scene_to_hdr(r, scene, opts, r->hdr_color);

        memset(&ap, 0, sizeof ap);
        ap.linear_sampler = sampler_id(r->gpu, r->linear);
        ap.color_map      = image_sampled_id(r->gpu, r->hdr_color);
        ap.bloom[3]       = 1.0f / (float)samples;

        p = begin_pass(r->gpu, &(PassDesc){
            .name        = "anim_accum",
            .pipe        = r->pipe_accum_blit,
            .state       = (DrawState){ .samples = SC_1, .blend = (si == 0u) ? BLEND_OPAQUE : BLEND_ADD, .depth_compare = CMP_NONE, .cull = CULL_NONE, .topology = TOPO_TRI, .write_mask = 0xffff },
            .colors      = { (si == 0u) ? attach_clear(r->accum_color, 0.0f, 0.0f, 0.0f, 0.0f) : attach_keep(r->accum_color) },
            .color_count = 1,
            .images      = { use_sampled(r->hdr_color) },
            .image_count = 1,
        });
        draw(p, BYTE_SPAN(ap), 3, 1);
        end_pass(p);

        if (si + 1u == samples)
            render_bloom_and_composite(r, scene, opts, r->accum_color, r->shot);
        frame_end(r->gpu);
    }
}

bool anim_export_png(AnimRenderer *r, AnimScene *scene, const AnimRenderOpts *opts, const char *path)
{
    size_t    bytes  = (size_t)r->width * r->height * 4u;
    uint8_t  *pixels = (uint8_t *)malloc(bytes);
    Readback *rb;
    int       ok;

    if (!pixels)
        return false;
    if (!frame_begin(r->gpu))
    {
        free(pixels);
        return false;
    }
    anim_render_to_image(r, scene, opts, r->shot);
    frame_end(r->gpu);

    rb = readback_image(r->gpu, SUB(r->shot), (DataSpan){ pixels, (uint32_t)bytes });
    readback_ready(r->gpu, rb, true);
    readback_release(r->gpu, rb);

    ok = stbi_write_png(path, (int)r->width, (int)r->height, 4, pixels, (int)r->width * 4);
    free(pixels);
    return ok != 0;
}

static bool ends_with_ci(const char *s, const char *ext)
{
    size_t sl, el, i;
    if (!s || !ext) return false;
    sl = strlen(s);
    el = strlen(ext);
    if (sl < el) return false;
    s += sl - el;
    for (i = 0u; i < el; ++i)
    {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

static bool export_video_range(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts,
                               const char *out_path, const char *audio_wav_path, float t_start, float t_end)
{
    uint32_t   fps          = opts->fps ? opts->fps : ANIM_DEFAULT_FPS;
    float      tl_dur       = anim_timeline_duration(tl);
    float      span         = MU_MAX(t_end - t_start, 1.0f / (float)fps);
    uint32_t   total_frames = (uint32_t)ceilf(span * (float)fps);
    size_t     frame_bytes  = (size_t)r->width * r->height * 4u;
    uint8_t   *rings[3]     = { NULL, NULL, NULL };
    Readback  *rbs[3]       = { NULL, NULL, NULL };
    char       cmd[1536];
    FILE      *pipe;
    uint32_t   f;
    bool       is_webm      = ends_with_ci(out_path, ".webm");
    bool       is_mov       = ends_with_ci(out_path, ".mov");
    bool       is_gif       = ends_with_ci(out_path, ".gif");

    if (is_gif)
    {
        if (opts->transparent_bg)
        {
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                     "-vf \"split[s0][s1];[s0]palettegen=reserve_transparent=1:stats_mode=diff[p];[s1][p]paletteuse=alpha_threshold=128\" \"%s\"",
                     r->width, r->height, fps, out_path);
        }
        else
        {
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                     "-vf \"split[s0][s1];[s0]palettegen=stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=3\" \"%s\"",
                     r->width, r->height, fps, out_path);
        }
    }
    else if (is_webm)
    {
        const char *vpix = opts->transparent_bg ? "yuva420p" : "yuv420p";
        if (audio_wav_path && audio_wav_path[0])
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                     "-i \"%s\" -c:v libvpx-vp9 -pix_fmt %s -crf 24 -b:v 0 -c:a libopus -shortest \"%s\"",
                     r->width, r->height, fps, audio_wav_path, vpix, out_path);
        else
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                     "-c:v libvpx-vp9 -pix_fmt %s -crf 24 -b:v 0 \"%s\"",
                     r->width, r->height, fps, vpix, out_path);
    }
    else if (is_mov && opts->transparent_bg)
    {
        if (audio_wav_path && audio_wav_path[0])
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                     "-i \"%s\" -c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le -c:a pcm_s16le -shortest \"%s\"",
                     r->width, r->height, fps, audio_wav_path, out_path);
        else
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                     "-c:v prores_ks -profile:v 4444 -pix_fmt yuva444p10le \"%s\"",
                     r->width, r->height, fps, out_path);
    }
    else if (audio_wav_path && audio_wav_path[0])
    {
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                 "-i \"%s\" -c:v libx264 -pix_fmt yuv420p -crf 17 -preset fast -c:a aac -shortest \"%s\"",
                 r->width, r->height, fps, audio_wav_path, out_path);
    }
    else
    {
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -y -loglevel error -f rawvideo -pix_fmt rgba -s %ux%u -r %u -i - "
                 "-c:v libx264 -pix_fmt yuv420p -crf 17 -preset fast \"%s\"",
                 r->width, r->height, fps, out_path);
    }

    pipe = popen(cmd, "w");
    if (!pipe)
    {
        anim__set_error("anim_export_video: failed to open ffmpeg pipe");
        log_error("anim_export_video: failed to open ffmpeg pipe");
        return false;
    }

    for (f = 0u; f < 3u; ++f)
        rings[f] = (uint8_t *)malloc(frame_bytes);

    for (f = 0u; f < total_frames; ++f)
    {
        uint32_t slot = f % 3u;
        float    t    = t_start + ((float)f / (float)fps);

        if (rbs[slot])
        {
            readback_ready(r->gpu, rbs[slot], true);
            readback_release(r->gpu, rbs[slot]);
            rbs[slot] = NULL;
            if (fwrite(rings[slot], 1u, frame_bytes, pipe) != frame_bytes)
                break;
        }

        if (opts->motion_blur_samples > 1u)
        {
            render_subframes_to_shot(r, tl, scene, opts, MU_CLAMP(t, 0.0f, tl_dur), 1.0f / (float)fps);
        }
        else
        {
            anim_seek(tl, scene, MU_CLAMP(t, 0.0f, tl_dur));
            if (!frame_begin(r->gpu))
                continue;
            anim_render_to_image(r, scene, opts, r->shot);
            frame_end(r->gpu);
        }
        rbs[slot] = readback_image(r->gpu, SUB(r->shot), (DataSpan){ rings[slot], (uint32_t)frame_bytes });
    }

    for (f = (total_frames > 3u ? total_frames - 3u : 0u); f < total_frames; ++f)
    {
        uint32_t slot = f % 3u;
        if (rbs[slot])
        {
            readback_ready(r->gpu, rbs[slot], true);
            readback_release(r->gpu, rbs[slot]);
            rbs[slot] = NULL;
            fwrite(rings[slot], 1u, frame_bytes, pipe);
        }
    }

    for (f = 0u; f < 3u; ++f)
        free(rings[f]);

    return pclose(pipe) == 0;
}

bool anim_export_video(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts,
                       const char *mp4_path, const char *audio_wav_path)
{
    float dur = MU_MAX(anim_timeline_duration(tl), 1.0f / (float)(opts->fps ? opts->fps : ANIM_DEFAULT_FPS));
    return export_video_range(r, tl, scene, opts, mp4_path, audio_wav_path, 0.0f, dur);
}

bool anim_export_gif(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts,
                     const char *gif_path)
{
    float dur = MU_MAX(anim_timeline_duration(tl), 1.0f / (float)(opts->fps ? opts->fps : ANIM_DEFAULT_FPS));
    if (!gif_path || !gif_path[0])
        return false;
    return export_video_range(r, tl, scene, opts, gif_path, NULL, 0.0f, dur);
}

bool anim_export_sections(AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, const AnimRenderOpts *opts,
                          const char *out_dir)
{
    uint32_t n = anim_timeline_section_count(tl);
    uint32_t i;
    bool     all_ok = true;

    if (!r || !tl || !scene || !opts || !out_dir || !out_dir[0])
        return false;
    mkdir(out_dir, 0755);
    if (n == 0u)
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/section_000_main.mp4", out_dir);
        return anim_export_video(r, tl, scene, opts, path, NULL);
    }
    for (i = 0u; i < n; ++i)
    {
        const char *name = NULL;
        char        safe[64];
        char        path[1024];
        float       t0 = 0.0f, t1 = 0.0f;
        bool        skip = false;
        size_t      j, k = 0u;

        if (!anim_timeline_section_info(tl, i, &name, &t0, &t1, &skip))
            continue;
        if (skip || t1 <= t0 + 1e-4f)
            continue;
        if (name)
        {
            for (j = 0u; name[j] && k + 1u < sizeof safe; ++j)
            {
                unsigned char c = (unsigned char)name[j];
                safe[k++] = (isalnum(c) || c == '_' || c == '-') ? (char)c : '_';
            }
        }
        safe[k] = '\0';
        snprintf(path, sizeof path, "%s/section_%03u_%s.mp4", out_dir, i, safe[0] ? safe : "section");
        if (!export_video_range(r, tl, scene, opts, path, NULL, t0, t1))
            all_ok = false;
    }
    return all_ok;
}

/* ----------------------------------------------------- interactive preview */

static void preview_orbit_cam(AnimCamera *cam, float d_az, float d_el)
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
}

void anim_run_interactive(Platform *platform, Gpu *gpu, AnimRenderer *r, AnimTimeline *tl, AnimScene *scene, AnimRenderOpts opts)
{
    MuInputState *input       = platform_input(platform);
    float         dur         = MU_MAX(anim_timeline_duration(tl), 1e-3f);
    float         t           = 0.0f;
    float         speed       = 1.0f;
    bool          playing     = true;
    uint32_t      shot_n      = 0u;
    bool          cam_over    = false;
    float         orbit_az    = 0.0f;
    float         orbit_el    = 0.0f;
    float         zoom_mul    = 1.0f;
    float         last_mx     = 0.0f;
    float         last_my     = 0.0f;
    bool          dragging_3d = false;

    opts.show_hud = true;
    while (platform_poll(platform))
    {
        uint32_t w, h;
        bool     shift = mu_input_key_down(input, MU_KEY_LEFT_SHIFT) || mu_input_key_down(input, MU_KEY_RIGHT_SHIFT);

        if (mu_input_key_pressed(input, MU_KEY_ESCAPE))
            break;
        if (mu_input_key_pressed(input, MU_KEY_SPACE))
            playing = !playing;
        if (mu_input_key_pressed(input, MU_KEY_R) || mu_input_key_pressed(input, MU_KEY_HOME))
            t = 0.0f;
        if (mu_input_key_pressed(input, MU_KEY_C))
        {
            cam_over = false;
            orbit_az = 0.0f;
            orbit_el = 0.0f;
            zoom_mul = 1.0f;
        }
        if (mu_input_key_pressed(input, MU_KEY_B))
            opts.bloom = !opts.bloom;
        if (mu_input_key_pressed(input, MU_KEY_H))
            opts.show_hud = !opts.show_hud;
        if (mu_input_key_pressed(input, MU_KEY_LEFT))
        {
            playing = false;
            t       = MU_MAX(0.0f, t - (shift ? 1.0f : (1.0f / 30.0f)));
        }
        if (mu_input_key_pressed(input, MU_KEY_RIGHT))
        {
            playing = false;
            t       = MU_MIN(dur, t + (shift ? 1.0f : (1.0f / 30.0f)));
        }
        if (mu_input_key_pressed(input, MU_KEY_1)) speed = 0.25f;
        if (mu_input_key_pressed(input, MU_KEY_2)) speed = 0.5f;
        if (mu_input_key_pressed(input, MU_KEY_3)) speed = 1.0f;
        if (mu_input_key_pressed(input, MU_KEY_4)) speed = 2.0f;
        if (mu_input_key_pressed(input, MU_KEY_5)) speed = 4.0f;

        platform_size(platform, &w, &h);
        gpu_resize(gpu, w, h);

        if (opts.show_hud && mu_input_mouse_down(input, MU_MOUSE_LEFT) && input->mouse_y >= (float)h - 32.0f && w > 48u)
        {
            float u = MU_CLAMP((input->mouse_x - 24.0f) / (float)(w - 48u), 0.0f, 1.0f);
            playing     = false;
            dragging_3d = false;
            t           = u * dur;
        }
        else if (mu_input_mouse_down(input, MU_MOUSE_LEFT))
        {
            if (!dragging_3d)
            {
                dragging_3d = true;
                last_mx     = input->mouse_x;
                last_my     = input->mouse_y;
            }
            else
            {
                float dx = input->mouse_x - last_mx;
                float dy = input->mouse_y - last_my;
                last_mx  = input->mouse_x;
                last_my  = input->mouse_y;
                if (fabsf(dx) > 0.1f || fabsf(dy) > 0.1f)
                {
                    cam_over  = true;
                    orbit_az -= dx * 0.0055f;
                    orbit_el += dy * 0.0055f;
                }
            }
        }
        else
        {
            dragging_3d = false;
        }

        if (fabsf(input->wheel_y) > 1e-4f)
        {
            cam_over  = true;
            zoom_mul *= (input->wheel_y > 0.0f) ? 1.1f : (1.0f / 1.1f);
            zoom_mul  = MU_CLAMP(zoom_mul, 0.2f, 8.0f);
        }

        if (playing)
        {
            t += input->dt * speed;
            if (t > dur)
                t = 0.0f;
        }

        anim_seek(tl, scene, t);
        if (cam_over)
        {
            AnimCamera cam = anim_scene_camera(scene);
            preview_orbit_cam(&cam, orbit_az, orbit_el);
            cam.zoom *= zoom_mul;
            anim_scene_set_camera(scene, cam);
        }

        if (mu_input_key_pressed(input, MU_KEY_F2))
        {
            char name[64];
            snprintf(name, sizeof name, "mu_anim_shot_%03u.png", shot_n++);
            anim_export_png(r, scene, &opts, name);
            log_info("saved %s at t=%.2fs", name, (double)t);
        }

        opts.hud_progress = MU_CLAMP(t / dur, 0.0f, 1.0f);
        opts.hud_playing  = playing;
        if (frame_begin(gpu))
        {
            anim_render_to_frame(r, scene, &opts);
            frame_end(gpu);
        }
    }
}

struct AnimContext {
    Platform     *platform;
    Gpu          *gpu;
    AnimRenderer *renderer;
};

AnimContext *anim_context_create(uint32_t width, uint32_t height, const char *title, bool headless, bool validate)
{
    AnimContext *ctx = (AnimContext *)calloc(1, sizeof *ctx);
    uint32_t     w   = width ? width : ANIM_FRAME_PX_W;
    uint32_t     h   = height ? height : ANIM_FRAME_PX_H;

    MU_ASSERT(ctx != NULL);
    ctx->platform = platform_create(&(PlatformDesc){
        .title    = title ? title : "mu_anim",
        .width    = w,
        .height   = h,
        .headless = headless,
    });
    ctx->gpu = gpu_create(&(GpuDesc){
        .name                 = title ? title : "mu_anim",
        .width                = w,
        .height               = h,
        .enable_validation    = validate,
        .size_of_cpu_pool     = MU_MB(32),
        .size_of_gpu_pool     = MU_MB(32),
        .size_of_staging_pool = MU_MB(48),
        .vsync                = true,
    });
    if (!headless)
        gpu_set_surface(ctx->gpu, platform_create_surface(ctx->platform, gpu_raw_instance(ctx->gpu)));
    ctx->renderer = anim_renderer_create(ctx->gpu, w, h);
    return ctx;
}

void anim_context_destroy(AnimContext *ctx)
{
    if (!ctx)
        return;
    gpu_wait_idle(ctx->gpu);
    anim_renderer_destroy(ctx->renderer);
    gpu_destroy(ctx->gpu);
    platform_destroy(ctx->platform);
    free(ctx);
}

AnimRenderer *anim_context_renderer(AnimContext *ctx) { return ctx ? ctx->renderer : NULL; }
Gpu          *anim_context_gpu(AnimContext *ctx)      { return ctx ? ctx->gpu : NULL; }
Platform     *anim_context_platform(AnimContext *ctx) { return ctx ? ctx->platform : NULL; }
const char   *anim_context_gpu_error(const AnimContext *ctx) { return (ctx && ctx->gpu) ? gpu_error(ctx->gpu) : ""; }
