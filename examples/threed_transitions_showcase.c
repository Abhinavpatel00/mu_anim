/*
 * examples/threed_transitions_showcase.c — Showcase of 3D camera movements,
 * 3D surface morphing, and 3D transitions in mu_anim.
 *
 * Build & run:
 *   make threed_demo
 *   ./threed_demo --lowres --video threed_showcase.mp4
 */

#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_macros.h"
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_log.h"
#include "../third_party/mugfx/desktop/platform.h"
#include "../third_party/mugfx/desktop/vk.h"
#include "../anim.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *find_font(void)
{
    static const char *const cands[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
    };
    size_t i;
    for (i = 0; i < MU_ARRAY_COUNT(cands); ++i)
    {
        FILE *f = fopen(cands[i], "rb");
        if (f) { fclose(f); return cands[i]; }
    }
    return NULL;
}

static AnimId make_card(AnimScene *s, const char *font_path, const char *title, const char *sub,
                        AnimColor border_col, AnimColor title_col)
{
    AnimId bg = anim_rounded_rect(s, 5.85f, 1.28f, 0.16f);
    AnimId t1 = anim_text(s, font_path, title, 0.27f);
    AnimId t2 = anim_text(s, font_path, sub, 0.18f);
    anim_set_fill(s, bg, anim_rgba(0.005f, 0.009f, 0.022f, 0.90f), false);
    anim_set_stroke(s, bg, border_col, 2.2f, false);
    anim_move_to(s, t1, ANIM_V3(0.0f,  0.22f, 0.03f));
    anim_move_to(s, t2, ANIM_V3(0.0f, -0.24f, 0.03f));
    anim_set_fill(s, t1, title_col, false);
    anim_set_fill(s, t2, anim_hex(0xCBD5E1), false);
    anim_set_z_index(s, bg, 10.0f, false);
    anim_set_z_index(s, t1, 11.0f, true);
    anim_set_z_index(s, t2, 11.0f, true);
    {
        AnimId kids[3] = { bg, t1, t2 };
        return anim_group(s, kids, 3u);
    }
}

static AnimColor surf_teal_gold(float u, float v, AnimVec3 p, AnimVec3 n, void *user)
{
    (void)v; (void)p; (void)n; (void)user;
    float s = 0.5f + 0.5f * sinf(u * ANIM_TAU * 2.0f);
    AnimColor c0 = anim_srgb_to_linear(0.10f, 0.62f, 0.85f, 1.0f);
    AnimColor c1 = anim_srgb_to_linear(0.55f, 0.36f, 0.92f, 1.0f);
    return anim_color_lerp_oklab(c0, c1, s);
}

static AnimVec3 torus_fn(float u, float v, void *user)
{
    (void)user;
    float th = u * ANIM_TAU;
    float ph = v * ANIM_TAU;
    float R = 1.25f, r = 0.42f;
    return ANIM_V3((R + r * cosf(ph)) * cosf(th),
                   r * sinf(ph),
                   (R + r * cosf(ph)) * sinf(th));
}

static AnimVec3 sphere_fn(float u, float v, void *user)
{
    (void)user;
    float th = u * ANIM_TAU;
    float ph = v * ANIM_PI;
    float R  = 1.15f;
    return ANIM_V3(R * sinf(ph) * cosf(th),
                   R * cosf(ph),
                   R * sinf(ph) * sinf(th));
}

static AnimColor torus_col(float u, float v, AnimVec3 p, AnimVec3 n, void *user)
{
    (void)p; (void)n; (void)user;
    float s = 0.5f + 0.5f * cosf((u + v) * ANIM_TAU);
    AnimColor c0 = anim_srgb_to_linear(0.14f, 0.82f, 0.56f, 1.0f);
    AnimColor c1 = anim_srgb_to_linear(0.96f, 0.68f, 0.22f, 1.0f);
    return anim_color_lerp_oklab(c0, c1, s);
}

