#!/bin/bash
# iwl-capture.sh - capture iwlwifi register writes, host commands and TX
# frames through the INIT and runtime firmware loads, an optional scan and an
# optional association with DHCP and ping traffic, and compare the loads with
# the OS/2 driver's WRND32$ log.
#
#   sudo ./iwl-capture.sh capture [outdir]
#        ./iwl-capture.sh process RAW_TRACE outdir
#        ./iwl-capture.sh compare WRND32_LOG outdir [load_number]
#
# capture environment:
#   CMD_OFFSETS="id flags len"  iwl_host_cmd byte offsets (else pahole/DWARF)
#   SKB_OFFSETS="len data"      struct sk_buff offsets (else pahole on vmlinux BTF)
#   IFACE=wlan0                 interface to bring up (else auto-detected)
#   INIT_SECONDS=5              wait after modprobe (probe-time INIT load)
#   RT_SECONDS=8                wait after "ip link set up" (runtime load)
#   SCAN=1                      also trigger one scan (needs iw); 0 disables
#   SCAN_SECONDS=8              wait after the scan trigger
#   ASSOC_SSID="name"           connect to this (open) network after the scan
#   ASSOC_FREQ=2412             optional frequency in MHz for the connect
#   ASSOC_WAIT=20               seconds to wait for "Connected" after the connect;
#                               the traffic step is skipped when it never comes
#   ASSOC_SECONDS=8             stay connected this long before the traffic step
#   TRAFFIC=1                   after the connect: DHCP, then ping (0 disables)
#   DHCP_SECONDS=15             DHCP client timeout
#   DHCP_CMD="..."              own DHCP command ($IF is the interface); else
#                               dhcpcd or dhclient is used when installed
#   STATIC_IP=192.168.1.50/24   used when no DHCP client or no lease
#   STATIC_GW=192.168.1.1       default gateway for STATIC_IP
#   PING_TARGET=addr            default: the default gateway
#   PING_COUNT=10               echo requests to send
#   PCAP=1                      tcpdump the interface during the traffic step
#   PORTAL_WAIT=0               seconds to pause after DHCP so that a captive
#                               portal can be logged in to from a browser
#   ALLOW_MISSING=0             1: run even when tcpdump or a DHCP client is missing
#   PAYLOAD_CHUNKS=8            224-byte host-command payload blocks to record
#   TX_PAYLOAD_CHUNKS=1         224-byte blocks of each TX command (TX_CMD)
#   TX_FRAME_CHUNKS=2           224-byte blocks of each transmitted frame
#   BUF_KB=16384                trace buffer per CPU
set -u

SCRIPT_REV="cmp-5"
T=/sys/kernel/tracing
GROUP=iwlcap
TCPDUMP_PID=""
EVENT_PID=""
DHCP_KIND=""

