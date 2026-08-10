# GRAPE ARCHITECTURE DOCS
This document describes how GRAPE works, how the code base is structured and the underlying architecture.

## GRAPE firmware
### Structure
The firmware for GRAPE is built in ESP-IDF and currently tested in v5.5.4. The firmware root directory
consists of the main file `main.c` and the `components/` directory.

`main.c` currently houses demos used for in-development project testing but will be used later to house the main loop
that will invoke functions to operate GRAPE, such as the `gfxlink` and the graphics loop

The `components/` directory houses most of GRAPE's code. 

### Surfaces, Textures and the Framebuffer
GRAPE's main unit of graphics are surfaces, code for which is located in `components/grape/src/grape_surface.c`
Each surface has its own texture. This is what's actually getting drawn on the screen. 
When drawing, the pixels get computed and put into the `framebuffer`. Notice: only the parts of the screen
that changed (which are going to be referred to as "dirty regions") get re-drawn each frame. The rest of the 
framebuffer stays static. 