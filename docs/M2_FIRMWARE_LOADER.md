# M2 FH firmware loader

## What this revision adds

- `core/iwm_fw_loader.c` implements the Intel 8000/9000 secure firmware-loading sequence through FH service channel 9.
- A 16 KiB aligned DMA32 bounce block is allocated by the OS layer. Each cataloged section is copied from the validated firmware store into that block and transferred with a bounded five-second completion wait.
- CPU1/CPU2 section selection uses the firmware's CPU separator. API-46 INIT sections that occur after the runtime paging marker remain assigned to CPU2; runtime paging blocks are retained for the later paging command instead of being sent as ordinary boot sections.
- Extended SRAM addressing is enabled only around chunks in `0x40000..0x57fff`.
- Secure CPU reset release, CPU2 header location, per-section load-status bits, full interrupt enable, and ALIVE diagnostics are implemented.
- The Drv32 layer now supplies real `udelay`, aligned contiguous DMA allocation/free, and the ISR-to-loader event path.

## Deliberate safety boundary

An `ALIVE` interrupt bit is not sufficient proof that the firmware protocol is usable. RFH queue 0 is now programmed before CPU release and the loader requires an ALIVE packet with status `0xcafe`. The driver still does **not** call `iwm_adapter_fw_alive()` after INIT ALIVE and does not expose scan as operational; command/NVM/PHY initialization and runtime ALIVE remain required.

This avoids a false-ready state in which XWLAN could request scan while command and notification queues are still absent.

## Expected first target log

After `IWMFWLD.EXE` commits a valid API-46 image, the driver should print a line shaped like:

```
+iwm: init load rc 0 stage 6 sections 9 bytes N dst XXXXXXXX aliveirq N aliventf 1 csr XXXXXXXX fh XXXXXXXX load XXXXXXXX ints N rx N bad 0 closed N
```

The exact section and byte counts come from the image. `rc -6 stage 7` with
`aliventf 0` means section loading and CPU release completed but no valid ALIVE
packet reached queue 0 within one second. The `csr`, `fh`, `load`, `rx`, `bad`,
and `closed` fields distinguish interrupt, boot-status, empty-queue, and packet
format failures. Any negative `rc` is fail-closed: interrupts are masked,
adapter state becomes ERROR, and the staged image is discarded.

## Next code slice

1. Use the now-initialized command TX queue to add descriptor construction,
   a synchronous response path, and bounded completion handling.
2. Issue INIT NVM and PHY configuration commands.
3. Stop INIT firmware and repeat the load for runtime sections, retaining paging blocks for `FW_PAGING_BLOCK_CMD`.
4. Validate runtime ALIVE before calling `iwm_adapter_fw_alive()`.
5. Connect scan commands/results, association, keys, and NDIS packet I/O.

## Validation

+Host-side validation is in `tests/test_fw_loader.c` and `tests/test_rx.c`. It checks CPU partitioning, runtime-paging exclusion, DMA transfer completion, RFH setup, receive-buffer recycling, ALIVE validation, timeout failure, and the fail-safe rule that INIT ALIVE alone does not expose scanning.