die() { echo "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# process: fold a raw trace into OS/2-wtrace-format tables, one per firmware
# load, plus the host-command list, the TX-frame list and a load summary.
#
# Folding follows os2/iwm_d32_fw.c iwm_tr_w():
#   0x444 PRPH_WADDR  sets the address, not counted
#   0x448 PRPH_RADDR  ignored
#   0x44c PRPH_WDAT   key p<addr & 0xfffff>
#   0x410 MEM_WADDR   sets the address, not counted
#   0x418 MEM_WDAT    key m<addr & 0xffffff>
#   other CSR         key c<offset>
# Each row: key=first/last*count@seq, seq = ordinal of the first write in
# the window.  A load window ends with the first 0x1af0 = 0xffffffff write
# (the OS/2 "mark").  Load 1 starts at the beginning of the trace; each later
# load starts at the first CSR_RESET write with SW_RESET (0x80) after the
# previous mark.  Interrupt-context writes (irq/ threads, hardirq flag h/H)
# are excluded, as the OS/2 table records task-level writes only.
# Host-command and TX lines carry mark=<label>, the last trace marker seen
# (modprobe, linkup, scan, assoc, dhcp, ping, disconnect, end).
# ---------------------------------------------------------------------------
process_trace() {
    local raw="$1" out="$2"
    [ -r "$raw" ] || die "cannot read $raw"
    mkdir -p "$out" || die "cannot create $out"
    rm -f "$out"/iwl-wt-load*.txt "$out"/iwl-wt-post*.txt
    gawk -v OUT="$out" '
    function hv(s) { if (s ~ /fault/) return 0; return strtonum(s) }
    function arg(name,   i, n, f) {
        n = split($0, f, /[ \t]+/)
        for (i = 1; i <= n; i++)
            if (index(f[i], name "=") == 1) return substr(f[i], length(name) + 2)
        return ""
    }
    function note(key, val) {
        ++total
        if (!(key in first)) {
            first[key] = val; seq[key] = total; order[++nkeys] = key
        }
        last[key] = val; cnt[key]++
    }
    function reset_table() {
        delete first; delete last; delete cnt; delete seq; delete order
        nkeys = 0; total = 0; chunks = 0; bytes = 0; nstatus = 0
    }
    function dump(file, hdr,   i, k) {
        print hdr > file
        for (i = 1; i <= nkeys; i++) {
            k = order[i]
            printf "%s=%08x/%08x*%d@%d\n", k, first[k], last[k], cnt[k], seq[k] > file
        }
        close(file)
    }
    function begin_load() {
        if (state == "post" && nkeys)
            dump(OUT "/iwl-wt-post" nload ".txt",
                 sprintf("# post %d rows %d writes %d", nload, nkeys, total))
        reset_table(); state = "load"; ++nload; load_ts = ts
    }
    function end_load() {
        dump(OUT "/iwl-wt-load" nload ".txt",
             sprintf("# load %d start %s end %s rows %d writes %d chunks %d bytes %d status_writes %d",
                     nload, load_ts, ts, nkeys, total, chunks, bytes, nstatus))
        printf "load %d start %s end %s rows %d writes %d chunks %d bytes %d status_writes %d\n",
               nload, load_ts, ts, nkeys, total, chunks, bytes, nstatus > SUMMARY
        reset_table(); state = "post"
    }
    function csr(off, val) {
        if (off == 0x444) { waddr = and(val, 0xfffff); return }
        if (off == 0x448) return
        if (off == 0x410) { maddr = val; return }
        if (off == 0x44c) { note(sprintf("p%x", waddr), val); return }
        if (off == 0x418) { note(sprintf("m%x", and(maddr, 0xffffff)), val); return }
        if (state == "post" && off == 0x20 && and(val, 0x80)) begin_load()
        note(sprintf("c%x", off), val)
        if (state != "load") return
        if (off == 0x19c8) ++chunks
        if (off == 0x194c) bytes += and(val, 0x0fffffff)
        if (off == 0x1af0) {
            ++nstatus
            if (val == 0xffffffff) end_load()
        }
    }
    BEGIN {
        SUMMARY = OUT "/iwl-loads.txt"; CMDS = OUT "/iwl-cmds.txt"
        TXS = OUT "/iwl-tx.txt"
        printf "" > SUMMARY; printf "" > CMDS; printf "" > TXS
        state = "load"; nload = 1; load_ts = "start"; lastmark = "start"
        reset_table()
    }
    {
        tsf = ""
        for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+\.[0-9]+:$/) { tsf = $i; break }
        if (tsf == "") next
        ts = substr(tsf, 1, length(tsf) - 1)
        isr = ($1 ~ /^irq\// || $1 ~ /^<idle>/ || $3 ~ /[hH]/)
    }
    / tracing_mark_write: / {
        m = $0; sub(/^.*tracing_mark_write: */, "", m)
        lastmark = m; sub(/^IWLCAP /, "", lastmark); gsub(/[ \t]+/, "_", lastmark)
        printf "mark %s %s (after load %d)\n", ts, m, (state == "post") ? nload : nload - 1 > SUMMARY
        next
    }
    / cmd: / {
        id = hv(arg("id")); seg = (state == "post") ? nload : nload - 1
        printf "after_load %d n %d ts %s id=0x%x grp=0x%02x op=0x%02x flags=%s len=%s d0=%s d1=%s mark=%s\n",
               seg, ++ncmd, ts, id, and(rshift(id, 8), 0xff), and(id, 0xff),
               arg("flags"), arg("len"), arg("d0"), arg("d1"), lastmark > CMDS
        next
    }
    / tx: / {
        printf "tx %d ts %s txq=%s cmd=0x%02x grp=0x%02x seq=%s skblen=%s mark=%s\n",
               ++ntx, ts, arg("txq"), hv(arg("cmd")), hv(arg("grp")),
               arg("seq"), arg("skblen"), lastmark > TXS
        next
    }
    isr { ++isr_skipped; next }
    / w32: / { csr(hv(arg("ofs")), hv(arg("val"))); next }
    / prph: / { note(sprintf("p%x", and(hv(arg("addr")), 0xfffff)), hv(arg("val"))); next }
    / mem: / {
        a = hv(arg("addr")); n = hv(arg("dwords")); v = arg("v0")
        nullbuf = (v ~ /fault/)
        for (j = 0; j < n && j < 4096; j++)
            note(sprintf("m%x", and(a + 4 * j, 0xffffff)),
                 (j == 0 && !nullbuf) ? hv(v) : (nullbuf ? 0 : 0xdeadbeef))
        next
    }
    END {
        if (state == "load" && nkeys)
            dump(OUT "/iwl-wt-load" nload "-incomplete.txt",
                 sprintf("# load %d incomplete (no 0x1af0=0xffffffff) rows %d writes %d", nload, nkeys, total))
        else if (state == "post" && nkeys)
            dump(OUT "/iwl-wt-post" nload ".txt",
                 sprintf("# post %d rows %d writes %d", nload, nkeys, total))
        printf "isr_writes_skipped %d commands %d tx_frames %d\n", isr_skipped, ncmd, ntx > SUMMARY
    }' "$raw"
    grep ' cmd: ' "$raw" > "$out/iwl-cmd-payloads.txt"
    grep ' tx: ' "$raw" > "$out/iwl-tx-payloads.txt"
    echo "processed $raw -> $out"
    cat "$out/iwl-loads.txt"
}

# ---------------------------------------------------------------------------
# compare: OS/2 wtrace rows (lines "iwm: wt ...") against one Linux load.
# ---------------------------------------------------------------------------
compare_log() {
    local log="$1" out="$2" want="${3:-}" os2bytes lt
    [ -r "$log" ] || die "cannot read $log"
    [ -r "$out/iwl-loads.txt" ] || die "run process or capture first ($out/iwl-loads.txt missing)"
    os2bytes=$(grep -a -m1 "iwm: init load rc" "$log" | sed -n 's/.* bytes \([0-9]*\) .*/\1/p')
    if [ -z "$want" ]; then
        want=$(awk -v b="${os2bytes:-x}" '$1 == "load" && $14 == b { print $2; exit }' "$out/iwl-loads.txt")
        [ -n "$want" ] || want=1
    fi
    lt="$out/iwl-wt-load$want.txt"
    [ -r "$lt" ] || die "$lt not found"
    gawk -v LT="$lt" -v WANT="$want" -v OS2BYTES="${os2bytes:-?}" '
    function isdma(k,   o) {
        if (k == "p8000" || k == "p8100" || k == "p8200" || k == "c197c" || k == "c1948")
            return 1
        if (k !~ /^c/) return 0
        o = strtonum("0x" substr(k, 2))
        return (o >= 0x19d0 && o <= 0x1a0c) || (o >= 0x1bf0 && o <= 0x1bfc) ||
               (o >= 0x1b20 && o <= 0x1b48)
    }
    function parse(tok, side,   k, r, a, b) {
        k = tok; sub(/=.*/, "", k); r = tok; sub(/^[^=]*=/, "", r)
        split(r, a, "[/*@]")
        if (side == "L") { lf[k] = a[1]; ll[k] = a[2]; lc[k] = a[3]; ls[k] = a[4]; lord[++nl] = k }
        else { of[k] = a[1]; ol[k] = a[2]; oc[k] = a[3]; os[k] = a[4]; oord[++no] = k }
    }
    { sub(/\r$/, "") }
    FILENAME == LT { if ($0 !~ /^#/ && NF) parse($1, "L"); else if ($0 ~ /^#/) lhdr = $0; next }
    / iwm: wtrace rows / { ohdr = $0; sub(/^.*iwm: /, "", ohdr); next }
    / iwm: wt / {
        for (i = 1; i <= NF; i++)
            if ($i ~ /^[cpm][0-9a-f]+=/) parse($i, "O")
        next
    }
    function row(st, k,   L, O) {
        L = (k in lf) ? sprintf("%s/%s*%s@%s", lf[k], ll[k], lc[k], ls[k]) : "-"
        O = (k in of) ? sprintf("%s/%s*%s@%s", of[k], ol[k], oc[k], os[k]) : "-"
        printf "%-11s %-9s linux %-32s os2 %s\n", st, k, L, O
        tally[st]++
    }
    END {
        printf "linux %s\nos2   %s (init bytes %s)\nlinux load compared: %s\n\n", lhdr, ohdr, OS2BYTES, WANT
        printf "%-11s %-9s %-38s %s\n", "status", "key", "linux first/last*count@seq", "os2 first/last*count@seq"
        for (i = 1; i <= nl; i++) {
            k = lord[i]
            if (!(k in of)) { row("LINUX-ONLY", k); continue }
            if (lf[k] == of[k] && ll[k] == ol[k]) st = (lc[k] == oc[k]) ? "same" : "same-vals"
            else st = isdma(k) ? "dma-addr" : "DIFF"
            row(st, k)
        }
        for (i = 1; i <= no; i++) { k = oord[i]; if (!(k in lf)) row("OS2-ONLY", k) }
        printf "\nsummary:"
        for (s in tally) printf " %s=%d", s, tally[s]
        printf "\n(dma-addr: DMA/physical address registers; values are expected to differ)\n"
    }' "$lt" "$log" > "$out/iwl-compare.txt"
    {
        echo
        echo "=== Linux host commands after load $want ==="
        grep "^after_load $want " "$out/iwl-cmds.txt"
        echo
        echo "=== OS/2 command/sequence summary lines from $log ==="
        grep -a -E "iwm: (init load|alive status|initseq|init other|nvm sec|init firmware|rt |rtseq|runtime firmware|assoc|authfrm|assocfrm)" "$log" | tr -d '\r'
    } >> "$out/iwl-compare.txt"
    echo "wrote $out/iwl-compare.txt"
    grep -E "^(linux|os2|summary)" "$out/iwl-compare.txt"
}

# ---------------------------------------------------------------------------
# capture
# ---------------------------------------------------------------------------
# Stop the background tcpdump: SIGTERM makes it flush and close the pcap;
# SIGKILL only if it is still running after 5 s.
stop_tcpdump() {
    local i
    [ -n "${TCPDUMP_PID:-}" ] || return 0
    kill -TERM "$TCPDUMP_PID" || true
    for i in 1 2 3 4 5; do
        [ -d "/proc/$TCPDUMP_PID" ] || break
        sleep 1
    done
    if [ -d "/proc/$TCPDUMP_PID" ]; then
        kill -KILL "$TCPDUMP_PID" || true
    fi
    wait "$TCPDUMP_PID" || true
    TCPDUMP_PID=""
}

# iw event (auth, assoc, deauth with status and reason codes) while connecting.
start_iw_events() {
    if command -v stdbuf; then
        stdbuf -oL iw event -t > "$OUT_DIR/iw-events.txt" 2>&1 &
    else
        iw event -t > "$OUT_DIR/iw-events.txt" 2>&1 &
    fi
    EVENT_PID=$!
}

stop_iw_events() {
    [ -n "${EVENT_PID:-}" ] || return 0
    kill -TERM "$EVENT_PID" || true
    wait "$EVENT_PID" || true
    EVENT_PID=""
}

cleanup() {
    stop_tcpdump
    stop_iw_events
    [ -d "$T" ] || return
    echo 0 > "$T/tracing_on"
    if [ -d "$T/events/$GROUP" ]; then
        echo 0 > "$T/events/$GROUP/enable"
    fi
    for ev in w32 prph mem cmd tx; do
        if grep -q "^p:$GROUP/$ev " "$T/kprobe_events"; then
            echo "-:$GROUP/$ev" >> "$T/kprobe_events"
        fi
    done
}

mark() { echo "IWLCAP $*" > "$T/trace_marker"; echo "mark: $*"; }

find_iface() {
    local d drv
    for d in /sys/class/net/*; do
        drv=$(readlink -f "$d/device/driver")
        case "$drv" in
            */iwlwifi) basename "$d"; return 0 ;;
        esac
    done
    return 1
}

offsets_from_pahole() {
    local layout
    layout=$(pahole -C iwl_host_cmd "$@")
    [ -n "$layout" ] || return 1
    { echo "--- pahole -C iwl_host_cmd $* ---"; echo "$layout"; } >> "$INFO"
    echo "$layout" | awk '
        function off(line,   i, n, f) {
            n = split(line, f, /[ \t]+/)
            for (i = 1; i <= n; i++) if (f[i] == "/*") return f[i + 1]
            return ""
        }
        /[ \t]id;/    { id = off($0) }
        /[ \t]flags;/ { fl = off($0) }
        /[ \t]len\[/  { ln = off($0) }
        END { if (id != "" && fl != "" && ln != "") print id, fl, ln }'
}

# struct sk_buff len and data offsets from the running kernel's BTF.
skb_offsets() {
    local layout
    layout=$(pahole -C sk_buff /sys/kernel/btf/vmlinux)
    [ -n "$layout" ] || return 1
    { echo "--- pahole -C sk_buff /sys/kernel/btf/vmlinux ---"
      echo "$layout" | grep -E 'len;|data;'; } >> "$INFO"
    echo "$layout" | awk '
        function off(line,   n) {
            if (match(line, /\/\*[ \t]*[0-9]+/)) {
                n = substr(line, RSTART + 2, RLENGTH - 2); gsub(/[ \t]/, "", n); return n
            }
            return ""
        }
        /[ \t*]len;/  && ln == "" { ln = off($0) }
        /[ \t*]data;/ && dt == "" { dt = off($0) }
        END { if (ln != "" && dt != "") print ln, dt }'
}

# The connect command returns at once; wait until iw reports a connection.
wait_assoc() {
    local IF="$1" secs="${ASSOC_WAIT:-20}" i
    for i in $(seq 1 "$secs"); do
        if iw dev "$IF" link | grep -q '^Connected to'; then
            return 0
        fi
        sleep 1
    done
    return 1
}

# Before anything is unloaded: every tool the traffic step needs must exist,
# otherwise the run produces empty files (cmp-4 on 2026-11-19: no tcpdump, no
# DHCP client).
preflight() {
    local missing=""
    [ -n "${ASSOC_SSID:-}" ] || return 0
    command -v iw || missing="$missing iw"
    if [ "${TRAFFIC:-1}" = "1" ]; then
        command -v ping || missing="$missing ping"
        if [ -z "${DHCP_CMD:-}" ]; then
            command -v dhcpcd || command -v dhclient || missing="$missing dhcpcd(or-dhclient)"
        fi
        if [ "${PCAP:-1}" = "1" ]; then
            command -v tcpdump || missing="$missing tcpdump"
        fi
    fi
    if [ -n "$missing" ]; then
        echo "preflight: missing tools:$missing" | tee -a "$INFO"
        [ "${ALLOW_MISSING:-0}" = "1" ] || die "install them first (for example: zypper in tcpdump dhcpcd), or set ALLOW_MISSING=1"
    else
        echo "preflight: all tools present" | tee -a "$INFO"
    fi
}

try_probe() {
    if echo "$1" >> "$T/kprobe_events"; then
        echo "armed: $1" | tee -a "$INFO"
        return 0
    fi
    echo "kprobe rejected: $1" >&2
    if [ -e "$T/error_log" ]; then tail -n 3 "$T/error_log" >&2; fi
    return 1
}

# ---------------------------------------------------------------------------
# traffic: DHCP and ping on the associated link, so that the trace holds the
# data frames Linux sends (queue 4, mark=dhcp and mark=ping) and, from the
# tcpdump and the station counters, what the AP sends back.
# ---------------------------------------------------------------------------
snap_link() {
    local IF="$1" tag="$2"
    ip -s link show dev "$IF" > "$OUT_DIR/ip-link-$tag.txt" 2>&1
    iw dev "$IF" station dump > "$OUT_DIR/iw-station-$tag.txt" 2>&1
}

have_ipv4() {
    [ -n "$(ip -4 -o addr show dev "$1" scope global)" ]
}

run_dhcp() {
    local IF="$1" secs="${DHCP_SECONDS:-15}"
    DHCP_KIND=""
    if [ -n "${DHCP_CMD:-}" ]; then
        DHCP_KIND=custom
        IF="$IF" bash -c "$DHCP_CMD" 2>&1 | tee -a "$TRAFFIC_LOG"
    elif command -v dhcpcd; then
        DHCP_KIND=dhcpcd
        dhcpcd -1 -L -t "$secs" "$IF" 2>&1 | tee -a "$TRAFFIC_LOG"
    elif command -v dhclient; then
        DHCP_KIND=dhclient
        dhclient -1 -v -timeout "$secs" "$IF" 2>&1 | tee -a "$TRAFFIC_LOG"
    else
        echo "no DHCP client found (dhcpcd, dhclient); set DHCP_CMD" | tee -a "$TRAFFIC_LOG"
    fi
    if ! have_ipv4 "$IF"; then
        if [ -n "${STATIC_IP:-}" ]; then
            echo "no DHCP lease; using STATIC_IP $STATIC_IP" | tee -a "$TRAFFIC_LOG"
            ip addr add "$STATIC_IP" dev "$IF" 2>&1 | tee -a "$TRAFFIC_LOG"
            if [ -n "${STATIC_GW:-}" ]; then
                ip route add default via "$STATIC_GW" dev "$IF" 2>&1 | tee -a "$TRAFFIC_LOG"
            fi
        else
            echo "no IPv4 address on $IF (no lease, STATIC_IP not set)" | tee -a "$TRAFFIC_LOG"
        fi
    fi
}

stop_dhcp() {
    local IF="$1"
    case "$DHCP_KIND" in
        dhcpcd)   dhcpcd -k "$IF" 2>&1 | tee -a "$TRAFFIC_LOG" ;;
        dhclient) dhclient -r "$IF" 2>&1 | tee -a "$TRAFFIC_LOG" ;;
    esac
    ip addr flush dev "$IF" 2>&1 | tee -a "$TRAFFIC_LOG"
}

traffic_step() {
    local IF="$1" target
    TRAFFIC_LOG="$OUT_DIR/iwl-traffic-log.txt"
    : > "$TRAFFIC_LOG"
    snap_link "$IF" before
    if [ "${PCAP:-1}" = "1" ]; then
        if command -v tcpdump; then
            tcpdump -i "$IF" -n -e -s 0 -w "$OUT_DIR/iwl-traffic.pcap.txt" \
                > "$OUT_DIR/iwl-tcpdump-log.txt" 2>&1 &
            TCPDUMP_PID=$!
            sleep 1
        else
            echo "tcpdump not installed; no pcap" | tee -a "$TRAFFIC_LOG"
        fi
    fi
    mark "dhcp $IF"
    run_dhcp "$IF"
    ip -4 addr show dev "$IF" > "$OUT_DIR/ip-addr.txt" 2>&1
    ip -4 route show > "$OUT_DIR/ip-route.txt" 2>&1
    tee -a "$TRAFFIC_LOG" < "$OUT_DIR/ip-addr.txt"
    if [ "${PORTAL_WAIT:-0}" -gt 0 ] && have_ipv4 "$IF"; then
        mark "portal $IF"
        echo "captive portal: log in from a browser now; continuing in ${PORTAL_WAIT}s" | tee -a "$TRAFFIC_LOG"
        sleep "$PORTAL_WAIT"
    fi
    target="${PING_TARGET:-}"
    if [ -z "$target" ]; then
        target=$(ip -4 route show default dev "$IF" | awk '{ print $3; exit }')
    fi
    if [ -n "$target" ]; then
        mark "ping $IF"
        ping -c "${PING_COUNT:-10}" -W 2 -I "$IF" "$target" 2>&1 | tee -a "$OUT_DIR/ping.txt" "$TRAFFIC_LOG"
    else
        echo "no ping target (no default route, PING_TARGET not set)" | tee -a "$TRAFFIC_LOG"
    fi
    sleep 2
    snap_link "$IF" after
    stop_dhcp "$IF"
    if [ -n "$TCPDUMP_PID" ]; then
        stop_tcpdump
        tcpdump -nn -e -v -r "$OUT_DIR/iwl-traffic.pcap.txt" > "$OUT_DIR/iwl-traffic.txt" 2>&1
    fi
}

capture() {
    OUT_DIR="${1:-$PWD}"
    RAW="$OUT_DIR/iwl-w32.txt"
    INFO="$OUT_DIR/iwl-probe-info.txt"
    local fn W32_FN="" PRPH_FN="" MEM_FN="" CMD_FN="" TX_FN="" OFFS="" SKBO="" DBG KO IF i
    local PARGS TARGS FARGS SKB_LEN SKB_DATA
    trap cleanup EXIT INT TERM
    [ "$(id -u)" -eq 0 ] || die "run as root (sudo)"
    [ "$(uname -m)" = "x86_64" ] || die "register argument mapping assumes x86_64"
    if [ ! -e "$T/kprobe_events" ]; then
        if [ -e /sys/kernel/debug/tracing/kprobe_events ]; then
            T=/sys/kernel/debug/tracing
        else
            mount -t tracefs nodev /sys/kernel/tracing || die "cannot mount tracefs"
            [ -e "$T/kprobe_events" ] || die "kernel lacks CONFIG_KPROBE_EVENTS"
        fi
    fi
    mkdir -p "$OUT_DIR" || die "cannot create $OUT_DIR"

    modprobe iwlwifi || die "modprobe iwlwifi failed"
    sleep 3
    has_fn() { grep -qE "^$1( |$)" "$T/available_filter_functions"; }
    for fn in iwl_trans_pcie_write32 iwl_write32; do
        if has_fn "$fn"; then W32_FN="$fn"; break; fi
    done
    for fn in iwl_trans_pcie_write_prph iwl_write_prph_no_grab; do
        if has_fn "$fn"; then PRPH_FN="$fn"; break; fi
    done
    if has_fn iwl_trans_pcie_write_mem; then MEM_FN=iwl_trans_pcie_write_mem; fi
    for fn in iwl_trans_send_cmd iwl_trans_pcie_send_hcmd iwl_pcie_enqueue_hcmd; do
        if has_fn "$fn"; then CMD_FN="$fn"; break; fi
    done
    if has_fn iwl_trans_pcie_tx; then TX_FN=iwl_trans_pcie_tx; fi
    {
        echo "script revision: $SCRIPT_REV"
        echo "kernel: $(uname -r)"
        echo "w32 probe:  ${W32_FN:-<none>}"
        echo "prph probe: ${PRPH_FN:-<none>}"
        echo "mem probe:  ${MEM_FN:-<none>}"
        echo "cmd probe:  ${CMD_FN:-<none>}"
        echo "tx probe:   ${TX_FN:-<none>}"
    } > "$INFO"
    cat "$INFO"
    [ -n "$W32_FN" ] || die "no probe-able write32 function"
    [ -n "$CMD_FN" ] || die "no probe-able host-command function"
    preflight

    if [ -n "${CMD_OFFSETS:-}" ]; then
        OFFS="$CMD_OFFSETS"
        echo "cmd offsets from CMD_OFFSETS: $OFFS" | tee -a "$INFO"
    else
        command -v pahole || die "pahole not installed (zypper in dwarves)"
        DBG=$(find /usr/lib/debug/lib/modules/"$(uname -r)" -name 'iwlwifi.ko*.debug' -print -quit)
        echo "debuginfo file: ${DBG:-<none>}" | tee -a "$INFO"
        if [ -n "$DBG" ]; then OFFS=$(offsets_from_pahole -F dwarf "$DBG"); fi
        if [ -z "$OFFS" ]; then
            KO=$(modinfo -n iwlwifi)
            case "$KO" in
                *.zst) zstd -dcf "$KO" > "$OUT_DIR/iwlwifi.ko" && KO="$OUT_DIR/iwlwifi.ko" ;;
                *.xz)  xz -dcf "$KO" > "$OUT_DIR/iwlwifi.ko" && KO="$OUT_DIR/iwlwifi.ko" ;;
                *.gz)  gzip -dcf "$KO" > "$OUT_DIR/iwlwifi.ko" && KO="$OUT_DIR/iwlwifi.ko" ;;
            esac
            OFFS=$(offsets_from_pahole -F dwarf "$KO")
        fi
        [ -n "$OFFS" ] || die "no DWARF for iwlwifi; rerun with CMD_OFFSETS=\"id flags len\""
        echo "cmd offsets from pahole: $OFFS" | tee -a "$INFO"
    fi
    set -- $OFFS

    if [ -n "$TX_FN" ]; then
        if [ -n "${SKB_OFFSETS:-}" ]; then
            SKBO="$SKB_OFFSETS"
        elif command -v pahole; then
            SKBO=$(skb_offsets)
        fi
        if [ -n "$SKBO" ]; then
            echo "sk_buff len/data offsets: $SKBO" | tee -a "$INFO"
        else
            echo "WARNING: sk_buff offsets unavailable (set SKB_OFFSETS=\"len data\"); TX frames not captured" | tee -a "$INFO"
            TX_FN=""
        fi
    fi

    modprobe -r iwlmvm iwlwifi || die "cannot unload iwlmvm/iwlwifi (stop NetworkManager/wpa_supplicant first)"
    echo 0 > "$T/tracing_on"
    echo > "$T/trace"
    echo "${BUF_KB:-16384}" > "$T/buffer_size_kb"
    cleanup
    trap cleanup EXIT INT TERM

    try_probe "p:$GROUP/w32 iwlwifi:$W32_FN ofs=%si:x32 val=%dx:x32" || die "w32 probe rejected"
    if [ -n "$PRPH_FN" ]; then
        try_probe "p:$GROUP/prph iwlwifi:$PRPH_FN addr=%si:x32 val=%dx:x32"
    fi
    if [ -n "$MEM_FN" ]; then
        try_probe "p:$GROUP/mem iwlwifi:$MEM_FN addr=%si:x32 dwords=%cx:u32 v0=+0(%dx):x32"
    fi
    # Payload capture: data pointer 0 is at offset 0 of iwl_host_cmd.  Kprobe
    # arrays must have fewer than 64 elements, so PAYLOAD_CHUNKS blocks of
    # x32 x 56 (224 bytes) are fetched; 8 blocks cover the 1688-byte scan.
    PARGS=""
    for i in $(seq 0 $(( ${PAYLOAD_CHUNKS:-8} - 1 ))); do
        PARGS="$PARGS p$i=+$((i * 224))(+0(%si)):x32[56]"
    done
    try_probe "p:$GROUP/cmd iwlwifi:$CMD_FN id=+$1(%si):x32 flags=+$2(%si):x32 len=+$3(%si):u16 d0=+0(+0(%si)):x32 d1=+4(+0(%si)):x32$PARGS" \
        || try_probe "p:$GROUP/cmd iwlwifi:$CMD_FN id=+$1(%si):x32 flags=+$2(%si):x32 len=+$3(%si):u16 d0=+0(+0(%si)):x32 d1=+4(+0(%si)):x32" \
        || die "cmd probe rejected with offsets $OFFS"
    if [ -n "$TX_FN" ]; then
        # iwl_trans_pcie_tx(trans, skb, dev_cmd, txq_id): skb in %si, dev_cmd
        # in %dx, txq in %cx.  dev_cmd starts with the 4-byte command header
        # (cmd, group, le16 sequence); TX_CMD is cmd 0x1c, so the "cmd=" field
        # of every captured line doubles as a check of this layout.
        set -- $SKBO
        SKB_LEN="$1"; SKB_DATA="$2"
        TARGS=""
        for i in $(seq 0 $(( ${TX_PAYLOAD_CHUNKS:-1} - 1 ))); do
            TARGS="$TARGS t$i=+$((4 + i * 224))(%dx):x32[56]"
        done
        FARGS=""
        for i in $(seq 0 $(( ${TX_FRAME_CHUNKS:-2} - 1 ))); do
            FARGS="$FARGS f$i=+$((i * 224))(+$SKB_DATA(%si)):x32[56]"
        done
        try_probe "p:$GROUP/tx iwlwifi:$TX_FN txq=%cx:s32 cmd=+0(%dx):u8 grp=+1(%dx):u8 seq=+2(%dx):u16 skblen=+$SKB_LEN(%si):u32$TARGS$FARGS" \
            || echo "WARNING: tx probe rejected; TX frames not captured" | tee -a "$INFO"
    fi
    echo 1 > "$T/events/$GROUP/enable" || die "cannot enable $GROUP events"
    echo 1 > "$T/tracing_on"

    mark "modprobe"
    modprobe iwlwifi || echo "WARNING: modprobe iwlwifi returned an error; trace is kept" >&2
    sleep "${INIT_SECONDS:-5}"

    IF="${IFACE:-}"
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -n "$IF" ] && break
        IF=$(find_iface) && break
        sleep 1
    done
    if [ -z "$IF" ]; then
        echo "WARNING: no iwlwifi network interface found; runtime load not triggered" | tee -a "$INFO"
    else
        echo "interface: $IF" | tee -a "$INFO"
        if command -v rfkill; then rfkill list | tee -a "$INFO"; fi
        mark "linkup $IF"
        ip link set "$IF" up || echo "WARNING: ip link set $IF up failed (rfkill? NetworkManager?)" | tee -a "$INFO"
        sleep "${RT_SECONDS:-8}"
        if [ "${SCAN:-1}" = "1" ]; then
            if command -v iw; then
                mark "scan $IF"
                iw dev "$IF" scan trigger || echo "WARNING: scan trigger failed" | tee -a "$INFO"
                sleep "${SCAN_SECONDS:-8}"
            else
                echo "iw not installed; scan skipped" | tee -a "$INFO"
            fi
        fi
        if [ -n "${ASSOC_SSID:-}" ]; then
            if command -v iw; then
                iw dev "$IF" info > "$OUT_DIR/iw-info.txt" 2>&1
                start_iw_events
                mark "assoc $IF"
                iw dev "$IF" connect -w "$ASSOC_SSID" ${ASSOC_FREQ:-} 2>&1 | tee -a "$INFO" \
                    || echo "WARNING: iw connect failed" | tee -a "$INFO"
                if wait_assoc "$IF"; then
                    echo "associated" | tee -a "$INFO"
                    sleep "${ASSOC_SECONDS:-8}"
                    iw dev "$IF" link > "$OUT_DIR/iw-link.txt" 2>&1
                    cat "$OUT_DIR/iw-link.txt"
                    if [ "${TRAFFIC:-1}" = "1" ]; then
                        traffic_step "$IF"
                        iw dev "$IF" link > "$OUT_DIR/iw-link-after.txt" 2>&1
                        cat "$OUT_DIR/iw-link-after.txt"
                    fi
                else
                    iw dev "$IF" link > "$OUT_DIR/iw-link.txt" 2>&1
                    cat "$OUT_DIR/iw-link.txt"
                    echo "WARNING: not associated after ${ASSOC_WAIT:-20}s; traffic step skipped (see iw-events.txt and dmesg-tail.txt)" | tee -a "$INFO"
                    dmesg | tail -n 60 > "$OUT_DIR/dmesg-tail.txt"
                fi
                stop_iw_events
                mark "disconnect $IF"
                iw dev "$IF" disconnect || echo "WARNING: iw disconnect failed" | tee -a "$INFO"
                sleep 3
            else
                echo "iw not installed; association skipped" | tee -a "$INFO"
            fi
        fi
        mark "end"
    fi
    echo 0 > "$T/tracing_on"
    cat "$T/trace" > "$RAW"
    echo "lost events (per CPU):" | tee -a "$INFO"
    grep -H . "$T"/per_cpu/cpu*/stats | grep -E 'overrun|dropped' | grep -v ' 0$' | tee -a "$INFO"
    echo "events captured: $(grep -cE ' (w32|prph|mem|cmd|tx): ' "$RAW")" | tee -a "$INFO"
    process_trace "$RAW" "$OUT_DIR"
    echo "command payload lines: $(wc -l < "$OUT_DIR/iwl-cmd-payloads.txt")" | tee -a "$INFO"
    echo "tx payload lines: $(wc -l < "$OUT_DIR/iwl-tx-payloads.txt")" | tee -a "$INFO"
}

echo "iwl-capture.sh revision $SCRIPT_REV"
case "${1:-capture}" in
    capture) shift || true; capture "${1:-$PWD}" ;;
    process) [ $# -ge 3 ] || die "usage: $0 process RAW_TRACE outdir"; process_trace "$2" "$3" ;;
    compare) [ $# -ge 3 ] || die "usage: $0 compare WRND32_LOG outdir [load]"; compare_log "$2" "$3" "${4:-}" ;;
    *) capture "$1" ;;
esac
