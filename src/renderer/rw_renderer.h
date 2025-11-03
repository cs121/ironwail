#pragma once

#ifndef RW_RENDERER_DLL_READY
#define RW_RENDERER_DLL_READY 1
#endif

#if !defined(RW_RENDERER_LINK_STATIC) && !defined(RW_RENDERER_LINK_DYNAMIC)
#define RW_RENDERER_LINK_STATIC 1
#endif

#if defined(RW_RENDERER_LINK_STATIC) && defined(RW_RENDERER_LINK_DYNAMIC)
#error "RW_RENDERER_LINK_STATIC and RW_RENDERER_LINK_DYNAMIC cannot both be defined"
#endif

#ifdef _WIN32
 #ifdef RW_RENDERER_EXPORTS
  #define RW_API __declspec(dllexport)
 #else
  #define RW_API __declspec(dllimport)
 #endif
 #define RW_CALL __cdecl
#else
 #define RW_API
 #define RW_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RW_RENDERER_VERSION_MAJOR 0
#define RW_RENDERER_VERSION_MINOR 1
#define RW_RENDERER_VERSION_PATCH 0

struct rw_renderer_t;
typedef struct rw_renderer_t rw_renderer_t;

typedef struct rw_renderer_version_t {
    int major;
    int minor;
    int patch;
} rw_renderer_version_t;

typedef enum rw_renderer_status_t {
    RW_OK = 0,
    RW_ERR_GENERIC = -1,
    RW_ERR_INVALID_ARG = -2,
    RW_ERR_NOT_READY = -3,
    RW_ERR_UNSUPPORTED_VERSION = -4,
    RW_ERR_UNSUPPORTED_BACKEND = -5
} rw_status_t;

typedef enum rw_renderer_backend_t {
    RW_RENDERER_BACKEND_AUTO = 0,
    RW_RENDERER_BACKEND_OPENGL = 1,
    // TODO(dll): add Vulkan, Metal, and other backends when dynamic loading is implemented.
} rw_renderer_backend_t;

typedef struct rw_renderer_config_t {
    rw_renderer_version_t version;
    int width;
    int height;
    rw_renderer_backend_t backend;
    void *platform_data; // TODO(dll): replace with backend specific configuration payload.
} rw_renderer_config_t;

RW_API const rw_renderer_version_t *RW_CALL rw_renderer_get_version(void);

RW_API rw_renderer_t *RW_CALL rw_renderer_create(void);
RW_API void           RW_CALL rw_renderer_destroy(rw_renderer_t *renderer);
RW_API rw_status_t    RW_CALL rw_renderer_init(rw_renderer_t *renderer, const rw_renderer_config_t *config);
RW_API rw_status_t    RW_CALL rw_renderer_resize(rw_renderer_t *renderer, int width, int height);
RW_API rw_status_t    RW_CALL rw_renderer_begin_frame(rw_renderer_t *renderer);
RW_API rw_status_t    RW_CALL rw_renderer_end_frame(rw_renderer_t *renderer);
RW_API rw_status_t    RW_CALL rw_renderer_present(rw_renderer_t *renderer);

RW_API rw_status_t    RW_CALL rw_renderer_get_framebuffer_info(const rw_renderer_t *renderer, int *x, int *y, int *width, int *height);

// TODO(dll): resource APIs will forward to dynamically loaded implementations once available.
RW_API rw_status_t    RW_CALL rw_renderer_create_texture(rw_renderer_t *renderer, void **handle_out);
RW_API rw_status_t    RW_CALL rw_renderer_destroy_texture(rw_renderer_t *renderer, void *handle);
RW_API rw_status_t    RW_CALL rw_renderer_create_shader(rw_renderer_t *renderer, void **handle_out);
RW_API rw_status_t    RW_CALL rw_renderer_destroy_shader(rw_renderer_t *renderer, void *handle);
RW_API rw_status_t    RW_CALL rw_renderer_create_buffer(rw_renderer_t *renderer, void **handle_out);
RW_API rw_status_t    RW_CALL rw_renderer_destroy_buffer(rw_renderer_t *renderer, void *handle);

// Facade helpers for the current monolithic build.
RW_API void           RW_CALL rw_renderer_set_active(rw_renderer_t *renderer);
RW_API rw_renderer_t *RW_CALL rw_renderer_get_active(void);
RW_API rw_status_t    RW_CALL rw_renderer_begin_frame_active(int *x, int *y, int *width, int *height);
RW_API rw_status_t    RW_CALL rw_renderer_end_frame_active(void);
RW_API rw_status_t    RW_CALL rw_renderer_present_active(void);

static inline rw_renderer_config_t rw_renderer_config_default(void)
{
    rw_renderer_config_t cfg;
    cfg.version.major = RW_RENDERER_VERSION_MAJOR;
    cfg.version.minor = RW_RENDERER_VERSION_MINOR;
    cfg.version.patch = RW_RENDERER_VERSION_PATCH;
    cfg.width = 0;
    cfg.height = 0;
    cfg.backend = RW_RENDERER_BACKEND_OPENGL;
    cfg.platform_data = NULL;
    return cfg;
}

static inline void rw_renderer_active_begin_frame(int *x, int *y, int *width, int *height)
{
    (void)rw_renderer_begin_frame_active(x, y, width, height);
}

static inline void rw_renderer_active_end_frame(void)
{
    (void)rw_renderer_end_frame_active();
}

static inline void rw_renderer_active_present(void)
{
    (void)rw_renderer_present_active();
}

#ifdef __cplusplus
}
#endif

