#ifndef MU_ANIM_SCRIPT_H
#define MU_ANIM_SCRIPT_H

/*
 * anim_script.h — Minimal, orthogonal DSL (.anim) for mu_anim.
 * ============================================================================
 *  Features:
 *    - Clean C/Python-like syntax with UFCS method chaining (`.method()`)
 *    - Config blocks (`scene { bg: #000 -> #050812, tonemap: agx, fps: 60 }`)
 *    - Inline Typst math literals (`$bold(r)(t)$`) and vector tuples (`(x, y)`)
 *    - Reactive `:=` bindings compiled into ValueTracker + updater DAG
 *    - Inline lambdas (`x => sin(x)`), `f(x) = ...`, ranges (`0..10:0.5`), CSG
 *    - Synchronized timeline blocks (`play`, `lagged`, `sequence`, `at`, `~dur`)
 * ==========================================================================*/

#include "anim.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AnimScript AnimScript;

typedef struct AnimScriptOpts {
    AnimRenderOpts render;
    bool           has_render_overrides;
} AnimScriptOpts;

AnimScript *anim_script_create(void);
void        anim_script_destroy(AnimScript *sc);

bool anim_script_exec_string(AnimScript *sc, AnimScene *scene, AnimTimeline *tl,
                             const char *source, const char *filename, AnimScriptOpts *out_opts);
bool anim_script_exec_file(AnimScript *sc, AnimScene *scene, AnimTimeline *tl,
                           const char *path, AnimScriptOpts *out_opts);

const char   *anim_script_error(const AnimScript *sc);
AnimId        anim_script_get_mobject(const AnimScript *sc, const char *name);
AnimTrackerId anim_script_get_tracker(const AnimScript *sc, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MU_ANIM_SCRIPT_H */