static void build_scene(AnimScene *s, AnimTimeline *tl, const char *font_path)
{
    anim_scene_set_bg(s, anim_hex(0x000000), anim_hex(0x050812));

    AnimCamera cam0 = anim_camera_default();
    cam0.pos    = ANIM_V3(0.0f, 1.2f, 9.6f);
    cam0.target = ANIM_ORIGIN;
    cam0.fov_deg = 42.0f;
    anim_scene_set_camera(s, cam0);

    AnimColor col_sky   = anim_hex(0x38BDF8);
    AnimColor col_gold  = anim_hex(0xFBBF24);
    AnimColor col_green = anim_hex(0x4ADE80);
    AnimColor col_rose  = anim_hex(0xFB7185);
    AnimColor col_purp  = anim_hex(0xA78BFA);

    /* Background 3D reference axes + floor grid ring */
    AnimId axes = anim_axes3d(s, 3.6f, 2.4f, 3.6f);
    anim_set_opacity(s, axes, 0.45f, true);
    anim_add(s, axes);

    AnimId floor_ring = anim_circle(s, 3.4f);
    anim_rotate(s, floor_ring, 0.5f * ANIM_PI, ANIM_RIGHT);
    anim_move_to(s, floor_ring, ANIM_V3(0.0f, -1.65f, 0.0f));
    anim_set_stroke(s, floor_ring, anim_rgba(0.18f, 0.36f, 0.65f, 0.45f), 2.0f, false);
    anim_add(s, floor_ring);

    /* Center 3D parametric sphere -> torus morph pair */
    AnimId sph = anim_surface3d(s, sphere_fn, surf_teal_gold, 0.0f, 1.0f, 0.0f, 1.0f, 32u, 20u, NULL);
    anim_surface_set_wireframe(s, sph, 12.0f, 0.45f, false);
    anim_surface_set_roughness(s, sph, 0.25f, false);
    anim_move_to(s, sph, ANIM_V3(-1.45f, -0.15f, 0.0f));

    AnimId tor = anim_surface3d(s, torus_fn, torus_col, 0.0f, 1.0f, 0.0f, 1.0f, 32u, 20u, NULL);
    anim_surface_set_wireframe(s, tor, 14.0f, 0.50f, false);
    anim_surface_set_roughness(s, tor, 0.20f, false);
    anim_move_to(s, tor, ANIM_V3(0.0f, -0.10f, 0.0f));

    /* Exploding 3D Icosahedron */
    AnimId ico = anim_polyhedron(s, ANIM_POLY_ICOSAHEDRON, 0.92f);
    anim_set_fill(s, ico, col_purp, false);
    anim_surface_set_wireframe(s, ico, 8.0f, 0.55f, false);
    anim_surface_set_roughness(s, ico, 0.22f, false);
    anim_move_to(s, ico, ANIM_V3(2.25f, -0.15f, 0.0f));

    /* Orbiting satellite + traced path */
    AnimId sat = anim_dot3d(s, ANIM_V3(2.35f, -0.10f, 0.0f), 0.20f);
    anim_set_fill(s, sat, col_gold, false);
    anim_set_glow(s, sat, 0.90f, false);

    /* 3D Camera Path spline for Act 4 */
    AnimId cam_rail = anim_vmob_create(s);
    anim_vmob_begin_subpath(s, cam_rail, ANIM_V3(4.2f, 2.6f, 6.2f));
    anim_vmob_cubic_to(s, cam_rail,
                       ANIM_V3(2.2f, 4.2f, 1.5f),
                       ANIM_V3(-3.5f, 2.2f, -3.5f),
                       ANIM_V3(-4.2f, 1.1f, 6.8f));

    /* 3D Flip-Replace Header Cards */
    AnimId card1 = make_card(s, font_path, "1. 3D Tumble, Flip & Explode",
                             "tumble_in + explode_3d + camera_shake", col_purp, col_sky);
    AnimId card2 = make_card(s, font_path, "2. 3D Surface Morph & Fly-To",
                             "Sphere -> Torus + spherical camera_fly_to", col_green, col_green);
    AnimId card3 = make_card(s, font_path, "3. Swirl 3D & Quaternion Orbit",
                             "swirl_3d + rotate_around + camera_orbit_axis", col_gold, col_gold);
    AnimId card4 = make_card(s, font_path, "4. Dolly Zoom & Camera Rail",
                             "dolly_zoom + camera_path + camera_track", col_rose, col_rose);
    anim_move_to(s, card1, ANIM_V3(0.0f, 2.70f, 0.0f));
    anim_move_to(s, card2, ANIM_V3(0.0f, 2.70f, 0.0f));
    anim_move_to(s, card3, ANIM_V3(0.0f, 2.70f, 0.0f));
    anim_move_to(s, card4, ANIM_V3(0.0f, 2.70f, 0.0f));
    anim_set_billboard(s, card3, true, false);
    anim_set_billboard(s, card4, true, false);
    anim_add(s, card1);

    /* ==================== ACT 1: Tumble-In, Explode-3D & Flip-Replace ==================== */
    {
        AnimSpec s_in[3] = {
            anim_anim_tumble_in(sph, ANIM_V3(-3.5f, 2.2f, -4.0f), ANIM_TAU, ANIM_UR),
            anim_anim_tumble_in(ico, ANIM_V3( 3.5f, 2.0f, -4.0f), -ANIM_TAU, ANIM_UL),
            anim_anim_flip_3d(card1, ANIM_RIGHT, 2.0f)
        };
        anim_play(tl, s, s_in, 3u, 1.35f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec s_exp[3] = {
            anim_spec_with_rate(anim_anim_explode_3d(ico, 1.15f, ANIM_PI * 1.5f), ANIM_RATE_THERE_AND_BACK),
            anim_anim_camera_shake(0.11f, 7.0f),
            anim_anim_rotate(sph, ANIM_PI * 0.7f, ANIM_UP)
        };
        anim_play(tl, s, s_exp, 3u, 1.90f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 0.25f);

    /* ==================== ACT 2: 3D Surface Morph + Spherical Camera Fly-To ==================== */
    {
        AnimCamera c_fly = cam0;
        c_fly.pos    = ANIM_V3(5.8f, 2.8f, 7.2f);
        c_fly.target = ANIM_V3(0.0f, 0.55f, 0.0f);
        AnimSpec s_act2[4] = {
            anim_anim_tumble_out(ico, ANIM_V3(4.0f, -2.5f, -3.0f), ANIM_PI, ANIM_UP),
            anim_anim_transform(sph, tor),
            anim_anim_flip_replace_3d(card1, card2, ANIM_UP),
            anim_anim_camera_fly_to(c_fly, 1.25f)
        };
        anim_play(tl, s, s_act2, 4u, 2.40f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 0.30f);

    /* ==================== ACT 3: Swirl-3D, Rotate-Around & Quaternion Orbit ==================== */
    anim_add(s, sat);
    {
        AnimId sat_trail = anim_traced_path(s, sat, anim_rgba(0.98f, 0.80f, 0.24f, 0.85f), 2.8f, 48u);
        anim_add(s, sat_trail);
        {
            AnimSpec s_act3[4] = {
                anim_anim_flip_replace_3d(card2, card3, ANIM_RIGHT),
                anim_spec_with_rate(anim_anim_swirl_3d(sph, 1.65f, ANIM_UP), ANIM_RATE_THERE_AND_BACK),
                anim_anim_rotate_around(sat, ANIM_V3(0.0f, -0.10f, 0.0f), ANIM_TAU * 1.25f, ANIM_V3(0.15f, 1.0f, 0.20f), true),
                anim_anim_camera_orbit_axis(-1.35f, ANIM_V3(0.28f, 1.0f, 0.15f), 1.05f)
            };
            anim_play(tl, s, s_act3, 4u, 3.00f, ANIM_RATE_SMOOTH);
        }
    }

    /* ==================== ACT 4: Dolly Zoom, Camera Rail & Spring Finale ==================== */
    {
        AnimSpec s_act4[3] = {
            anim_anim_flip_replace_3d(card3, card4, ANIM_UP),
            anim_anim_camera_dolly_zoom(68.0f),
            anim_anim_rotate_around(sat, ANIM_V3(0.0f, -0.10f, 0.0f), ANIM_PI * 0.8f, ANIM_UP, true)
        };
        anim_play(tl, s, s_act4, 3u, 1.90f, ANIM_RATE_SMOOTH);
    }
    {
        AnimSpec s_rail[4] = {
            anim_anim_camera_path(cam_rail, ANIM_V3(0.0f, 0.3f, 0.0f), false),
            anim_anim_camera_track(sph, ANIM_ORIGIN, false),
            anim_anim_spring_move(sat, ANIM_V3(0.0f, 1.35f, 0.0f), 0.45f, 14.0f),
            anim_anim_pulse_glow(sph, 0.85f, 1.12f)
        };
        anim_play(tl, s, s_rail, 4u, 2.30f, ANIM_RATE_SMOOTH);
    }
    anim_wait(tl, s, 0.50f);
}

int main(int argc, char **argv)
{
    const char    *shot_path  = NULL;
    const char    *video_path = NULL;
    float          shot_time  = -1.0f;
    uint32_t       w          = 1280u;
    uint32_t       h          = 720u;
    uint32_t       fps        = 60u;
    uint32_t       blur       = 2u;
    bool           validate   = false;
    int            i;
    int            rc = 0;

    Platform      *platform;
    Gpu           *gpu;
    AnimScene     *scene;
    AnimTimeline  *tl;
    AnimRenderer  *ren;
    AnimRenderOpts opts;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)        shot_path  = argv[++i];
        else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) video_path = argv[++i];
        else if ((strcmp(argv[i], "--time") == 0 || strcmp(argv[i], "--t") == 0) && i + 1 < argc)
            shot_time = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)  w          = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) h          = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--lowres") == 0)                 { w = 854u; h = 480u; fps = 30u; blur = 1u; }
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)   fps        = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--blur") == 0 && i + 1 < argc)  blur       = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) validate = atoi(argv[++i]) != 0;
    }

    scene = anim_scene_create();
    tl    = anim_timeline_create();
    build_scene(scene, tl, find_font());
    log_info("threed_transitions_showcase duration = %.2f s", anim_timeline_duration(tl));

    platform = platform_create(&(PlatformDesc){
        .title    = "mu_anim — 3D Camera & Transitions Showcase",
        .width    = w,
        .height   = h,
        .headless = (shot_path != NULL || video_path != NULL),
    });
    gpu = gpu_create(&(GpuDesc){
        .name                 = "threed_demo",
        .width                = w,
        .height               = h,
        .enable_validation    = validate,
        .size_of_cpu_pool     = MU_MB(32),
        .size_of_gpu_pool     = MU_MB(32),
        .size_of_staging_pool = MU_MB(48),
        .vsync                = true,
    });
    if (!shot_path && !video_path)
        gpu_set_surface(gpu, platform_create_surface(platform, gpu_raw_instance(gpu)));

    ren  = anim_renderer_create(gpu, w, h);
    opts = anim_render_opts_default(w, h);
    opts.fps                 = fps;
    opts.motion_blur_samples = MU_MAX(blur, 1u);
    opts.tonemap             = ANIM_TONEMAP_AGX;
    opts.aberration_px       = 0.35f;
    opts.grain_amount        = 0.0010f;

    if (shot_path)
    {
        float t = (shot_time >= 0.0f) ? shot_time : 0.5f * anim_timeline_duration(tl);
        anim_seek(tl, scene, t);
        if (!anim_export_png(ren, scene, &opts, shot_path)) rc = 1;
    }
    else if (video_path)
    {
        if (!anim_export_video(ren, tl, scene, &opts, video_path, NULL)) rc = 1;
    }
    else
    {
        anim_run_interactive(platform, gpu, ren, tl, scene, opts);
    }

    gpu_wait_idle(gpu);
    if (gpu_error(gpu)[0] != '\0')
    {
        log_error("Vulkan error: %s", gpu_error(gpu));
        rc = 1;
    }

    anim_renderer_destroy(ren);
    gpu_destroy(gpu);
    platform_destroy(platform);
    anim_timeline_destroy(tl);
    anim_scene_destroy(scene);
    return rc;
}
