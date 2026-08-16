# GRAPE

...Wow, such empty...

## TODO:
- [ ] Explain the occupancy map in ARCHITECTURE.md
- [ ] Re-test PPA-rotated Y shear on ESP32-P4 rev 3.x (GRAY8 SRM is unavailable on the current pre-v3 chip)
  - TEST THIS ONCE CHIP V3.X ARRIVES!
- [ ] Merge the different debug options into one unified system
- [ ] Tackle watchdog and vtaskdelay bs so we never get this error ever again
- [ ] Make a unified surface create thingy
  - surface config
  - one function
  - multiple shaders
  - surfaces supporting width/height not determined by the texture 
  - texture stretching modes (stretch, tile, fit, etc.)
  - [ ] Update ARCHITECTURE.md
- [ ] Effect shaders
- [ ] Builtin shaders
  - Anti-aliasing
  - Brightness
  - Color filter thingies (sepia, something like that)

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
- GFXLINK
  - USB
  - SPI
- Graphics driver port for PC
- OpenGL port for ESP32
- 