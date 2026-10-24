#pragma once
// Default settings by platform: phones start at the game's own quality,
// desktops above it.
#if defined(__ANDROID__)
#define RENUT_DEFAULT(desktop, handheld) handheld
#else
#define RENUT_DEFAULT(desktop, handheld) desktop
#endif
