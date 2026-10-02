# OTA Reference

The Papilio ESP Bootloader listens for OTA requests on TCP port `3232`.
Applications are written to the inactive OTA slot and selected only after the
image has been validated. The `factory` loader remains available for recovery.

The user application should identify itself with an app description or an
identity line such as:

```
PAPILIO_APP name=fpga_companion version=v2.0.0
```

The loader reports the active role and slot through its status endpoint. A
failed status request must be treated as unknown; clients must not reuse a
previously cached role when choosing USB versus OTA.

## Migration note

The Phase-6 layout is not hot-swappable. Flash the published merged image
once over USB at offset `0x0`, then use the loader for normal app updates.

## Recovery

If the board is not reachable over WiFi, use the USB-serial path while the
factory loader is selected. If application code has claimed USB pins, enter
the ESP32-S3 ROM download mode with the board's BOOT and RESET controls first.