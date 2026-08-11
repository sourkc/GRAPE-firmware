# GRAPE

...Wow such empty...

## TODO:
- [x] USE PPA for rotation and scaling!
- [ ] grape_texture_invalidate_rect(texture, x, y, w, h); (I forgot what this means)
- [ ] Re-test PPA-rotated Y shear on ESP32-P4 rev 3.x (GRAY8 SRM is unavailable on the current pre-v3 chip)
  - TEST THIS ONCE CHIP V3.X ARRIVES!
- [x] Tile-based adaptive damage grouping
- [x] Per-texture occupancy masks for transparent/irregular textures
- [ ] Merge the different debug options into one unified system
- [x] Build a deterministic benchmark suite
- [x] Centralize hardware features and their lookup
- [ ] Tackle watchdog and vtaskdelay bs so we never get this error ever again

## Chip revision overlays

Pre-v3 silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev_pre_v3.defaults" build
```

Rev 3.x silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev3.defaults" build
```

## Planned stuff
- JPEG images (hardware)
- Vector graphics
  - Font rendering
  - Expand SVG feature coverage
- GFXLINK
  - USB
  - SPI
- Graphics driver port for PC
- OpenGL port for ESP32
- 