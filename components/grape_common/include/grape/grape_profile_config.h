#pragma once

/*
 * GRAPE profiler configuration.
 *
 * All switches are compile-time. Set any individual timing to 0 to remove
 * its timestamping/recording code from the build.
 */

#define GRAPE_PROFILE_ENABLE                    1
#define GRAPE_PROFILE_REPORT_INTERVAL_MS        1000

#define GRAPE_PROFILE_PRESENT                   1
#define GRAPE_PROFILE_DAMAGE_ADD                1
#define GRAPE_PROFILE_SURFACE_TRANSFORM         1
#define GRAPE_PROFILE_SURFACE_RECACHE           1
#define GRAPE_PROFILE_COMPOSITOR                1
#define GRAPE_PROFILE_PPA_FILL                  1
#define GRAPE_PROFILE_CPU_FILL                  1
#define GRAPE_PROFILE_PPA_BLEND_DISPATCH        1
#define GRAPE_PROFILE_PPA_BLEND_HW              1
#define GRAPE_PROFILE_CPU_SURFACE_RASTER        1
#define GRAPE_PROFILE_DISPLAY_BLIT              1
#define GRAPE_PROFILE_LCD_DRAW_SUBMIT           1
#define GRAPE_PROFILE_LCD_DRAW_WAIT             1
