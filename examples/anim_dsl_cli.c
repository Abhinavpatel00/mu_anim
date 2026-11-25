/*
 * examples/anim_dsl_cli.c — Standalone CLI runner for mu_anim (.anim) DSL scripts.
 *
 * Usage:
 *   ./anim_dsl script.anim                       (interactive window)
 *   ./anim_dsl script.anim --shot out.png --time 2.5
 *   ./anim_dsl script.anim --lowres --video out.mp4
 */

#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_macros.h"
#include "../third_party/mugfx/desktop/external/mu_lib/mu/mu_log.h"
#include "../third_party/mugfx/desktop/platform.h"
#include "../third_party/mugfx/desktop/vk.h"
#include "../anim.h"
#include "../anim_script.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *argv0)
{
    printf(
        "Usage: %s <script.anim> [options]\n"
        "Options:\n"
        "  --shot <path.png>   Render single frame PNG (headless)\n"
        "  --time <sec>        Time in seconds for --shot (default: midpoint)\n"
        "  --video <path.mp4>  Export full timeline MP4/WebM/MOV (headless)\n"
        "  --gif <path.gif>    Export full timeline GIF (headless)\n"
        "  --sections <dir>    Export timeline sections into directory (headless)\n"
        "  --transparent       Transparent background (for PNG/GIF/WebM/MOV)\n"
        "  --audio <path.wav>  Optional audio track for --video\n"
        "  --lowres            854x480 @ 30fps, blur=1\n"
        "  --width <px>        Override width\n"
        "  --height <px>       Override height\n"
        "  --fps <n>           Override FPS\n"
        "  --blur <n>          Override motion blur samples\n"
        "  --validate <0|1>    Enable Vulkan validation layers\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char    *script_path = NULL;
    const char    *shot_path     = NULL;
    const char    *video_path    = NULL;
    const char    *gif_path      = NULL;
    const char    *sections_dir  = NULL;
    const char    *audio_path    = NULL;
    float          shot_time     = -1.0f;
    uint32_t       ovr_w         = 0u;
    uint32_t       ovr_h         = 0u;
    uint32_t       ovr_fps       = 0u;
    uint32_t       ovr_blur      = 0u;
    bool           lowres        = false;
    bool           transparent   = false;
    bool           validate      = false;
    int            i, rc = 0;

    AnimScene     *scene;
    AnimTimeline  *tl;
    AnimScript    *sc;
    AnimScriptOpts sopts;
    AnimRenderOpts opts;
    Platform      *platform;
    Gpu           *gpu;
    AnimRenderer  *ren;

    for (i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)     shot_path    = argv[++i];
        else if (strcmp(argv[i], "--video") == 0 && i + 1 < argc)    video_path   = argv[++i];
        else if (strcmp(argv[i], "--gif") == 0 && i + 1 < argc)      gif_path     = argv[++i];
        else if (strcmp(argv[i], "--sections") == 0 && i + 1 < argc) sections_dir = argv[++i];
        else if (strcmp(argv[i], "--transparent") == 0)              transparent  = true;
        else if (strcmp(argv[i], "--audio") == 0 && i + 1 < argc)    audio_path   = argv[++i];
        else if ((strcmp(argv[i], "--time") == 0 || strcmp(argv[i], "--t") == 0) && i + 1 < argc)
            shot_time = (float)atof(argv[++i]);
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)    ovr_w      = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)   ovr_h      = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc)      ovr_fps    = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--blur") == 0 && i + 1 < argc)     ovr_blur   = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--lowres") == 0)                   lowres     = true;
        else if (strcmp(argv[i], "--validate") == 0 && i + 1 < argc) validate   = atoi(argv[++i]) != 0;
        else if (argv[i][0] != '-' && !script_path)                  script_path = argv[i];
    }

    if (!script_path)
    {
        print_usage(argv[0]);
        return 1;
    }

    scene = anim_scene_create();
    tl    = anim_timeline_create();
    sc    = anim_script_create();
    if (!anim_script_exec_file(sc, scene, tl, script_path, &sopts))
    {
        fprintf(stderr, "DSL Error: %s\n", anim_script_error(sc));
        anim_script_destroy(sc);
        anim_timeline_destroy(tl);
        anim_scene_destroy(scene);
        return 1;
    }

    opts = sopts.render;
    if (lowres) { opts.width = 854u; opts.height = 480u; opts.fps = 30u; opts.motion_blur_samples = 1u; }
    if (ovr_w > 0u)    opts.width  = ovr_w;
    if (ovr_h > 0u)    opts.height = ovr_h;
    if (ovr_fps > 0u)  opts.fps    = ovr_fps;
    if (ovr_blur > 0u) opts.motion_blur_samples = ovr_blur;
    if (transparent)   opts.transparent_bg = true;

    log_info("Loaded '%s': duration = %.2f s (%ux%u @ %u fps)",
             script_path, (double)anim_timeline_duration(tl), opts.width, opts.height, opts.fps);

    platform = platform_create(&(PlatformDesc){
        .title    = script_path,
        .width    = opts.width,
        .height   = opts.height,
        .headless = (shot_path != NULL || video_path != NULL || gif_path != NULL || sections_dir != NULL),
    });
    gpu = gpu_create(&(GpuDesc){
        .name                 = "anim_dsl",
        .width                = opts.width,
        .height               = opts.height,
        .enable_validation    = validate,
        .size_of_cpu_pool     = MU_MB(32),
        .size_of_gpu_pool     = MU_MB(32),
        .size_of_staging_pool = MU_MB(48),
        .vsync                = true,
    });
    if (!shot_path && !video_path && !gif_path && !sections_dir)
        gpu_set_surface(gpu, platform_create_surface(platform, gpu_raw_instance(gpu)));

    ren = anim_renderer_create(gpu, opts.width, opts.height);

    if (shot_path)
    {
        float t = (shot_time >= 0.0f) ? shot_time : 0.5f * anim_timeline_duration(tl);
        anim_seek(tl, scene, t);
        if (!anim_export_png(ren, scene, &opts, shot_path)) rc = 1;
    }
    else if (video_path)
    {
        if (!anim_export_video(ren, tl, scene, &opts, video_path, audio_path)) rc = 1;
    }
    else if (gif_path)
    {
        if (!anim_export_gif(ren, tl, scene, &opts, gif_path)) rc = 1;
    }
    else if (sections_dir)
    {
        if (!anim_export_sections(ren, tl, scene, &opts, sections_dir)) rc = 1;
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
    anim_script_destroy(sc);
    anim_timeline_destroy(tl);
    anim_scene_destroy(scene);
    return rc;
}
