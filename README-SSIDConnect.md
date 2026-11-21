# SSIDCONNECT for OS/2

`SSIDCONNECT.EXE` is a small ring-3 CLI for initial testing with Intel IWM/GenMAC and compatible wireless drivers. It can list the driver's visible-SSID cache, connect directly to an open network, launch `WPA_SUPPLICANT.EXE` for WPA-family networks, or disassociate from a verified current SSID. Every connection path waits for a positive link acknowledgement, verifies the driver's current SSID, and only then starts DHCP on the requested OS/2 LAN interface.

Every named option accepts either the customary OS/2 slash prefix or a dash,
case-insensitively. These are equivalent and may be mixed:

```text
ssidconnect.exe /SSID "LWGuest" /INT lan1 /SD
ssidconnect.exe -SSID "LWGuest" -INT lan1 -SD
ssidconnect.exe /SSID "LWGuest" -INT lan1 /SD
```

## Example

```text
ssidconnect.exe /SSID "LWGuest" /INT lan1
```

Expected successful flow:

```text
SSIDCONNECT: requesting open-network association with "LWGuest" via \DEV\WRND32$.
SSIDCONNECT: SSID request acknowledged; waiting for link confirmation.
SSIDCONNECT: connected to "LWGuest"; BSSID xx:xx:xx:xx:xx:xx, RSSI -42 dBm.
SSIDCONNECT: started DHCLIENT.EXE for lan1 (session n).
```

DHCP is never started after an OID failure, SSID mismatch, or association timeout.

## WPA supplicant mode

`SSIDCONNECT` can launch `WPA_SUPPLICANT.EXE` with either a generated temporary
configuration or an existing persistent configuration. It follows XWLAN's
interface form, `WPA_SUPPLICANT.EXE -i lanN -c file`, then waits for the normal
GenMAC link/media/SSID acknowledgement before starting DHCP.

### Generated temporary configuration

The default generated policy requested in this design is WPA/WPA2 automatic
negotiation:

```text
ssidconnect.exe /SSID "LWGuest" /INT lan1 /WPA auto /PSK "passphrase"
```

Other generated policies are explicit:

```text
rem WPA-PSK with TKIP
ssidconnect.exe /SSID "LegacyNet" /INT lan1 /WPA wpa /PSK "passphrase" /TKIP

rem WPA2-PSK/RSN with CCMP
ssidconnect.exe /SSID "SecureNet" /INT lan1 /WPA wpa2 /PSK "passphrase" /CCMP

rem WPA3-SAE with required protected management frames
ssidconnect.exe /SSID "ModernNet" /INT lan1 /WPA wpa3 /SAE "password" /CCMP

rem WPA2-PSK/WPA3-SAE transition network
ssidconnect.exe /SSID "TransitionNet" /INT lan1 /WPA transition \
  /PSK "passphrase" /SAE "password" /CCMP
```

Generated configurations contain `ap_scan=2`, one network block, the requested
SSID, protocol/key-management policy, cipher policy, credentials, and PMF
policy where applicable. `/HIDDEN` adds `scan_ssid=1`. `/CCMP` and `/TKIP`
override the per-mode cipher defaults; both may be supplied for WPA/WPA2 auto
negotiation. WPA3 modes reject TKIP.

The generated file is passed only to `WPA_SUPPLICANT.EXE`. The supplicant is
started in a background OS/2 command session whose next command deletes the
temporary file when the supplicant process exits. If launch fails, the file is
deleted immediately. The PSK or SAE password is not placed on the supplicant's
command line, although a `/PSK` or `/SAE` value is naturally visible on the
original `SSIDCONNECT` command line.

### Existing persistent configuration

Use `/WPACONF` to launch with an existing file:

```text
ssidconnect.exe /SSID "CorporateNet" /INT lan1 \
  /WPACONF "C:\MPTN\ETC\wpa_supplicant.conf"
```

The file must already exist. It is neither modified nor deleted. `/WPACONF`
cannot be combined with `/WPA`, `/PSK`, `/SAE`, `/CCMP`, `/TKIP`, `/PMF`, or
`/HIDDEN`. `/SSID` is still required so the reported connected SSID can be
verified before DHCP starts.

