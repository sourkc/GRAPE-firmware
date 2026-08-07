# GRAPE

...Wow such empty...

## TODO:
- [ ] USE PPA for rotation and scaling!
- [ ] grape_texture_invalidate_rect(texture, x, y, w, h);

## Chip revision overlays

Pre-v3 silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev_pre_v3.defaults" build
```

Rev 3.x silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev3.defaults" build
```
