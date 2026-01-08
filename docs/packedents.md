# Packed entity updates

## Field order
Packed entity updates are sent in the following fixed order. Each field is only present if its bit is set in the mask; fields that are not present fall back to the entity baseline:

1. model (uint16)
2. frame (uint16)
3. colormap (uint8)
4. skin (uint8)
5. effects (uint8)
6. origin x (int16, 12.4 fixed)
7. origin y (int16, 12.4 fixed)
8. origin z (int16, 12.4 fixed)
9. pitch angle (uint16)
10. yaw angle (uint16)
11. roll angle (uint16)
12. velocity x (int16)
13. velocity y (int16)
14. velocity z (int16)
15. alpha (uint8)
16. scale (uint8)
17. lerpfinish (uint8)
18. step flag (no payload; presence bit only)

## Quantization
* Position: 12.4 fixed-point (units * 16), clamped to int16.
* Angles: uint16 mapping 0..65535 -> 0..360 degrees.
* Velocity: int16 with a 1/8 units/sec scale (value * 8), clamped to int16.

## Mask extension
The low 16-bit mask reserves bit 15 as an extension marker (`PACKEDENT_MASK_EXTEND`).
If any upper bits (16-31) are used, the low mask sets the extension bit and the upper
16-bit mask follows immediately.

## Cvars
* `sv_packedents` (0/1): server-side packed entity updates.
* `cl_packedents` (0/1): client advertises packed entity support.
* `sv_packedents_debug` (0/1): enable packed entity statistics logging.

## Notes
* Packed updates are only sent when the server is enabled and the client advertises support.
* To extend the field set, assign new bits above bit 15 and include them in the fixed order.
