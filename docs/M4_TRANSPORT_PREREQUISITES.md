# M4: 9000-family transport prerequisites

## Scope

This revision implements the bounded transport prerequisite step only. It does
not submit a host command, correlate a response, read NVM, configure PHY, or
transition to runtime firmware.

The new transport owns:

- one 4 KiB keep-warm page;
- the 31-queue legacy scheduler byte-count allocation (31 x 320 x 2 bytes,
  1024-byte aligned);
- one 256-entry, 128-byte-per-entry command TFD ring (queue 0);
- 32 first-TB slots and 32 reserved command buffers for the next patch;
- deterministic prepare, post-ALIVE scheduler-start, stop and diagnostic paths.

## Linux 9000-family ordering

The sequence deliberately follows the split in Linux `iwl_pcie_nic_init()` and
`iwl_trans_pcie_fw_alive()`:

1. prepare PCIe/APM hardware;
2. allocate and program RX;
3. initialize TX DMA state, stop SCD FIFOs, program keep-warm and CBBC;
4. load INIT firmware and validate the RFH ALIVE notification;
5. only then read `SCD_SRAM_BASE_ADDR`, clear SCD SRAM state, program the
   byte-count-table base, activate command queue 0 on FIFO 7, enable FH TX DMA
   channels, and set the SCD auto-retry chicken bit.

Starting the scheduler before ALIVE would not match Linux. In particular,
`SCD_SRAM_BASE_ADDR` is treated as post-ALIVE state. Therefore an INIT assert
can test the keep-warm/CBBC prerequisite, but not scheduler activation itself.

## Failure diagnostics

If INIT still fails, the OS/2 log now prints one `txdbg` line containing:

- software allocation/prepare/start phase flags;
- `FH_KW_MEM_ADDR_REG` and command queue CBBC;
- FH channel 0 and 7 configuration;
- FH TX chicken bits;
- SCD SRAM and DRAM bases;
- SCD FIFO activation;
- command queue read pointer, write pointer and status;
- HBUS producer pointer.

A pre-ALIVE failure is expected to show `prepared=1`, `started=0`, non-zero
keep-warm and CBBC values, and zero/inactive SCD/FH channel state. That is the
Linux phase boundary, not by itself another failure.

## Tests

`test_tx` exercises allocation geometry, DMA alignment rejection, pre-ALIVE
prepare, post-ALIVE SCD start, command producer bounds, diagnostics and stop.
`test_fw_loader` is stateful: it synthesizes ALIVE only after CPU release, a
programmed RX queue, and visible keep-warm/command-CBBC state. It also verifies
that a failed pre-ALIVE boot captures diagnostics before transport shutdown.
