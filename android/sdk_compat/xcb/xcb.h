#pragma once
// No X11 on Android. The SDK's SDL window code (src/ui/window_sdl.cpp,
// surface_gnulinux.cpp) is compiled there all the same, though never reached
// (SDL reports no X11 display): what it names, stubbed in
// android_sdk_compat.cpp.
#include <stdint.h>

typedef struct xcb_connection_t xcb_connection_t;
typedef struct xcb_generic_error_t xcb_generic_error_t;
typedef uint32_t xcb_window_t;
typedef uint32_t xcb_drawable_t;
typedef struct { unsigned int sequence; } xcb_get_geometry_cookie_t;
typedef struct { uint16_t width, height; } xcb_get_geometry_reply_t;

#ifdef __cplusplus
extern "C" {
#endif
xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t *c, xcb_drawable_t drawable);
xcb_get_geometry_reply_t *xcb_get_geometry_reply(xcb_connection_t *c, xcb_get_geometry_cookie_t cookie,
                                                 xcb_generic_error_t **e);
#ifdef __cplusplus
}
#endif
