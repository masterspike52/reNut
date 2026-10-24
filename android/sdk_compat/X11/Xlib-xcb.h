#pragma once
// See xcb/xcb.h.
#include <xcb/xcb.h>

typedef struct _XDisplay Display;

#ifdef __cplusplus
extern "C" {
#endif
xcb_connection_t *XGetXCBConnection(Display *dpy);
#ifdef __cplusplus
}
#endif
