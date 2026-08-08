# GRAPE Feature API

GRAPE exposes optional implementation capabilities through one feature registry.
Features describe capabilities that may vary by hardware, build, initialization,
or runtime policy; normal rendering operations remain separate APIs.

## State model

Each feature has three distinct pieces of state:

- **available**: the current platform/provider can supply the feature;
- **mode**: `AUTO`, `ENABLED`, or `DISABLED` runtime policy;
- **active**: the feature is currently selected for use.

`ENABLED` is rejected with `ESP_ERR_NOT_SUPPORTED` when a feature is unavailable.
`AUTO` and `DISABLED` are always valid for runtime-toggleable features. Resetting a
feature restores the default mode from the registry.

Availability also carries a machine-readable reason when false, such as
`UNSUPPORTED_HARDWARE` or `INIT_FAILED`.

## Registry

Feature metadata lives in
`components/grape/include/grape/grape_feature_registry.def`. Each entry defines a
stable numeric ID, name, default mode, and flags. IDs are explicit so they can be
used by future GFXLINK protocol messages without depending on enum ordering.

Current features:

- `ppa.fill` - PPA background fill, automatic by default;
- `ppa.a8_blend` - PPA A8 compositing, automatic by default;
- `ppa.a8_rotate` - experimental PPA GRAY8 rotation used by the alternate Y-shear
  path, disabled by default and unavailable on pre-v3 ESP32-P4 silicon.

## Public API

```c
grape_feature_info_t info;
ESP_ERROR_CHECK(
    grape_feature_get_info(
        grape,
        GRAPE_FEATURE_PPA_A8_BLEND,
        &info
    )
);

ESP_ERROR_CHECK(
    grape_feature_disable(
        grape,
        GRAPE_FEATURE_PPA_A8_BLEND
    )
);

ESP_ERROR_CHECK(
    grape_feature_reset(
        grape,
        GRAPE_FEATURE_PPA_A8_BLEND
    )
);
```

Use `grape_feature_count()` plus `grape_feature_get_info_at()` to enumerate the
full registry. `grape_feature_list_available()` provides the compact list needed
for capability discovery.

The renderer checks feature state inside the implementation that owns the
accelerated operation. Callers continue to request rendering operations without
knowing which backend is selected.
