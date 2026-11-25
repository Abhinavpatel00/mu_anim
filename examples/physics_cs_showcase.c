/*
 * examples/physics_cs_showcase.c — Physics & Computer Science toolkit demo.
 * Demonstrates how concise Physics & CS videos are with anim_phys_* and anim_cs_*.
 *
 * Usage:
 *   ./physics_cs_demo                         Interactive preview
 *   ./physics_cs_demo --shot out.png --time 2 Headless PNG shot
 *   ./physics_cs_demo --video physics_cs.mp4  Headless MP4 export
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

/* Damped harmonic oscillator ODE: y = [theta, omega] */
static void damped_osc_ode(float t, const float *y, float *dydt, uint32_t dim, void *user)
{
    (void)t;
    (void)dim;
    (void)user;
    dydt[0] = y[1];
    dydt[1] = -9.0f * y[0] - 0.55f * y[1];
}

static AnimVec3 phase_proj(const float *y, uint32_t dim, void *user)
{
    (void)dim;
    (void)user;
    return ANIM_V3(3.2f + y[0] * 1.15f, -0.5f + y[1] * 0.35f, 0.0f);
}

typedef struct SpringPendCtx {
    AnimTrackerId phase;
    AnimId        spring;
    AnimId        mass;
    AnimId        pend;
    AnimId        wave;
} SpringPendCtx;

static void update_spring_mass(AnimScene *s, AnimId target, float dt, void *user)
{
    SpringPendCtx *ctx = (SpringPendCtx *)user;
    float          t   = anim_tracker_get(s, ctx->phase);
    float          x   = -3.2f + 0.85f * cosf(t * 2.2f);
    (void)dt;
    anim_phys_spring_set(s, target, ANIM_V3(-5.5f, 1.3f, 0.0f), ANIM_V3(x - 0.35f, 1.3f, 0.0f), 9u, 0.42f);
    anim_move_to(s, ctx->mass, ANIM_V3(x, 1.3f, 0.0f));
}

static void update_pendulum_and_wave(AnimScene *s, AnimId target, float dt, void *user)
{
    SpringPendCtx *ctx = (SpringPendCtx *)user;
    float          t   = anim_tracker_get(s, ctx->phase);
    float          ang = 0.52f * expf(-0.10f * t) * cosf(t * 2.4f);
    (void)dt;
    anim_phys_pendulum_set(s, target, ANIM_V3(0.1f, 2.2f, 0.0f), 2.1f, ang);
    anim_phys_wave_set(s, ctx->wave, ANIM_V3(-5.5f, -1.7f, 0.0f), ANIM_V3(-0.9f, -1.7f, 0.0f), 0.38f, 3.0f, -t * 3.5f, 80u);
}

