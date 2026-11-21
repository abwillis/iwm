# Changes

## 0.7

- Added `/WPA auto|wpa|wpa2|wpa3|transition` to generate a temporary `wpa_supplicant` configuration and launch `WPA_SUPPLICANT.EXE`.
- Added `/PSK`, `/SAE`, `/CCMP`, `/TKIP`, `/PMF`, and `/HIDDEN` generated-policy controls.
- Made WPA/WPA2 auto negotiation the default generated policy when `/WPA auto` is selected.
- Added `/WPACONF file` for a pre-existing persistent configuration that is never modified or deleted.
- Generated files are deleted by the background command session after `WPA_SUPPLICANT.EXE` exits, or immediately if launch fails.
- WPA launch uses XWLAN's `-i lanN -c file` form and still requires GenMAC link/media/current-SSID acknowledgement before DHCP.
- Added strict WPA option compatibility, credential, cipher, PMF, SSID, and safe-path validation.
- Added WPA, WPA2, WPA3-SAE, transition, persistent-file, cleanup, and invalid-combination tests.

## 0.6

- Extended `/SD` to support `/SSID "name" /DISC /INT lanN /SD`.
- After verified disassociation, runs `dhcpmon -r`, `dhcpmon -t`, `ifconfig lanN down`, and `ifconfig lanN delete`.
- Does not run `DHCPSTRT` in disconnect cleanup mode.
- Requires `/INT` and `/SD` together when used with `/DISC`; rejects either one alone.
- Preserves exact-current-SSID verification before any disassociation or interface cleanup.
- Added cleanup-order, no-restart, quiet, dash/slash, and invalid-combination tests.

## 0.5

- Added `/DISC` and `-DISC` disassociate mode, used as `/SSID "name" /DISC`.
- Verifies that the adapter is associated with the exact requested SSID before issuing `OID_802_11_DISASSOCIATE`.
- Waits for link status, media status, and current SSID to confirm link-down.
- Prevents DHCP and interface changes in disassociate mode.
- Rejects `/DISC` with `/INT`, `/SCAN`, `/SD`, `/DHCP`, or `/NOSCAN`.
- Added success, quiet, dash/slash, mixed-prefix, mismatch, missing-SSID, and option-conflict tests.

## 0.4

- Added OS/2-style `/OPTION` support for every named parameter.
- Retained equivalent `-OPTION` support; slash and dash forms may be mixed.
- Kept option matching case-insensitive, including `/?`, `/H`, and `/HELP`.
- Updated help text and examples to show slash notation first.
- Added slash-only, dash-only, mixed-prefix, lowercase, help, scan, and `/SD` tests.

## 0.3

- Added opt-in `-SD` mode for stale classic OS/2 DHCP state.
- When `-SD` is supplied, runs `dhcpmon -r`, `dhcpmon -t`, `ifconfig lanN down`, and `ifconfig lanN delete` before `dhcpstrt -i lanN`.
- Made the validated `-INT` value the only source of `lanN` in the cleanup commands.
- Made `-SD` force `DHCPSTRT` and reject incompatible scan, dhclient, and no-DHCP combinations.
- Added executable-presence checks, cleanup warnings, sequencing tests, and documentation.

## 0.2

- Added standalone `-SCAN` mode to request a scan and print SSID, BSSID, channel, RSSI, frequency, and security state.
- Made `-SCAN -NOSCAN` list the driver's existing scan cache without requesting a new scan.
- Rejected `-SCAN` when combined with `-SSID` or `-INT`.
- Added strict validation for the variable-length `NDIS_802_11_BSSID_LIST_EX` response.
- Added fresh-cache, existing-cache, and mutually-exclusive argument tests.

## 0.1

- Added `-SSID` and `-INT` command-line parsing.
- Added GenMAC open-network setup and optional scan.
- Added synchronous SSID-request acknowledgement checks.
- Added link-status, media-status, current-SSID, BSSID, and RSSI confirmation.
- Prevented DHCP startup unless the requested association is confirmed.
- Added automatic ISC `DHCLIENT.EXE` selection with classic `DHCPSTRT.EXE` fallback.
- Added configurable timeout, polling, device, DHCP mode, and no-scan options.
- Added a host-only flow test.
