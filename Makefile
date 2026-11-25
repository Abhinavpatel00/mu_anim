CC      ?= cc
AR      ?= ar
PREFIX  ?= /usr/local
CFLAGS  ?= -O2 -g
WARN    := -std=c11 -Wall -Wextra -Werror -fno-strict-aliasing
MU_NEW  := third_party/mugfx/desktop
INCLUDE := -I$(MU_NEW)/external/vulkan/include -I$(MU_NEW) -I.
LIBS    := -lm -ldl -lpthread
DEPEND  := -MMD -MP

GLFW_SRC := context init input monitor platform vulkan window egl_context osmesa_context \
            null_init null_monitor null_window null_joystick \
            posix_module posix_time posix_thread posix_poll linux_joystick

ifeq ($(WAYLAND),1)
WINDOW_SYSTEM  := wayland
GLFW_SRC       += wl_init wl_monitor wl_window
GLFW_FLAGS     := -D_GLFW_WAYLAND -DHAVE_MEMFD_CREATE -Ibuild/wayland
PLATFORM_FLAGS := -DPLATFORM_NO_LIBDECOR
else
WINDOW_SYSTEM  := x11
GLFW_SRC       += x11_init x11_monitor x11_window xkb_unicode glx_context
GLFW_FLAGS     := -D_GLFW_X11
PLATFORM_FLAGS :=
endif

GLFW_OBJ := $(GLFW_SRC:%=build/glfw/%.o)
LIB_OBJ  := build/anim.o build/anim_svg.o build/anim_pcs.o build/anim_extra.o build/anim_render.o build/anim_script.o build/vk.o build/mu.o

all: libmu_anim.a mu_anim anim_dsl

lib: libmu_anim.a

libmu_anim.a: $(LIB_OBJ) build/platform.o $(GLFW_OBJ)
	$(AR) rcs $@ $^

mu_anim: build/main.o libmu_anim.a
	$(CC) $^ -o $@ $(LIBS)

test_anim: build/test_anim.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

physics_cs_demo: build/physics_cs_showcase.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

graphics_pipeline_demo: build/graphics_pipeline_explainer.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

uniform_circular_motion_demo: build/uniform_circular_motion.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

quaternions_demo: build/quaternions_viz.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

noise_viz_demo: build/noise_algorithms_viz.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

threed_demo: build/threed_transitions_showcase.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

anim_dsl: build/anim_dsl_cli.o build/platform.o $(LIB_OBJ) $(GLFW_OBJ)
	$(CC) $^ -o $@ $(LIBS)

examples: physics_cs_demo graphics_pipeline_demo uniform_circular_motion_demo quaternions_demo noise_viz_demo threed_demo anim_dsl

build/main.o: main.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/physics_cs_showcase.o: examples/physics_cs_showcase.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/graphics_pipeline_explainer.o: examples/graphics_pipeline_explainer.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/uniform_circular_motion.o: examples/uniform_circular_motion.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/quaternions_viz.o: examples/quaternions_viz.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/noise_algorithms_viz.o: examples/noise_algorithms_viz.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/threed_transitions_showcase.o: examples/threed_transitions_showcase.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/anim_dsl_cli.o: examples/anim_dsl_cli.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/test_anim.o: tests/test_anim.c anim.h anim_script.h Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/anim.o: anim.c anim.h anim_internal.h Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/anim_svg.o: anim_svg.c anim.h anim_internal.h Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/anim_pcs.o: anim_pcs.c anim.h anim_internal.h Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/anim_extra.o: anim_extra.c anim.h anim_internal.h Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/anim_render.o: anim_render.c anim.h anim_internal.h Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) -DMU_ANIM_SHADER_DIR=\"$(PREFIX)/share/mu_anim/shaders\" $(DEPEND) -c $< -o $@

build/anim_script.o: anim_script.c anim_script.h anim.h anim_internal.h Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/vk.o: $(MU_NEW)/vk.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(DEPEND) -c $< -o $@

build/platform.o: $(MU_NEW)/platform.c Makefile build/window-system
	$(CC) $(CFLAGS) $(WARN) $(INCLUDE) $(PLATFORM_FLAGS) $(DEPEND) -c $< -o $@

build/mu.o: $(MU_NEW)/external/mu_lib/mu.c Makefile | build
	$(CC) $(CFLAGS) $(WARN) $(DEPEND) -c $< -o $@

build/glfw/%.o: $(MU_NEW)/external/glfw/src/%.c Makefile build/window-system | build/glfw
	$(CC) $(CFLAGS) -std=c99 -w $(GLFW_FLAGS) -D_DEFAULT_SOURCE -I$(MU_NEW)/external/glfw/include $(DEPEND) -c $< -o $@

build/window-system: FORCE | build
	@echo $(WINDOW_SYSTEM) | cmp -s - $@ || echo $(WINDOW_SYSTEM) > $@

build build/glfw build/wayland:
	mkdir -p $@

check: test_anim
	./test_anim

shot: mu_anim
	./mu_anim --shot demo_shot.png --validate 1

video: mu_anim
	./mu_anim --video demo.mp4

clean:
	rm -rf build libmu_anim.a mu_anim.pc mu_anim test_anim physics_cs_demo graphics_pipeline_demo uniform_circular_motion_demo quaternions_demo noise_viz_demo threed_demo anim_dsl

mu_anim.pc: Makefile
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: mu_anim\nDescription: C11 + Vulkan + Slang mathematical & technical animation engine and .anim DSL\nVersion: 1.0.0\nLibs: -L$${libdir} -lmu_anim %s\nCflags: -I$${includedir}/mu_anim\n' "$(PREFIX)" "$(LIBS)" > $@

install: libmu_anim.a anim_dsl mu_anim.pc
	install -d "$(DESTDIR)$(PREFIX)/lib/pkgconfig" "$(DESTDIR)$(PREFIX)/include/mu_anim" "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(PREFIX)/share/mu_anim/shaders"
	install -m 644 libmu_anim.a "$(DESTDIR)$(PREFIX)/lib/libmu_anim.a"
	install -m 644 anim.h anim_script.h "$(DESTDIR)$(PREFIX)/include/mu_anim/"
	install -m 644 mu_anim.pc "$(DESTDIR)$(PREFIX)/lib/pkgconfig/mu_anim.pc"
	install -m 755 anim_dsl "$(DESTDIR)$(PREFIX)/bin/anim_dsl"
	install -m 644 shaders/anim.*.spv "$(DESTDIR)$(PREFIX)/share/mu_anim/shaders/"

uninstall:
	rm -rf "$(DESTDIR)$(PREFIX)/include/mu_anim" "$(DESTDIR)$(PREFIX)/share/mu_anim" \
	       "$(DESTDIR)$(PREFIX)/lib/libmu_anim.a" "$(DESTDIR)$(PREFIX)/lib/pkgconfig/mu_anim.pc" \
	       "$(DESTDIR)$(PREFIX)/bin/anim_dsl"

shaders:
	for ep in vs_screen fs_bg vs_surface3d fs_surface3d vs_band fs_band fs_band_depth fs_accum_blit fs_bloom_prefilter fs_bloom_down fs_bloom_up fs_composite; do \
		stage=fragment; \
		case $$ep in vs_*) stage=vertex;; esac; \
		slangc shaders/anim.slang -target spirv -fvk-use-entrypoint-name -entry $$ep -stage $$stage -o shaders/anim.$$ep.spv; \
	done

FORCE:

-include $(wildcard build/*.d build/glfw/*.d)

.PHONY: all lib examples check shot video install uninstall clean shaders FORCE