static void build_physics_cs_scene(AnimScene *s, AnimTimeline *tl, SpringPendCtx *ctx)
{
    /* ---- Act 1: Physics Mechanics (Spring, Pendulum, Wave, RK4 Phase) --- */
    AnimId h1   = anim_text(s, NULL, "Physics: Mechanics, Oscillations & RK4", 0.48f);
    AnimId wall = anim_line(s, ANIM_V3(-5.5f, 0.5f, 0.0f), ANIM_V3(-5.5f, 2.1f, 0.0f));
    AnimId spr  = anim_phys_spring(s, ANIM_V3(-5.5f, 1.3f, 0.0f), ANIM_V3(-2.7f, 1.3f, 0.0f), 9u, 0.42f);
    AnimId mass = anim_rounded_rect(s, 0.70f, 0.70f, 0.08f);
    AnimId pend = anim_phys_pendulum(s, ANIM_V3(0.1f, 2.2f, 0.0f), 2.1f, 0.52f, 0.22f);
    AnimId wave = anim_phys_wave(s, ANIM_V3(-5.5f, -1.7f, 0.0f), ANIM_V3(-0.9f, -1.7f, 0.0f), 0.38f, 3.0f, 0.0f, 80u);
    float  y0[2] = { 1.4f, 0.0f };
    AnimId rk4  = anim_phys_trajectory(s, y0, 2u, 0.0f, 6.0f, 120u, damped_osc_ode, phase_proj, NULL);
    AnimId rk_lbl = anim_text(s, NULL, "RK4 Phase Spiral", 0.28f);
    AnimTrackerId ph = anim_tracker_create(s, 0.0f);

    anim_set_color(s, h1, ANIM_BLUE_A, true);
    anim_to_edge(s, h1, ANIM_UP, 0.38f);
    anim_set_stroke(s, wall, ANIM_GREY_B, 3.5f, false);
    anim_move_to(s, mass, ANIM_V3(-2.35f, 1.3f, 0.0f));
    anim_set_fill(s, mass, ANIM_BLUE_D, false);
    anim_set_stroke(s, mass, ANIM_WHITE, 2.0f, false);
    anim_set_stroke(s, rk4, ANIM_GOLD, 3.0f, false);
    anim_set_glow(s, rk4, 0.6f, false);
    anim_move_to(s, rk_lbl, ANIM_V3(3.2f, -2.5f, 0.0f));
    anim_set_color(s, rk_lbl, ANIM_GOLD, true);

    {
        AnimSpec a1[8] = {
            anim_anim_write(h1),
            anim_anim_create(wall),
            anim_anim_create(spr),
            anim_anim_fade_in(mass, ANIM_ORIGIN, 0.9f),
            anim_anim_fade_in(pend, ANIM_ORIGIN, 0.95f),
            anim_anim_create(wave),
            anim_anim_create(rk4),
            anim_anim_write(rk_lbl),
        };
        anim_play(tl, s, a1, 8u, 1.0f, ANIM_RATE_SMOOTH);
    }

    ctx->phase  = ph;
    ctx->spring = spr;
    ctx->mass   = mass;
    ctx->pend   = pend;
    ctx->wave   = wave;
    anim_add_updater(s, spr, update_spring_mass, ctx, NULL, 0u, &ph, 1u);
    anim_add_updater(s, pend, update_pendulum_and_wave, ctx, NULL, 0u, &ph, 1u);
    {
        AnimSpec run1 = anim_anim_tracker_to(ph, 4.5f);
        anim_play(tl, s, &run1, 1u, 2.6f, ANIM_RATE_LINEAR);
    }
    anim_clear_updaters(s, spr);
    anim_clear_updaters(s, pend);

    /* ---- Act 2: Physics EM Field Lines & Optics Ray Tracing --------------- */
    {
        AnimId h2 = anim_text(s, NULL, "Physics: Electric Fields & Thin-Lens Optics", 0.46f);
        AnimVec3 cpos[2] = { ANIM_V3(-4.2f, 0.0f, 0.0f), ANIM_V3(-1.6f, 0.0f, 0.0f) };
        float    qval[2] = { 1.0f, -1.0f };
        AnimId qp   = anim_phys_charge(s, cpos[0], 0.26f,  1);
        AnimId qm   = anim_phys_charge(s, cpos[1], 0.26f, -1);
        AnimId fl   = anim_phys_field_lines(s, cpos, qval, 2u, ANIM_V3(-6.0f, -2.5f, 0.0f), ANIM_V3(-0.2f, 2.5f, 0.0f), 10u);
        AnimId lens = anim_phys_lens(s, ANIM_V3(2.8f, 0.0f, 0.0f), 3.4f, 0.55f, false);
        AnimId rays = anim_phys_ray_bundle(s, ANIM_V3(2.8f, 0.0f, 0.0f), 1.8f, 3.2f, NULL, 7u, 0.4f, 5.6f);
        AnimId fdot = anim_dot(s, ANIM_V3(4.6f, 0.0f, 0.0f), 0.09f);

        anim_set_color(s, h2, ANIM_TEAL, true);
        anim_to_edge(s, h2, ANIM_UP, 0.38f);
        anim_set_fill(s, fdot, ANIM_RED, false);

        {
            AnimSpec to_a2[8] = {
                anim_anim_fade_out(h1, ANIM_UP, 0.9f),
                anim_anim_fade_out(wall, ANIM_ORIGIN, 0.9f),
                anim_anim_fade_out(spr, ANIM_ORIGIN, 0.9f),
                anim_anim_fade_out(mass, ANIM_ORIGIN, 0.9f),
                anim_anim_fade_out(pend, ANIM_ORIGIN, 0.9f),
                anim_anim_fade_out(wave, ANIM_ORIGIN, 0.9f),
                anim_anim_fade_out(rk4, ANIM_ORIGIN, 0.9f),
                anim_anim_fade_out(rk_lbl, ANIM_ORIGIN, 0.9f),
            };
            anim_play(tl, s, to_a2, 8u, 0.65f, ANIM_RATE_SMOOTH);
        }
        {
            AnimSpec show_a2[6] = {
                anim_anim_write(h2),
                anim_anim_create(fl),
                anim_anim_fade_in(qp, ANIM_ORIGIN, 0.8f),
                anim_anim_fade_in(qm, ANIM_ORIGIN, 0.8f),
                anim_anim_fade_in(lens, ANIM_ORIGIN, 0.9f),
                anim_anim_create(rays),
            };
            anim_play(tl, s, show_a2, 6u, 1.5f, ANIM_RATE_SMOOTH);
        }
        anim_add(s, fdot);
        anim_wait(tl, s, 1.0f);

        /* ---- Act 3: CS Sorting Bars + Binary Tree + Linked List --------- */
        {
            AnimId h3 = anim_text(s, NULL, "Computer Science: Sorting, Trees & Lists", 0.46f);
            float  vals[5] = { 4.0f, 1.5f, 5.0f, 2.2f, 3.2f };
            const char *blbls[5] = { "4", "1", "5", "2", "3" };
            AnimId bars[5];
            AnimId chart = anim_cs_bar_chart(s, vals, blbls, 5u, 4.4f, 2.3f, ANIM_BLUE_D, ANIM_TEAL, NULL, bars);
            const char *t_lbls[7] = { "8", "3", "10", "1", "6", NULL, "14" };
            AnimId tree  = anim_cs_binary_tree(s, t_lbls, 7u, 1.15f, 1.5f, 0.28f, NULL, NULL);
            const char *ll_lbls[4] = { "10", "24", "37", "42" };
            AnimId llist = anim_cs_linked_list(s, ll_lbls, 4u, 1.05f, 0.56f, 0.50f, NULL, NULL);

            anim_set_color(s, h3, ANIM_GOLD, true);
            anim_to_edge(s, h3, ANIM_UP, 0.38f);
            anim_move_to(s, chart, ANIM_V3(-3.3f, 0.4f, 0.0f));
            anim_move_to(s, tree, ANIM_V3(3.1f, 0.9f, 0.0f));
            anim_move_to(s, llist, ANIM_V3(0.0f, -2.25f, 0.0f));

            {
                AnimSpec to_a3[7] = {
                    anim_anim_fade_out(h2, ANIM_UP, 0.9f),
                    anim_anim_fade_out(qp, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(qm, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(fl, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(lens, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(rays, ANIM_ORIGIN, 0.9f),
                    anim_anim_fade_out(fdot, ANIM_ORIGIN, 0.9f),
                };
                anim_play(tl, s, to_a3, 7u, 0.65f, ANIM_RATE_SMOOTH);
            }
            {
                AnimSpec show_a3[4] = {
                    anim_anim_write(h3),
                    anim_anim_fade_in(chart, ANIM_UP, 0.95f),
                    anim_anim_create(tree),
                    anim_anim_create(llist),
                };
                anim_play(tl, s, show_a3, 4u, 1.1f, ANIM_RATE_SMOOTH);
            }

            /* Effortless bubble-sort swaps using anim_cs_bar_swap */
            anim_cs_bar_swap(tl, s, bars, 0u, 1u, 0.45f, 0.45f);
            anim_cs_bar_swap(tl, s, bars, 2u, 3u, 0.45f, 0.45f);
            anim_cs_bar_swap(tl, s, bars, 3u, 4u, 0.45f, 0.45f);

            /* ---- Act 4: CS Automata (FSM) + Logic Gates + Neural Net ---- */
            {
                AnimId h4 = anim_text(s, NULL, "CS: Finite Automata, Digital Logic & Neural Nets", 0.44f);
                AnimVec3 fpos[3] = { ANIM_V3(-4.8f, 0.8f, 0.0f), ANIM_V3(-3.0f, 0.8f, 0.0f), ANIM_V3(-1.2f, 0.8f, 0.0f) };
                const char *flbl[3] = { "q0", "q1", "q2" };
                bool     facc[3] = { false, false, true };
                uint32_t ffrm[4] = { 0u, 1u, 1u, 2u };
                uint32_t fto[4]  = { 1u, 0u, 2u, 2u };
                const char *felb[4] = { "1", "0", "0", "1" };
                AnimId fsm  = anim_cs_fsm(s, 3u, fpos, flbl, facc, 0u, 4u, ffrm, fto, felb, 0.38f, NULL);
                AnimId bits = anim_cs_bit_register(s, 0xB5u, 8u, 0.56f, true, NULL, NULL);
                AnimId gand = anim_cs_logic_gate(s, ANIM_GATE_AND, ANIM_V3(0.6f, 1.1f, 0.0f), 0.75f, "AND", NULL);
                AnimId gxor = anim_cs_logic_gate(s, ANIM_GATE_XOR, ANIM_V3(0.6f, -0.2f, 0.0f), 0.75f, "XOR", NULL);
                uint32_t layers[4] = { 3u, 5u, 4u, 2u };
                AnimId nn   = anim_cs_neural_net(s, layers, 4u, 3.6f, 2.8f, 0.15f, ANIM_BLUE, ANIM_GOLD);

                anim_set_color(s, h4, ANIM_BLUE_A, true);
                anim_to_edge(s, h4, ANIM_UP, 0.38f);
                anim_move_to(s, bits, ANIM_V3(-3.0f, -1.8f, 0.0f));
                anim_move_to(s, nn, ANIM_V3(3.8f, 0.1f, 0.0f));

                {
                    AnimSpec to_a4[4] = {
                        anim_anim_fade_out(h3, ANIM_UP, 0.9f),
                        anim_anim_fade_out(chart, ANIM_ORIGIN, 0.9f),
                        anim_anim_fade_out(tree, ANIM_ORIGIN, 0.9f),
                        anim_anim_fade_out(llist, ANIM_ORIGIN, 0.9f),
                    };
                    anim_play(tl, s, to_a4, 4u, 0.65f, ANIM_RATE_SMOOTH);
                }
                {
                    AnimSpec show_a4[6] = {
                        anim_anim_write(h4),
                        anim_anim_create(fsm),
                        anim_anim_fade_in(bits, ANIM_UP, 0.95f),
                        anim_anim_create(gand),
                        anim_anim_create(gxor),
                        anim_anim_fade_in(nn, ANIM_ORIGIN, 0.95f),
                    };
                    anim_play(tl, s, show_a4, 6u, 1.4f, ANIM_RATE_SPRING);
                }
                anim_wait(tl, s, 0.8f);
            }
        }
    }
}

int main(int argc, char **argv)
{
    uint32_t       w = 1280u, h = 720u, fps = 30u, blur = 1u;
    const char    *shot_path  = NULL;
    const char    *video_path = NULL;
    float          shot_time  = -1.0f;
    bool           validate   = false;
    int            i, rc = 0;

    Platform      *platform;
    Gpu           *gpu;
    AnimRenderer  *ren;
    AnimScene     *scene;
    AnimTimeline  *tl;
    AnimRenderOpts opts;
    SpringPendCtx  ctx = { 0 };

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)        shot_path  = argv[++i];
        else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc) video_path = argv[++i];
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc)  shot_time  = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)   fps        = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--blur") == 0 && i + 1 < argc)  blur       = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) validate = atoi(argv[++i]) != 0;
    }

    scene = anim_scene_create();
    tl    = anim_timeline_create();
    build_physics_cs_scene(scene, tl, &ctx);
    log_info("physics_cs_showcase duration = %.2f s", (double)anim_timeline_duration(tl));

    platform = platform_create(&(PlatformDesc){
        .title    = "mu_anim — Physics & CS Showcase",
        .width    = w,
        .height   = h,
        .headless = (shot_path != NULL || video_path != NULL),
    });
    gpu = gpu_create(&(GpuDesc){
        .name                 = "physics_cs_demo",
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

    ren = anim_renderer_create(gpu, w, h);
    opts = anim_render_opts_default(w, h);
    opts.fps = fps;
    opts.motion_blur_samples = MU_MAX(blur, 1u);

    if (shot_path)
    {
        float t = (shot_time >= 0.0f) ? shot_time : 0.5f * anim_timeline_duration(tl);
        anim_seek(tl, scene, t);
        if (!anim_export_png(ren, scene, &opts, shot_path))
            rc = 1;
    }
    else if (video_path)
    {
        if (!anim_export_video(ren, tl, scene, &opts, video_path, NULL))
            rc = 1;
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
