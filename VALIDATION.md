# Validation record

## Completed in the Linux host environment

Command:

```
make test
```

Result: all eight suites built with `-std=c99 -Wall -Wextra -Werror` and passed:

- `test_core`
- `test_fw_fixes`
- `test_fw_image`
- `test_transport`
- `test_oid`
- `test_os2_adapter`
- `test_bar`
- `test_fw_loader`

`test_oid` also covers the XWLAN-observed policy OIDs, validated current MAC
address behavior, and byte-exact serialization of a one-entry
`NDIS_802_11_BSSID_LIST_EX` result.

The same eight suites passed with GCC address and undefined-behavior
sanitizers enabled.  Leak detection was disabled because LeakSanitizer cannot
operate under the container's ptrace wrapper.

## Not executable in this environment

The Drv32/Open Watcom ring-0 build and Intel 9560 hardware run require the OS/2 target machine, Open Watcom, Drv32, MiniDDK, KEE library, and the physical adapter. `makefile.wat` includes `r0_fw_loader.obj` and `r0_transport.obj` and generates the mixed 16/32-bit link file, but target compilation and the new `init load` diagnostic still need to be run on OS/2.

## Safety result

The loader now fails with `IWM_ETIMEDOUT` if the early ALIVE interrupt does not
arrive.  After an early ALIVE it intentionally leaves the firmware session in
`IWM_FW_LOADING`; it cannot transition to `IWM_FW_RUNNING` until the RX
notification queue parses a valid ALIVE command. This keeps hardware scan and
association paths gated while their queues are not implemented.


