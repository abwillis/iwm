# Validation record

## Completed in the Linux host environment

Command:

```
make test
```

Result: all nine suites built with `-std=c99 -Wall -Wextra -Werror` and passed:

- `test_core`
- `test_fw_fixes`
- `test_fw_image`
- `test_transport`
- `test_oid`
- `test_os2_adapter`
- `test_bar`
- `test_fw_loader`
- `test_rx`

`test_oid` also covers the XWLAN-observed policy OIDs, validated current MAC
address behavior, and byte-exact serialization of a one-entry
`NDIS_802_11_BSSID_LIST_EX` result.

`test_rx` covers DMA32 allocation/alignment, RFH programming, descriptor
recycling, malformed frame rejection, and API-46 ALIVE parsing.

The same nine suites passed with GCC address and undefined-behavior
sanitizers enabled.  Leak detection was disabled because LeakSanitizer cannot
operate under the container's ptrace wrapper.

## Not executable in this environment

+The Drv32/Open Watcom ring-0 build and Intel 9560 hardware run require the OS/2 target machine, Open Watcom, Drv32, MiniDDK, KEE library, and the physical adapter. `makefile.wat` includes `r0_fw_loader.obj`, `r0_transport.obj`, and `r0_rx.obj` and generates the mixed 16/32-bit link file, but target compilation and the new RX/ALIVE diagnostics still need to be run on OS/2.

## Safety result

The loader now fails with `IWM_ETIMEDOUT` unless RFH queue 0 receives a valid
ALIVE command with status `0xcafe`. The CSR ALIVE cause is diagnostic only.
After INIT ALIVE the firmware session intentionally remains in
`IWM_FW_LOADING`; it cannot transition to `IWM_FW_RUNNING` until the command
queue, NVM/PHY sequence, runtime image, and runtime ALIVE are implemented. This
keeps hardware scan and association gated instead of fabricating readiness.


