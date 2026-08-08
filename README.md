# GRAPE

...Wow such empty...

## TODO:
- [x] USE PPA for rotation and scaling!
- [ ] grape_texture_invalidate_rect(texture, x, y, w, h); (I forgot what this means)
- [ ] Re-test PPA-rotated Y shear on ESP32-P4 rev 3.x (GRAY8 SRM is unavailable on the current pre-v3 chip)
  - TEST THIS ONCE CHIP V3.X ARRIVES!
- [x] Tile-based adaptive damage grouping
- [x] Per-texture occupancy masks for transparent/irregular textures

## Chip revision overlays

Pre-v3 silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev_pre_v3.defaults" build
```

Rev 3.x silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev3.defaults" build
```

## Debug layers

Debug overlays are rendered after normal surfaces and before the display blit. They do not create normal scene damage, so debug rendering cannot feed back into the damage system it is inspecting.

The first built-in layer visualizes the final logical damage rectangles for the current frame with a translucent red fill and stronger border:

```c
ESP_ERROR_CHECK(
    grape_debug_set_layer_enabled(
        grape,
        GRAPE_DEBUG_LAYER_DAMAGE_RECTS,
        true
    )
);
```

Disable it with the same call and `false`. The previous overlay area is queued as render-only damage so it is repainted once and disappears cleanly. Debug layers are intended for correctness/debugging rather than performance measurements.

## Planned stuff
- JPEG images (hardware)
- Vector graphics
  - Vector surfaces
  - Font rendering
  - Eventually maybe SVG rendering
- GFXLINK
  - USB
  - SPI
- Graphics driver port for PC
- OpenGL port for ESP32
- 