### Generated policy matrix

| `/WPA` mode | Key management | Protocol | Default ciphers | Default PMF | Credential |
|---|---|---|---|---|---|
| `auto` | WPA-PSK | WPA and RSN | CCMP and TKIP | Unspecified | `/PSK` |
| `wpa` | WPA-PSK | WPA | TKIP | Unspecified | `/PSK` |
| `wpa2` | WPA-PSK | RSN | CCMP | Unspecified | `/PSK` |
| `wpa3` | SAE | RSN | CCMP | Required | `/SAE`, or `/PSK` as the SAE password |
| `transition` | WPA-PSK and SAE | RSN | CCMP | Optional | `/PSK`; optional distinct `/SAE` |

WPA3 requires a supplicant build with SAE support and compatible driver and
firmware support. In the current IWM d27 source, GenMAC key-installation OIDs
remain a driver milestone, so WPA-family association cannot complete with that
driver until key command support is implemented. The launcher remains useful
with compatible GenMAC drivers and for testing the developing IWM path.

## Disassociate mode

Disassociate from the currently connected SSID without starting DHCP or
changing a TCP/IP interface:

```text
ssidconnect.exe /SSID "LWGuest" /DISC
```

The utility first confirms that the adapter is associated and that the
driver's current SSID exactly matches `/SSID`. It then sends
`OID_802_11_DISASSOCIATE` and waits for link status, media status, and the
current SSID to confirm link-down. A mismatched SSID is rejected without
issuing the disassociate request.

`/DISC` requires `/SSID` and cannot be combined with `/SCAN`, `/DHCP`, or
`/NOSCAN`. `/DEV`, `/TIMEOUT`, `/POLL`, and `/QUIET` remain available. `/INT`
and `/SD` are accepted only when supplied together for the optional cleanup
mode described below.

To disassociate and also stop/delete the interface's classic DHCP state, use
the existing `/SD` option together with `/INT`:

```text
ssidconnect.exe /SSID "LWGuest" /DISC /INT lan1 /SD
```

After disassociation is confirmed, this runs:

```text
dhcpmon -r
dhcpmon -t
ifconfig lan1 down
ifconfig lan1 delete
```

Unlike connect mode with `/SD`, disconnect mode does **not** run `DHCPSTRT`
afterward. With `/DISC`, `/INT` and `/SD` must be supplied together; neither is
accepted alone. The SSID mismatch safeguard remains in force, so no
disassociation or interface cleanup occurs unless the current SSID exactly
matches `/SSID`.

## Scan mode

Request a driver scan and display the resulting SSIDs:

```text
ssidconnect.exe /SCAN
```

Display the driver's existing scan cache without requesting a new scan:

```text
ssidconnect.exe /SCAN /NOSCAN
```

Example output:

```text
SSID                             BSSID               CH RSSI(dBm) FREQ(MHz) SECURITY
LWGuest                          02:11:22:33:44:55    1       -42      2412 open
ShopNet                          02:aa:bb:cc:dd:ee   36       -67      5180 secured
```

`/SCAN` is a standalone operating mode and cannot be combined with `/SSID`, `/INT`, `/DISC`, or `/SD`. It never starts DHCP. `/DEV`, `/NOSCAN`, and `/QUIET` remain available in scan mode.

## Supported connection type

This first-test utility configures an **open infrastructure network** only:

- automatic/open authentication
- encryption disabled
- accept-all privacy filter
- stale WEP key slots cleared

It intentionally does not accept a passphrase or configure WPA/WPA2. WPA requires `wpa_supplicant`, EAPOL transport, and working key-installation OIDs.

## DHCP behavior

The default `/DHCP auto` mode searches the current directory and `PATH` in this order:

1. `DHCLIENT.EXE lanN` — the current XWLAN/ArcaOS ISC client path.
2. `DHCPSTRT.EXE -i lanN -d 0` — the classic OS/2 fallback.

The selected client is launched as an independent background session after association is confirmed. Override with:

```text
/DHCP dhclient
/DHCP dhcpstrt
/DHCP none
```

`/DHCP none` is useful for testing only the driver association acknowledgement.

### Optional classic-DHCP reset

Use `/SD` when a previous `DHCPSTRT` session may prevent another one from
starting:

