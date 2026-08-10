# GRAPE short math explanation archive
This document describes the underlying math behind some mathematical operations in code (usually linked)

## Affine rotation
Affine rotation inverse-maps each destination pixel into the source
texture using a 2D rotation transform, then samples the source at that
coordinate.

GRAPE uses inverse mapping rather than forwarding source pixels into
the destination because inverse mapping guarantees that every
destination pixel receives a sample.

Rotation is performed around the configured transform origin.

```
sx = dx cos(θ) + dy sin(θ)
sy = -dx sin(θ) + dy cos(θ)

Where:
(dx, dy) is the destination pixel relative to the rotation origin.
(sx, sy) is the corresponding position in the source texture.
```

Warning: this algorithm is usually slower than [three-shear](#three-shear-rotation).

## Three-shear rotation
Three-shear rotation represents a rotation as three simpler image
shears: horizontal, vertical, then horizontal.

GRAPE uses this as an alternative to direct affine sampling because
shears have different computational and memory-access characteristics.

The decomposition uses:

```
x shear: -tan(θ / 2)
y shear:  sin(θ)
x shear: -tan(θ / 2)
```