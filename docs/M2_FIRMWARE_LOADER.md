# M2 FH firmware loader

## What this revision adds

- `core/iwm_fw_loader.c` implements the Intel 8000/9000 secure firmware-loading sequence through FH service channel 9.
- A 16 KiB aligned DMA32 bounce block is allocated by the OS layer. Each cataloged section is copied from the validated firmware store into that block and transferred with a bounded five-second completion wait.
- CPU1/CPU2 section selection uses the firmware's CPU separator. API-46 INIT sections that occur after the runtime paging marker remain assigned to CPU2; runtime paging blocks are retained for the later paging command instead of being sent as ordinary boot sections.
- Extended SRAM addressing is enabled only around chunks in `0x40000..0x57fff`.
- Secure CPU reset release, CPU2 header location, per-section load-status bits, full interrupt enable, and early ALIVE interrupt capture are implemented.
- The Drv32 layer now supplies real `udelay`, aligned contiguous DMA allocation/free, and the ISR-to-loader event path.

## Deliberate safety boundary

An `ALIVE` interrupt bit is not sufficient proof that the firmware protocol is usable. The driver therefore does **not** call `iwm_adapter_fw_alive()` and does not expose scan as operational. Firmware stays in `IWM_FW_LOADING` until the RX notification queue receives and validates the ALIVE command payload.

This avoids a false-ready state in which XWLAN could request scan while command and notification queues are still absent.

## Expected first target log

After `IWMFWLD.EXE` commits a valid API-46 image, the driver should print a line shaped like:

```
iwm: init load rc 0 stage 7 sections 9 bytes N dst XXXXXXXX aliveirq 1
```

The exact section and byte counts come from the image. `stage 6` with `aliveirq 0` means DMA section loading and CPU release completed but the early ALIVE bit was not observed within one second. Any negative `rc` is fail-closed: interrupts are masked, adapter state becomes ERROR, and the staged image is discarded.

## Next code slice

1. Add the 9560 multi-queue RX free/used/status structures and 4 KiB receive buffers.
2. Program RFH queue 0 before releasing secure CPU boot.
3. Drain RX packets outside hard-interrupt context and validate the API-46 ALIVE payload/status.
4. Only then call `iwm_adapter_fw_alive()`, initialize the command queue, read NVM, and send PHY configuration.
5. Stop INIT firmware and repeat the load for runtime sections, retaining paging blocks for `FW_PAGING_BLOCK_CMD`.

## Validation

Host-side validation is in `tests/test_fw_loader.c`. It checks CPU partitioning, runtime-paging exclusion, DMA transfer completion, early ALIVE capture, and the fail-safe rule that the firmware session remains `LOADING` without an RX ALIVE notification.