```text
ssidconnect.exe /SSID "LWGuest" /INT lan1 /SD
```

After association is confirmed, `/SD` deliberately runs this sequence and
then starts `DHCPSTRT`:

```text
dhcpmon -r
dhcpmon -t
ifconfig lan1 down
ifconfig lan1 delete
dhcpstrt -i lan1
```

The `lanN` value is taken only from the validated `/INT` argument. These four
cleanup commands are **never** issued unless `/SD` is explicitly supplied.
`/SD` forces the classic `DHCPSTRT` path even when `/DHCP` is omitted, and is
therefore incompatible with `/DHCP dhclient`, `/DHCP none`, and `/SCAN`.
In `/DISC /INT lanN /SD` mode, the same four cleanup commands run after
link-down confirmation, but `DHCPSTRT` is intentionally not restarted.

Before changing interface state, the utility verifies that `DHCPMON.EXE`,
`IFCONFIG.EXE`, and `DHCPSTRT.EXE` are available. Individual cleanup-command
errors are reported as warnings and the sequence continues because a missing
old lease or already-deleted interface is an expected condition. Failure to
find a required executable prevents the cleanup and DHCP launch.

## Build on OS/2

Open an Open Watcom environment, copy `ssidconnect.c` and `build.cmd` to the same directory, then run:

```text
build.cmd
```

Equivalent command:

```text
wcl386 -q -bt=os2 -l=os2v2 -d__OS2__ -fe=ssidconnect.exe ssidconnect.c
```

The program uses only OS/2 system APIs and the GenMAC IOCTL contract; no XWLAN libraries are linked.

## Options

```text
ssidconnect.exe /SSID "name" /INT lanN [options]
ssidconnect.exe /SSID "name" /INT lanN /WPA mode /PSK key
ssidconnect.exe /SSID "name" /INT lanN /WPACONF file
ssidconnect.exe /SSID "name" /DISC [options]
ssidconnect.exe /SSID "name" /DISC /INT lanN /SD [options]
ssidconnect.exe /SCAN [/NOSCAN] [options]

/SCAN                   List SSIDs; not with /SSID, /INT, /DISC, or /SD
/DISC                   Disassociate; /INT is allowed only together with /SD
/DEV device             Default: \DEV\WRND32$
/TIMEOUT seconds        1..600; default 30
/POLL milliseconds      50..5000; default 250
/DHCP mode              auto, dhclient, dhcpstrt, or none
/WPA mode               auto, wpa, wpa2, wpa3, or transition
/PSK key                8-63 printable characters or 64 hex digits
/SAE password           Explicit 1-63 character SAE password
/CCMP | /TKIP           Generated-config cipher selection
/PMF policy             disabled, optional, or required
/HIDDEN                 Scan for a hidden SSID in generated config
/WPACONF file           Persistent existing config; never deleted
/SD                     Stop old DHCP and delete lanN; connect mode restarts it
/NOSCAN                 Reuse the driver's existing scan cache
/QUIET                  Print errors only
/? | /H | /HELP         Show help
```

A leading `-` can replace `/` for every option shown above. Long option values
such as `dhclient`, `dhcpstrt`, and `none` are unchanged.

The driver-reported LAN number is queried. A mismatch with `/INT` produces a warning, but the explicit `/INT` value remains authoritative for DHCP.

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Scan displayed, association/DHCP completed, disassociation confirmed, or `/DHCP none` |
| 1 | Invalid command line |
| 2 | Driver open/type failure |
| 3 | Scan/link request, WPA configuration, or supplicant launch failed; SSID mismatch |
| 4 | Association or disassociation acknowledgement timed out |
| 5 | Link operation succeeded, but DHCP cleanup prerequisites or client launch failed |

## Host-flow check

`test-host.sh` builds a Linux simulation that exercises argument parsing, generated WPA/WPA2/WPA3/transition configurations, persistent-config retention, fresh/cached scan listing, scan-mode exclusivity, verified disassociation, disassociate-plus-interface cleanup without DHCP restart, the request → acknowledgement → DHCP ordering, and the opt-in `/SD` cleanup order. It does not test OS/2 ABI linkage or real hardware.
