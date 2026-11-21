/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ssidconnect.c - minimal OS/2 GenMAC open-network connector.
 *
 * Lists the GenMAC scan cache or sends the same non-WPA connection OID
 * sequence used by XWLAN, waits for the driver's link acknowledgement,
 * verifies the associated SSID, and then starts DHCP on the caller-selected
 * LAN interface.
 *
 * Build for OS/2 with Open Watcom:
 *   wcl386 -q -bt=os2 -l=os2v2 -fe=ssidconnect.exe ssidconnect.c
 *
 * Host-only flow simulation:
 *   cc -Wall -Wextra -std=c89 -DSSIDCONNECT_SIMULATE -o ssidconnect-sim ssidconnect.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <time.h>

#if defined(__OS2__)
#define INCL_DOSFILEMGR
#define INCL_DOSDEVICES
#define INCL_DOSPROCESS
#define INCL_DOSSESMGR
#define INCL_DOSMISC
#define INCL_DOSERRORS
#include <os2.h>
typedef HFILE DRIVER_HANDLE;
#else
typedef int DRIVER_HANDLE;
typedef unsigned long APIRET;
#endif

#define PROGRAM_NAME "ssidconnect"
#define PROGRAM_VERSION "0.7"
#define DEFAULT_DEVICE "\\DEV\\WRND32$"
#define DEFAULT_TIMEOUT_SECONDS 30UL
#define DEFAULT_POLL_MS 250UL

#define GENMAC_CATEGORY                    0x99UL
#define GENMAC_WRAPPER_OID_GET             0x40UL
#define GENMAC_WRAPPER_OID_SET             0x41UL

#define OID_GEN_MEDIA_CONNECT_STATUS       0x00010114UL
#define OID_802_11_BSSID                   0x0D010101UL
#define OID_802_11_SSID                    0x0D010102UL
#define OID_802_11_RSSI                    0x0D010206UL
#define OID_802_11_INFRASTRUCTURE_MODE     0x0D010108UL
#define OID_802_11_REMOVE_WEP              0x0D010114UL
#define OID_802_11_DISASSOCIATE             0x0D010115UL
#define OID_802_11_AUTHENTICATION_MODE     0x0D010118UL
#define OID_802_11_PRIVACY_FILTER          0x0D010119UL
#define OID_802_11_BSSID_LIST_SCAN         0x0D01011AUL
#define OID_802_11_ENCRYPTION_STATUS       0x0D01011BUL
#define OID_802_11_BSSID_LIST               0x0D010217UL
#define OID_PRIVATE_WRAPPER_LANNUMBER      0xFFFFFF00UL
#define OID_PRIVATE_WRAPPER_ISWIRELESS     0xFFFFFF01UL
#define OID_PRIVATE_WRAPPER_LINKSTATUS     0xFFFFFF02UL

#define NDIS_802_11_INFRASTRUCTURE          1UL
#define NDIS_802_11_AUTH_AUTO               2UL
#define NDIS_802_11_ENCRYPTION_DISABLED     1UL
#define NDIS_802_11_PRIVACY_ACCEPT_ALL      0UL
#define NDIS_MEDIA_CONNECTED                0UL
#define NDIS_LINK_CONNECTED                 1UL

#define SSID_MAX_BYTES 32U
#define IFACE_MAX_BYTES 15U
#define DEVICE_MAX_BYTES 63U
#define WPA_SECRET_MAX_BYTES 64U
#define WPA_PATH_MAX_BYTES 259U
#define SCAN_LIST_BYTES 32767UL
#define BSSID_EX_FIXED 116UL

#define EXIT_USAGE       1
#define EXIT_DRIVER      2
#define EXIT_CONNECT     3
#define EXIT_TIMEOUT     4
#define EXIT_DHCP        5

enum dhcp_mode {
    DHCP_AUTO = 0,
    DHCP_DHCLIENT,
    DHCP_DHCPSTRT,
    DHCP_NONE
};

enum wpa_mode {
    WPA_MODE_NONE = 0,
    WPA_MODE_AUTO,
    WPA_MODE_WPA,
    WPA_MODE_WPA2,
    WPA_MODE_WPA3,
    WPA_MODE_TRANSITION
};

enum pmf_mode {
    PMF_DEFAULT = 0,
    PMF_DISABLED,
    PMF_OPTIONAL,
    PMF_REQUIRED
};

struct ndis_ssid {
    uint32_t length;
    unsigned char ssid[SSID_MAX_BYTES];
};

struct options {
    char ssid[SSID_MAX_BYTES + 1U];
    char iface[IFACE_MAX_BYTES + 1U];
    char device[DEVICE_MAX_BYTES + 1U];
    char psk[WPA_SECRET_MAX_BYTES + 1U];
    char sae[WPA_SECRET_MAX_BYTES + 1U];
    char wpa_conf[WPA_PATH_MAX_BYTES + 1U];
    unsigned long timeout_seconds;
    unsigned long poll_ms;
    unsigned long iface_number;
    enum dhcp_mode dhcp;
    enum wpa_mode wpa;
    enum pmf_mode pmf;
    int dhcp_explicit;
    int scan;
    int list_mode;
    int disconnect_mode;
    int stop_delete;
    int wpa_conf_existing;
    int cipher_ccmp;
    int cipher_tkip;
    int hidden_ssid;
    int quiet;
};

struct oid_error {
    unsigned long os2_rc;
    uint32_t ndis_status;
};

static int ascii_equal(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;
    while (*a && *b) {
        ca = (unsigned char)tolower((unsigned char)*a++);
        cb = (unsigned char)tolower((unsigned char)*b++);
        if (ca != cb)
            return 0;
    }
    return *a == 0 && *b == 0;
}

/* OS/2 traditionally uses /OPTION, while many cross-platform tools use
 * -OPTION. Keep one parser and accept either prefix for every named option. */
static int option_equal(const char *argument, const char *dash_option)
{
    if (ascii_equal(argument, dash_option))
        return 1;
    return argument && dash_option && argument[0] == '/' &&
           dash_option[0] == '-' && dash_option[1] != '-' &&
           ascii_equal(argument + 1, dash_option + 1);
}

static void usage(FILE *stream)
{
    fprintf(stream,
        "SSIDCONNECT %s - connect, launch WPA, scan, or disconnect\n"
        "\n"
        "Usage:\n"
        "  ssidconnect.exe /SSID \"name\" /INT lanN [options]\n"
        "  ssidconnect.exe /SSID \"name\" /INT lanN /WPA mode /PSK key\n"
        "  ssidconnect.exe /SSID \"name\" /INT lanN /WPACONF file\n"
        "  ssidconnect.exe /SSID \"name\" /DISC [options]\n"
        "  ssidconnect.exe /SSID \"name\" /DISC /INT lanN /SD [options]\n"
        "  ssidconnect.exe /SCAN [/NOSCAN] [options]\n"
        "  (A leading '-' may be used instead of '/' for every option.)\n"
        "\n"
        "Required:\n"
        "  /SSID name              SSID, 1 through 32 bytes\n"
        "  /INT lanN               OS/2 TCP/IP interface, for example lan1\n"
        "\n"
        "Options:\n"
        "  /SCAN                   List SSIDs; not with /SSID, /INT, /DISC, or /SD\n"
        "  /DISC                   Disassociate from /SSID; /INT requires /SD\n"
        "  /DEV device             GenMAC device (default: \\DEV\\WRND32$)\n"
        "  /TIMEOUT seconds        Link wait, 1 through 600 (default: 30)\n"
        "  /POLL milliseconds      Poll interval, 50 through 5000 (default: 250)\n"
        "  /DHCP auto|dhclient|dhcpstrt|none\n"
        "                           DHCP launcher (default: auto)\n"
        "  /WPA auto|wpa|wpa2|wpa3|transition\n"
        "                           Generate a temporary supplicant config\n"
        "  /PSK key                WPA/WPA2 PSK; also default SAE password\n"
        "  /SAE password           Explicit WPA3 SAE password\n"
        "  /CCMP | /TKIP           Cipher selection; defaults follow /WPA mode\n"
        "  /PMF disabled|optional|required\n"
        "                           Management-frame protection policy\n"
        "  /HIDDEN                 Add scan_ssid=1 to generated config\n"
        "  /WPACONF file           Use persistent config; never delete it\n"
        "  /SD                     Stop old DHCP/delete lanN; connect restarts it\n"
        "  /NOSCAN                 Use the driver's existing scan cache\n"
        "  /QUIET                  Print errors only\n"
        "  /? | /H | /HELP         Show this help\n",
        PROGRAM_VERSION);
}

static int parse_unsigned(const char *text, unsigned long minimum,
                          unsigned long maximum, unsigned long *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !*text)
        return 0;
    parsed = strtoul(text, &end, 10);
    if (*end != 0 || parsed < minimum || parsed > maximum)
        return 0;
    *value = parsed;
    return 1;
}

static int parse_interface(const char *text, unsigned long *number)
{
    const char *p;
    unsigned long n;
    if (!text || strlen(text) < 4U || strlen(text) > IFACE_MAX_BYTES)
        return 0;
    if (tolower((unsigned char)text[0]) != 'l' ||
        tolower((unsigned char)text[1]) != 'a' ||
        tolower((unsigned char)text[2]) != 'n')
        return 0;
    p = text + 3;
    if (!isdigit((unsigned char)*p))
        return 0;
    n = 0;
    while (*p) {
        if (!isdigit((unsigned char)*p))
            return 0;
        n = n * 10UL + (unsigned long)(*p - '0');
        if (n > 99UL)
            return 0;
        ++p;
    }
    *number = n;
    return 1;
}

static int require_value(int argc, char **argv, int *index, const char **value)
{
    if (*index + 1 >= argc)
        return 0;
    ++*index;
    *value = argv[*index];
    return 1;
}

static int printable_value(const char *text, size_t minimum, size_t maximum)
{
    size_t i;
    size_t length;
    if (!text)
        return 0;
    length = strlen(text);
    if (length < minimum || length > maximum)
        return 0;
    for (i = 0; i < length; ++i)
        if (!isprint((unsigned char)text[i]) || text[i] == '\r' ||
            text[i] == '\n')
            return 0;
    return 1;
}

static int is_hex_key(const char *text)
{
    unsigned i;
    if (!text || strlen(text) != 64U)
        return 0;
    for (i = 0; i < 64U; ++i)
        if (!isxdigit((unsigned char)text[i]))
            return 0;
    return 1;
}

static int safe_command_path(const char *text)
{
    const char *p;
    if (!text || !*text || strlen(text) > WPA_PATH_MAX_BYTES)
        return 0;
    for (p = text; *p; ++p) {
        unsigned char ch = (unsigned char)*p;
        if (!isprint(ch) || ch == '"' || ch == '&' || ch == '|' ||
            ch == '<' || ch == '>' || ch == '\r' || ch == '\n')
            return 0;
    }
    return 1;
}

static int parse_options(int argc, char **argv, struct options *opt)
{
    int i;
    int have_ssid = 0;
    int have_iface = 0;
    const char *value;

    memset(opt, 0, sizeof(*opt));
    strcpy(opt->device, DEFAULT_DEVICE);
    opt->timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
    opt->poll_ms = DEFAULT_POLL_MS;
    opt->dhcp = DHCP_AUTO;
    opt->scan = 1;

    for (i = 1; i < argc; ++i) {
        if (option_equal(argv[i], "-?") || option_equal(argv[i], "-H") ||
            option_equal(argv[i], "-HELP") || ascii_equal(argv[i], "--HELP")) {
            usage(stdout);
            return 1;
        } else if (option_equal(argv[i], "-SSID")) {
            if (!require_value(argc, argv, &i, &value) ||
                strlen(value) < 1U || strlen(value) > SSID_MAX_BYTES) {
                fputs("SSIDCONNECT: -SSID must contain 1 through 32 bytes.\n", stderr);
                return -1;
            }
            strcpy(opt->ssid, value);
            have_ssid = 1;
        } else if (option_equal(argv[i], "-INT")) {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_interface(value, &opt->iface_number)) {
                fputs("SSIDCONNECT: -INT must be lan0 through lan99.\n", stderr);
                return -1;
            }
            strcpy(opt->iface, value);
            have_iface = 1;
        } else if (option_equal(argv[i], "-SCAN")) {
            opt->list_mode = 1;
        } else if (option_equal(argv[i], "-DISC")) {
            opt->disconnect_mode = 1;
        } else if (option_equal(argv[i], "-DEV")) {
            if (!require_value(argc, argv, &i, &value) || !*value ||
                strlen(value) > DEVICE_MAX_BYTES) {
                fputs("SSIDCONNECT: invalid -DEV value.\n", stderr);
                return -1;
            }
            strcpy(opt->device, value);
        } else if (option_equal(argv[i], "-TIMEOUT")) {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_unsigned(value, 1UL, 600UL, &opt->timeout_seconds)) {
                fputs("SSIDCONNECT: -TIMEOUT must be 1 through 600 seconds.\n", stderr);
                return -1;
            }
        } else if (option_equal(argv[i], "-POLL")) {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_unsigned(value, 50UL, 5000UL, &opt->poll_ms)) {
                fputs("SSIDCONNECT: -POLL must be 50 through 5000 milliseconds.\n", stderr);
                return -1;
            }
        } else if (option_equal(argv[i], "-DHCP")) {
            if (!require_value(argc, argv, &i, &value)) {
                fputs("SSIDCONNECT: -DHCP requires a value.\n", stderr);
                return -1;
            }
            if (ascii_equal(value, "auto")) opt->dhcp = DHCP_AUTO;
            else if (ascii_equal(value, "dhclient")) opt->dhcp = DHCP_DHCLIENT;
            else if (ascii_equal(value, "dhcpstrt")) opt->dhcp = DHCP_DHCPSTRT;
            else if (ascii_equal(value, "none")) opt->dhcp = DHCP_NONE;
            else {
                fputs("SSIDCONNECT: -DHCP must be auto, dhclient, dhcpstrt, or none.\n", stderr);
                return -1;
            }
            opt->dhcp_explicit = 1;
        } else if (option_equal(argv[i], "-WPA")) {
            if (!require_value(argc, argv, &i, &value)) {
                fputs("SSIDCONNECT: /WPA requires auto, wpa, wpa2, wpa3, or transition.\n",
                      stderr);
                return -1;
            }
            if (ascii_equal(value, "auto")) opt->wpa = WPA_MODE_AUTO;
            else if (ascii_equal(value, "wpa")) opt->wpa = WPA_MODE_WPA;
            else if (ascii_equal(value, "wpa2")) opt->wpa = WPA_MODE_WPA2;
            else if (ascii_equal(value, "wpa3")) opt->wpa = WPA_MODE_WPA3;
            else if (ascii_equal(value, "transition"))
                opt->wpa = WPA_MODE_TRANSITION;
            else {
                fputs("SSIDCONNECT: /WPA requires auto, wpa, wpa2, wpa3, or transition.\n",
                      stderr);
                return -1;
            }
        } else if (option_equal(argv[i], "-PSK")) {
            if (!require_value(argc, argv, &i, &value) ||
                !printable_value(value, 8U, WPA_SECRET_MAX_BYTES) ||
                (strlen(value) == 64U && !is_hex_key(value))) {
                fputs("SSIDCONNECT: /PSK must be 8-63 printable characters or 64 hexadecimal digits.\n",
                      stderr);
                return -1;
            }
            strcpy(opt->psk, value);
        } else if (option_equal(argv[i], "-SAE")) {
            if (!require_value(argc, argv, &i, &value) ||
                !printable_value(value, 1U, 63U)) {
                fputs("SSIDCONNECT: /SAE must contain 1-63 printable characters.\n",
                      stderr);
                return -1;
            }
            strcpy(opt->sae, value);
        } else if (option_equal(argv[i], "-CCMP")) {
            opt->cipher_ccmp = 1;
        } else if (option_equal(argv[i], "-TKIP")) {
            opt->cipher_tkip = 1;
        } else if (option_equal(argv[i], "-PMF")) {
            if (!require_value(argc, argv, &i, &value)) {
                fputs("SSIDCONNECT: /PMF requires disabled, optional, or required.\n",
                      stderr);
                return -1;
            }
            if (ascii_equal(value, "disabled")) opt->pmf = PMF_DISABLED;
            else if (ascii_equal(value, "optional")) opt->pmf = PMF_OPTIONAL;
            else if (ascii_equal(value, "required")) opt->pmf = PMF_REQUIRED;
            else {
                fputs("SSIDCONNECT: /PMF requires disabled, optional, or required.\n",
                      stderr);
                return -1;
            }
        } else if (option_equal(argv[i], "-HIDDEN")) {
            opt->hidden_ssid = 1;
        } else if (option_equal(argv[i], "-WPACONF")) {
            if (!require_value(argc, argv, &i, &value) ||
                !safe_command_path(value)) {
                fputs("SSIDCONNECT: /WPACONF requires a safe existing file path.\n",
                      stderr);
                return -1;
            }
            strcpy(opt->wpa_conf, value);
            opt->wpa_conf_existing = 1;
        } else if (option_equal(argv[i], "-NOSCAN")) {
            opt->scan = 0;
        } else if (option_equal(argv[i], "-SD")) {
            opt->stop_delete = 1;
        } else if (option_equal(argv[i], "-QUIET")) {
            opt->quiet = 1;
        } else {
            fprintf(stderr, "SSIDCONNECT: unknown option: %s\n", argv[i]);
            return -1;
        }
    }

    if (opt->list_mode) {
        if (have_ssid || have_iface || opt->disconnect_mode ||
            opt->stop_delete || opt->wpa != WPA_MODE_NONE ||
            opt->wpa_conf_existing || opt->psk[0] || opt->sae[0] ||
            opt->cipher_ccmp || opt->cipher_tkip ||
            opt->pmf != PMF_DEFAULT || opt->hidden_ssid) {
            fputs("SSIDCONNECT: /SCAN cannot be combined with connection, disconnect, or WPA options.\n",
                  stderr);
            return -1;
        }
        return 0;
    }

    if (opt->disconnect_mode) {
        if (!have_ssid) {
            fputs("SSIDCONNECT: /DISC requires /SSID.\n", stderr);
            return -1;
        }
        if (have_iface != opt->stop_delete) {
            fputs("SSIDCONNECT: with /DISC, /INT and /SD must be supplied together.\n",
                  stderr);
            return -1;
        }
        if (opt->dhcp_explicit || !opt->scan ||
            opt->wpa != WPA_MODE_NONE || opt->wpa_conf_existing ||
            opt->psk[0] || opt->sae[0] || opt->cipher_ccmp ||
            opt->cipher_tkip || opt->pmf != PMF_DEFAULT ||
            opt->hidden_ssid) {
            fputs("SSIDCONNECT: /DISC cannot be used with /DHCP, /NOSCAN, or WPA options.\n",
                  stderr);
            return -1;
        }
        return 0;
    }

    if (!have_ssid || !have_iface) {
        fputs("SSIDCONNECT: both -SSID and -INT are required.\n", stderr);
        usage(stderr);
        return -1;
    }
    if (opt->wpa_conf_existing) {
        if (opt->wpa != WPA_MODE_NONE || opt->psk[0] || opt->sae[0] ||
            opt->cipher_ccmp || opt->cipher_tkip ||
            opt->pmf != PMF_DEFAULT || opt->hidden_ssid) {
            fputs("SSIDCONNECT: /WPACONF cannot be combined with generated WPA options.\n",
                  stderr);
            return -1;
        }
        if (!opt->scan) {
            fputs("SSIDCONNECT: /NOSCAN is not used with /WPACONF.\n", stderr);
            return -1;
        }
    } else if (opt->wpa != WPA_MODE_NONE) {
        if (!printable_value(opt->ssid, 1U, SSID_MAX_BYTES)) {
            fputs("SSIDCONNECT: generated WPA configurations require a printable SSID.\n",
                  stderr);
            return -1;
        }
        if (!opt->scan) {
            fputs("SSIDCONNECT: /NOSCAN is not used with /WPA.\n", stderr);
            return -1;
        }
        if ((opt->wpa == WPA_MODE_WPA || opt->wpa == WPA_MODE_WPA2 ||
             opt->wpa == WPA_MODE_AUTO ||
             opt->wpa == WPA_MODE_TRANSITION) && !opt->psk[0]) {
            fputs("SSIDCONNECT: this /WPA mode requires /PSK.\n", stderr);
            return -1;
        }
        if (opt->wpa == WPA_MODE_WPA3 && !opt->sae[0] && !opt->psk[0]) {
            fputs("SSIDCONNECT: /WPA wpa3 requires /SAE or /PSK.\n", stderr);
            return -1;
        }
        if ((opt->wpa == WPA_MODE_WPA || opt->wpa == WPA_MODE_WPA2 ||
             opt->wpa == WPA_MODE_AUTO) && opt->sae[0]) {
            fputs("SSIDCONNECT: /SAE is only valid with wpa3 or transition mode.\n",
                  stderr);
            return -1;
        }
        if ((opt->wpa == WPA_MODE_WPA3 ||
             opt->wpa == WPA_MODE_TRANSITION) && opt->cipher_tkip) {
            fputs("SSIDCONNECT: WPA3 and transition modes do not permit /TKIP.\n",
                  stderr);
            return -1;
        }
        if (opt->wpa == WPA_MODE_WPA3 &&
            opt->pmf != PMF_DEFAULT && opt->pmf != PMF_REQUIRED) {
            fputs("SSIDCONNECT: WPA3 requires /PMF required.\n", stderr);
            return -1;
        }
        if (opt->wpa == WPA_MODE_TRANSITION &&
            opt->pmf != PMF_DEFAULT && opt->pmf != PMF_OPTIONAL) {
            fputs("SSIDCONNECT: transition mode requires /PMF optional.\n",
                  stderr);
            return -1;
        }
        if ((opt->wpa == WPA_MODE_WPA3 ||
             opt->wpa == WPA_MODE_TRANSITION) && !opt->sae[0] &&
            is_hex_key(opt->psk)) {
            fputs("SSIDCONNECT: a 64-digit raw PSK cannot be an SAE password; supply /SAE.\n",
                  stderr);
            return -1;
        }
    } else if (opt->psk[0] || opt->sae[0] || opt->cipher_ccmp ||
               opt->cipher_tkip || opt->pmf != PMF_DEFAULT ||
               opt->hidden_ssid) {
        fputs("SSIDCONNECT: /PSK, /SAE, cipher, /PMF, and /HIDDEN require /WPA.\n",
              stderr);
        return -1;
    }
    if (opt->stop_delete &&
        (opt->dhcp == DHCP_DHCLIENT || opt->dhcp == DHCP_NONE)) {
        fputs("SSIDCONNECT: -SD requires DHCPSTRT; do not combine it with -DHCP dhclient or none.\n",
              stderr);
        return -1;
    }
    if (opt->stop_delete)
        opt->dhcp = DHCP_DHCPSTRT;
    return 0;
}

static int write_config_string(FILE *file, const char *name,
                               const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    if (fprintf(file, "    %s=\"", name) < 0)
        return 0;
    while (*p) {
        if (*p == '\\' || *p == '"')
            if (fputc('\\', file) == EOF)
                return 0;
        if (fputc(*p++, file) == EOF)
            return 0;
    }
    return fputs("\"\n", file) != EOF;
}

static const char *wpa_mode_label(enum wpa_mode mode)
{
    switch (mode) {
    case WPA_MODE_WPA: return "WPA";
    case WPA_MODE_WPA2: return "WPA2";
    case WPA_MODE_WPA3: return "WPA3-SAE";
    case WPA_MODE_TRANSITION: return "WPA2/WPA3 transition";
    default: return "WPA/WPA2 auto";
    }
}

static int write_generated_wpa_config(const struct options *opt,
                                      const char *path)
{
    FILE *file;
    const char *pairwise;
    const char *sae_password;
    int pmf = -1;

    file = fopen(path, "w");
    if (!file)
        return 0;
    if (fputs("# Temporary configuration generated by SSIDCONNECT\n"
              "ap_scan=2\n"
              "update_config=0\n"
              "network={\n", file) == EOF ||
        !write_config_string(file, "ssid", opt->ssid)) {
        fclose(file);
        remove(path);
        return 0;
    }
    if (opt->hidden_ssid && fputs("    scan_ssid=1\n", file) == EOF)
        goto failed;

    if (opt->cipher_ccmp || opt->cipher_tkip) {
        if (opt->cipher_ccmp && opt->cipher_tkip)
            pairwise = "CCMP TKIP";
        else if (opt->cipher_tkip)
            pairwise = "TKIP";
        else
            pairwise = "CCMP";
    } else if (opt->wpa == WPA_MODE_WPA)
        pairwise = "TKIP";
    else if (opt->wpa == WPA_MODE_AUTO)
        pairwise = "CCMP TKIP";
    else
        pairwise = "CCMP";

    switch (opt->wpa) {
    case WPA_MODE_WPA:
        if (fputs("    key_mgmt=WPA-PSK\n    proto=WPA\n", file) == EOF)
            goto failed;
        break;
    case WPA_MODE_WPA2:
        if (fputs("    key_mgmt=WPA-PSK\n    proto=RSN\n", file) == EOF)
            goto failed;
        break;
    case WPA_MODE_WPA3:
        if (fputs("    key_mgmt=SAE\n    proto=RSN\n", file) == EOF)
            goto failed;
        pmf = 2;
        break;
    case WPA_MODE_TRANSITION:
        if (fputs("    key_mgmt=WPA-PSK SAE\n    proto=RSN\n", file) == EOF)
            goto failed;
        pmf = 1;
        break;
    default:
        if (fputs("    key_mgmt=WPA-PSK\n    proto=WPA RSN\n", file) == EOF)
            goto failed;
        break;
    }
    if (fprintf(file, "    pairwise=%s\n    group=%s\n",
                pairwise, pairwise) < 0)
        goto failed;

    if (opt->pmf == PMF_DISABLED) pmf = 0;
    else if (opt->pmf == PMF_OPTIONAL) pmf = 1;
    else if (opt->pmf == PMF_REQUIRED) pmf = 2;
    if (pmf >= 0 && fprintf(file, "    ieee80211w=%d\n", pmf) < 0)
        goto failed;

    if (opt->wpa != WPA_MODE_WPA3) {
        if (is_hex_key(opt->psk)) {
            if (fprintf(file, "    psk=%s\n", opt->psk) < 0)
                goto failed;
        } else if (!write_config_string(file, "psk", opt->psk))
            goto failed;
    }
    if (opt->wpa == WPA_MODE_WPA3 ||
        opt->wpa == WPA_MODE_TRANSITION) {
        sae_password = opt->sae[0] ? opt->sae : opt->psk;
        if (!write_config_string(file, "sae_password", sae_password))
            goto failed;
    }
    if (fputs("}\n", file) == EOF || fclose(file) != 0) {
        remove(path);
        return 0;
    }
    return 1;

failed:
    fclose(file);
    remove(path);
    return 0;
}

static int prepare_wpa_config(const struct options *opt, char *path,
                              int *temporary)
{
    FILE *file;
    const char *directory;
    unsigned attempt;
    unsigned long stamp;
    if (opt->wpa_conf_existing) {
        file = fopen(opt->wpa_conf, "r");
        if (!file)
            return 0;
        fclose(file);
        strcpy(path, opt->wpa_conf);
        *temporary = 0;
        return 1;
    }
    directory = getenv("TMP");
    if (!directory || !*directory)
        directory = getenv("TEMP");
    if (!directory || !*directory)
        directory = ".";
    if (!safe_command_path(directory) || strlen(directory) > 210U)
        return 0;
    stamp = (unsigned long)time(NULL) ^ (unsigned long)clock();
    for (attempt = 0; attempt < 100U; ++attempt) {
#if defined(__OS2__)
        sprintf(path, "%s\\SC%08lx%02u.CFG", directory, stamp, attempt);
#else
        sprintf(path, "%s/SC%08lx%02u.CFG", directory, stamp, attempt);
#endif
        file = fopen(path, "r");
        if (!file)
            break;
        fclose(file);
    }
    if (attempt == 100U || !safe_command_path(path))
        return 0;
    *temporary = 1;
    return write_generated_wpa_config(opt, path);
}

#if defined(__OS2__)

static int platform_open_driver(const char *device, DRIVER_HANDLE *handle,
                                unsigned long *os2_rc)
{
    ULONG action = 0;
    ULONG mode = OPEN_ACCESS_READWRITE | OPEN_SHARE_DENYNONE;
    APIRET rc = DosOpen((PSZ)device, handle, &action, 0, FILE_NORMAL,
                        OPEN_ACTION_OPEN_IF_EXISTS | OPEN_ACTION_FAIL_IF_NEW,
                        mode, NULL);
    *os2_rc = (unsigned long)rc;
    return rc == NO_ERROR;
}

static void platform_close_driver(DRIVER_HANDLE handle)
{
    DosClose(handle);
}

static int platform_oid_ex(DRIVER_HANDLE handle, unsigned long function,
                           uint32_t oid, void *data, unsigned long data_length,
                           unsigned long *data_actual,
                           struct oid_error *error)
{
    ULONG parameters[4];
    ULONG parameter_length = sizeof(parameters);
    ULONG returned_length = (ULONG)data_length;
    APIRET rc;

    memset(parameters, 0, sizeof(parameters));
    parameters[0] = (ULONG)oid;
    parameters[1] = (ULONG)data_length;
    rc = DosDevIOCtl(handle, (ULONG)GENMAC_CATEGORY, (ULONG)function,
                     parameters, sizeof(parameters), &parameter_length,
                     data, (ULONG)data_length, &returned_length);
    error->os2_rc = (unsigned long)rc;
    error->ndis_status = (uint32_t)parameters[3];
    if (data_actual)
        *data_actual = (unsigned long)returned_length;
    return rc == NO_ERROR && parameters[3] == 0;
}

static int platform_oid(DRIVER_HANDLE handle, unsigned long function,
                        uint32_t oid, void *data, unsigned long data_length,
                        struct oid_error *error)
{
    return platform_oid_ex(handle, function, oid, data, data_length, NULL,
                           error);
}

static void platform_sleep(unsigned long milliseconds)
{
    DosSleep((ULONG)milliseconds);
}

static int find_program(const char *name, char *path, unsigned long path_size)
{
    APIRET rc = DosSearchPath(SEARCH_CUR_DIRECTORY | SEARCH_ENVIRONMENT |
                              SEARCH_IGNORENETERRS,
                              (PSZ)"PATH", (PSZ)name, (PBYTE)path,
                              (ULONG)path_size);
    return rc == NO_ERROR;
}

static int start_background_command(const char *title, const char *command,
                                    unsigned long *session_id,
                                    unsigned long *os2_rc)
{
    STARTDATA start;
    ULONG session = 0;
    PID pid = 0;
    APIRET rc;

    memset(&start, 0, sizeof(start));
    start.Length = sizeof(start);
    start.Related = SSF_RELATED_INDEPENDENT;
    start.FgBg = SSF_FGBG_BACK;
    start.TraceOpt = SSF_TRACEOPT_NONE;
    start.PgmTitle = (PSZ)title;
    start.PgmName = (PSZ)"CMD.EXE";
    start.PgmInputs = (PBYTE)command;
    start.InheritOpt = SSF_INHERTOPT_PARENT;
    start.SessionType = SSF_TYPE_DEFAULT;
    start.PgmControl = SSF_CONTROL_INVISIBLE;

    rc = DosStartSession(&start, &session, &pid);
    if (rc == ERROR_SMG_START_IN_BACKGROUND)
        rc = NO_ERROR;
    *session_id = (unsigned long)session;
    *os2_rc = (unsigned long)rc;
    return rc == NO_ERROR;
}

static int platform_launch_wpa(const struct options *opt, const char *config,
                               int temporary, unsigned long *session_id,
                               unsigned long *os2_rc)
{
    char executable[260];
    char command[768];
    if (!find_program("WPA_SUPPLICANT.EXE", executable,
                      sizeof(executable))) {
        if (temporary)
            remove(config);
        *os2_rc = 2;
        return 0;
    }
    if (temporary)
        sprintf(command,
                "/C WPA_SUPPLICANT.EXE -i %s -c \"%s\" & del \"%s\"",
                opt->iface, config, config);
    else
        sprintf(command, "/C WPA_SUPPLICANT.EXE -i %s -c \"%s\"",
                opt->iface, config);
    if (!start_background_command("SSIDCONNECT - WPA Supplicant", command,
                                  session_id, os2_rc)) {
        if (temporary)
            remove(config);
        return 0;
    }
    return 1;
}

static int run_stop_delete(const char *iface, int quiet,
                           const char **missing_program,
                           unsigned long *command_status)
{
    char path[260];
    char command[128];
    const char *commands[4];
    unsigned i;
    int rc;

    if (!find_program("DHCPMON.EXE", path, sizeof(path))) {
        *missing_program = "DHCPMON.EXE";
        *command_status = 2;
        return 0;
    }
    if (!find_program("IFCONFIG.EXE", path, sizeof(path))) {
        *missing_program = "IFCONFIG.EXE";
        *command_status = 2;
        return 0;
    }

    commands[0] = "dhcpmon -r";
    commands[1] = "dhcpmon -t";
    sprintf(command, "ifconfig %s down", iface);
    commands[2] = command;
    commands[3] = NULL;

    for (i = 0; i < 2U; ++i) {
        if (!quiet)
            printf("SSIDCONNECT: -SD running: %s\n", commands[i]);
        fflush(stdout);
        rc = system(commands[i]);
        if (rc != 0 && !quiet)
            fprintf(stderr,
                    "SSIDCONNECT: warning: '%s' returned %d; continuing cleanup.\n",
                    commands[i], rc);
    }

    if (!quiet)
        printf("SSIDCONNECT: -SD running: %s\n", commands[2]);
    fflush(stdout);
    rc = system(commands[2]);
    if (rc != 0 && !quiet)
        fprintf(stderr,
                "SSIDCONNECT: warning: '%s' returned %d; continuing cleanup.\n",
                commands[2], rc);

    sprintf(command, "ifconfig %s delete", iface);
    if (!quiet)
        printf("SSIDCONNECT: -SD running: %s\n", command);
    fflush(stdout);
    rc = system(command);
    if (rc != 0 && !quiet)
        fprintf(stderr,
                "SSIDCONNECT: warning: '%s' returned %d; attempting DHCPSTRT.\n",
                command, rc);

    *missing_program = NULL;
    *command_status = 0;
    return 1;
}

static int platform_stop_delete(const char *iface, int quiet,
                                const char **failed_at,
                                unsigned long *os2_rc)
{
    return run_stop_delete(iface, quiet, failed_at, os2_rc);
}

static int platform_start_dhcp(enum dhcp_mode mode, const char *iface,
                               int stop_delete, int quiet,
                               const char **selected, unsigned long *session_id,
                               unsigned long *os2_rc)
{
    char path[260];
    char command[384];
    const char *program = NULL;

    if (mode == DHCP_NONE) {
        *selected = "none";
        *session_id = 0;
        *os2_rc = 0;
        return 1;
    }

    if ((mode == DHCP_AUTO || mode == DHCP_DHCLIENT) && !stop_delete &&
        find_program("DHCLIENT.EXE", path, sizeof(path))) {
        program = "DHCLIENT.EXE";
        sprintf(command, "/C %s %s", program, iface);
    } else if ((mode == DHCP_AUTO || mode == DHCP_DHCPSTRT) &&
               find_program("DHCPSTRT.EXE", path, sizeof(path))) {
        program = "DHCPSTRT.EXE";
        if (stop_delete)
            sprintf(command, "/C %s -i %s", program, iface);
        else
            sprintf(command, "/C %s -i %s -d 0", program, iface);
    } else {
        *selected = mode == DHCP_DHCLIENT ? "DHCLIENT.EXE" :
                    mode == DHCP_DHCPSTRT ? "DHCPSTRT.EXE" :
                    "DHCLIENT.EXE or DHCPSTRT.EXE";
        *session_id = 0;
        *os2_rc = 2; /* ERROR_FILE_NOT_FOUND */
        return 0;
    }

    if (stop_delete) {
        const char *missing_program = NULL;
        if (!platform_stop_delete(iface, quiet, &missing_program, os2_rc)) {
            *selected = missing_program;
            *session_id = 0;
            return 0;
        }
        if (!quiet)
            printf("SSIDCONNECT: -SD running: dhcpstrt -i %s\n", iface);
    }

    *selected = program;
    return start_background_command(program, command, session_id, os2_rc);
}

#else /* SSIDCONNECT_SIMULATE */

static struct ndis_ssid simulated_ssid;
static unsigned simulated_polls;
static int simulated_connected;

static int platform_open_driver(const char *device, DRIVER_HANDLE *handle,
                                unsigned long *os2_rc)
{
    (void)device;
    *handle = 1;
    *os2_rc = 0;
    simulated_polls = 0;
    memset(&simulated_ssid, 0, sizeof(simulated_ssid));
    simulated_ssid.length = 7;
    memcpy(simulated_ssid.ssid, "LWGuest", 7);
    simulated_connected = 1;
    return 1;
}

static void platform_close_driver(DRIVER_HANDLE handle)
{
    (void)handle;
}

static int platform_oid_ex(DRIVER_HANDLE handle, unsigned long function,
                           uint32_t oid, void *data, unsigned long data_length,
                           unsigned long *data_actual,
                           struct oid_error *error)
{
    uint32_t value;
    (void)handle;
    error->os2_rc = 0;
    error->ndis_status = 0;
    if (data_actual)
        *data_actual = data_length;

    if (function == GENMAC_WRAPPER_OID_SET && oid == OID_802_11_SSID) {
        if (data_length != sizeof(simulated_ssid))
            return 0;
        memcpy(&simulated_ssid, data, sizeof(simulated_ssid));
        simulated_polls = 0;
        simulated_connected = 0;
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_SET &&
        oid == OID_802_11_DISASSOCIATE) {
        simulated_connected = 0;
        memset(&simulated_ssid, 0, sizeof(simulated_ssid));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET &&
        oid == OID_PRIVATE_WRAPPER_LINKSTATUS) {
        if (!simulated_connected && simulated_ssid.length != 0 &&
            ++simulated_polls >= 3U)
            simulated_connected = 1;
        value = simulated_connected ? NDIS_LINK_CONNECTED : 0UL;
        memcpy(data, &value, sizeof(value));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET && oid == OID_GEN_MEDIA_CONNECT_STATUS) {
        value = simulated_connected ? NDIS_MEDIA_CONNECTED : 1UL;
        memcpy(data, &value, sizeof(value));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET && oid == OID_802_11_SSID) {
        memcpy(data, &simulated_ssid, sizeof(simulated_ssid));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET && oid == OID_802_11_BSSID) {
        static const unsigned char bssid[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
        memcpy(data, bssid, sizeof(bssid));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET && oid == OID_802_11_RSSI) {
        int32_t rssi = -42;
        memcpy(data, &rssi, sizeof(rssi));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET && oid == OID_PRIVATE_WRAPPER_LANNUMBER) {
        value = 1;
        memcpy(data, &value, sizeof(value));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET && oid == OID_PRIVATE_WRAPPER_ISWIRELESS) {
        value = 1;
        memcpy(data, &value, sizeof(value));
        return 1;
    }
    if (function == GENMAC_WRAPPER_OID_GET && oid == OID_802_11_BSSID_LIST) {
        unsigned char *out = (unsigned char *)data;
        static const unsigned char mac1[6] = {0x02,0x11,0x22,0x33,0x44,0x55};
        static const unsigned char mac2[6] = {0x02,0xaa,0xbb,0xcc,0xdd,0xee};
        uint32_t count = 2;
        uint32_t record = BSSID_EX_FIXED;
        uint32_t ssid_length;
        int32_t rssi;
        unsigned long required = 4UL + 2UL * BSSID_EX_FIXED;
        if (data_length < required) {
            error->os2_rc = 111;
            return 0;
        }
        memset(out, 0, (size_t)required);
        memcpy(out, &count, 4);
        memcpy(out + 4, &record, 4);
        memcpy(out + 8, mac1, 6);
        ssid_length = 7;
        memcpy(out + 16, &ssid_length, 4);
        memcpy(out + 20, "LWGuest", 7);
        rssi = -42;
        memcpy(out + 56, &rssi, 4);
        value = 3; memcpy(out + 60, &value, 4);
        value = 32; memcpy(out + 64, &value, 4);
        value = 100; memcpy(out + 68, &value, 4);
        value = 2412000UL; memcpy(out + 76, &value, 4);
        value = 16; memcpy(out + 80, &value, 4);
        value = 1; memcpy(out + 96, &value, 4);

        memcpy(out + 120, &record, 4);
        memcpy(out + 124, mac2, 6);
        ssid_length = 7;
        memcpy(out + 132, &ssid_length, 4);
        memcpy(out + 136, "ShopNet", 7);
        value = 1; memcpy(out + 168, &value, 4);
        rssi = -67; memcpy(out + 172, &rssi, 4);
        value = 2; memcpy(out + 176, &value, 4);
        value = 32; memcpy(out + 180, &value, 4);
        value = 100; memcpy(out + 184, &value, 4);
        value = 5180000UL; memcpy(out + 192, &value, 4);
        value = 16; memcpy(out + 196, &value, 4);
        value = 1; memcpy(out + 212, &value, 4);
        if (data_actual)
            *data_actual = required;
        return 1;
    }
    (void)data;
    (void)data_length;
    return 1;
}

static int platform_oid(DRIVER_HANDLE handle, unsigned long function,
                        uint32_t oid, void *data, unsigned long data_length,
                        struct oid_error *error)
{
    return platform_oid_ex(handle, function, oid, data, data_length, NULL,
                           error);
}

static void platform_sleep(unsigned long milliseconds)
{
    (void)milliseconds;
}

static int platform_stop_delete(const char *iface, int quiet,
                                const char **failed_at,
                                unsigned long *os2_rc)
{
    if (!quiet) {
        printf("SSIDCONNECT: -SD running: dhcpmon -r\n");
        printf("SSIDCONNECT: -SD running: dhcpmon -t\n");
        printf("SSIDCONNECT: -SD running: ifconfig %s down\n", iface);
        printf("SSIDCONNECT: -SD running: ifconfig %s delete\n", iface);
    }
    *failed_at = NULL;
    *os2_rc = 0;
    return 1;
}

static int platform_launch_wpa(const struct options *opt, const char *config,
                               int temporary, unsigned long *session_id,
                               unsigned long *os2_rc)
{
    FILE *src;
    FILE *dst;
    char buffer[512];
    size_t count;

    if (temporary) {
        src = fopen(config, "rb");
        dst = fopen("wpa-generated-test.conf", "wb");
        if (!src || !dst) {
            if (src) fclose(src);
            if (dst) fclose(dst);
            *os2_rc = 5;
            return 0;
        }
        while ((count = fread(buffer, 1, sizeof(buffer), src)) != 0)
            if (fwrite(buffer, 1, count, dst) != count) {
                fclose(src); fclose(dst); remove(config);
                *os2_rc = 5;
                return 0;
            }
        fclose(src);
        fclose(dst);
        remove(config); /* simulated supplicant has exited */
    }
    simulated_ssid.length = (uint32_t)strlen(opt->ssid);
    memcpy(simulated_ssid.ssid, opt->ssid, simulated_ssid.length);
    simulated_connected = 0;
    simulated_polls = 0;
    *session_id = 77;
    *os2_rc = 0;
    return 1;
}

static int platform_start_dhcp(enum dhcp_mode mode, const char *iface,
                               int stop_delete, int quiet,
                               const char **selected, unsigned long *session_id,
                               unsigned long *os2_rc)
{
    const char *failed_at = NULL;
    if (stop_delete &&
        !platform_stop_delete(iface, quiet, &failed_at, os2_rc)) {
        *selected = failed_at;
        *session_id = 0;
        return 0;
    }
    if (stop_delete && !quiet)
        printf("SSIDCONNECT: -SD running: dhcpstrt -i %s\n", iface);
    *selected = mode == DHCP_DHCPSTRT ? "DHCPSTRT.EXE" :
                mode == DHCP_NONE ? "none" : "DHCLIENT.EXE";
    *session_id = mode == DHCP_NONE ? 0UL : 42UL;
    *os2_rc = 0;
    return 1;
}

#endif

static int oid_get(DRIVER_HANDLE handle, uint32_t oid, void *data,
                   unsigned long length, struct oid_error *error)
{
    memset(data, 0, (size_t)length);
    return platform_oid(handle, GENMAC_WRAPPER_OID_GET, oid, data, length, error);
}

static int oid_get_ex(DRIVER_HANDLE handle, uint32_t oid, void *data,
                      unsigned long length, unsigned long *actual,
                      struct oid_error *error)
{
    memset(data, 0, (size_t)length);
    return platform_oid_ex(handle, GENMAC_WRAPPER_OID_GET, oid, data, length,
                           actual, error);
}

static int oid_set(DRIVER_HANDLE handle, uint32_t oid, void *data,
                   unsigned long length, struct oid_error *error)
{
    return platform_oid(handle, GENMAC_WRAPPER_OID_SET, oid, data, length, error);
}

static void report_oid_failure(const char *operation, uint32_t oid,
                               const struct oid_error *error)
{
    fprintf(stderr,
        "SSIDCONNECT: %s failed for OID %08lx (OS/2 rc %lu, NDIS status %08lx).\n",
        operation, (unsigned long)oid, error->os2_rc,
        (unsigned long)error->ndis_status);
}

static int set_u32(DRIVER_HANDLE handle, uint32_t oid, uint32_t value,
                   const char *operation)
{
    struct oid_error error;
    if (!oid_set(handle, oid, &value, sizeof(value), &error)) {
        report_oid_failure(operation, oid, &error);
        return 0;
    }
    return 1;
}

static uint32_t read_u32(const unsigned char *p)
{
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static int32_t read_i32(const unsigned char *p)
{
    int32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static unsigned long frequency_channel(uint32_t khz)
{
    unsigned long mhz = (unsigned long)(khz / 1000UL);
    if (mhz == 2484UL)
        return 14UL;
    if (mhz >= 2412UL && mhz <= 2472UL)
        return (mhz - 2407UL) / 5UL;
    if (mhz >= 5000UL && mhz <= 5900UL)
        return (mhz - 5000UL) / 5UL;
    if (mhz >= 5955UL && mhz <= 7115UL)
        return (mhz - 5950UL) / 5UL;
    return 0UL;
}

static void copy_printable_ssid(char out[SSID_MAX_BYTES + 1U],
                                const unsigned char *ssid, uint32_t length)
{
    unsigned i;
    if (length > SSID_MAX_BYTES)
        length = SSID_MAX_BYTES;
    if (length == 0) {
        strcpy(out, "<hidden>");
        return;
    }
    for (i = 0; i < (unsigned)length; ++i)
        out[i] = isprint(ssid[i]) ? (char)ssid[i] : '?';
    out[length] = 0;
}

static int list_scan_cache(DRIVER_HANDLE handle, int request_scan, int quiet)
{
    unsigned char *list;
    unsigned long actual = 0;
    unsigned long offset;
    uint32_t count;
    uint32_t i;
    struct oid_error error;

    if (request_scan) {
        if (!set_u32(handle, OID_802_11_BSSID_LIST_SCAN, 0,
                     "request scan"))
            return 0;
        /* XWLAN allows the adapter a short settling period before reading the
         * variable-length BSSID list. The current IWM IOCTL is synchronous,
         * but retaining the delay also supports other GenMAC adapters. */
        platform_sleep(200UL);
    }

    list = (unsigned char *)malloc((size_t)SCAN_LIST_BYTES);
    if (!list) {
        fputs("SSIDCONNECT: unable to allocate the scan-list buffer.\n", stderr);
        return 0;
    }
    if (!oid_get_ex(handle, OID_802_11_BSSID_LIST, list, SCAN_LIST_BYTES,
                    &actual, &error)) {
        report_oid_failure("read scan cache", OID_802_11_BSSID_LIST, &error);
        free(list);
        return 0;
    }
    if (actual < 4UL || actual > SCAN_LIST_BYTES) {
        fputs("SSIDCONNECT: driver returned an invalid scan-list length.\n",
              stderr);
        free(list);
        return 0;
    }

    count = read_u32(list);
    if (!quiet)
        printf("SSIDCONNECT: %s scan cache contains %lu network%s.\n",
               request_scan ? "refreshed" : "existing",
               (unsigned long)count, count == 1 ? "" : "s");
    if (count == 0) {
        free(list);
        return 1;
    }

    printf("%-32s %-17s %4s %9s %9s %s\n",
           "SSID", "BSSID", "CH", "RSSI(dBm)", "FREQ(MHz)", "SECURITY");
    offset = 4UL;
    for (i = 0; i < count; ++i) {
        uint32_t record_length;
        uint32_t ssid_length;
        uint32_t privacy;
        uint32_t frequency;
        int32_t rssi;
        unsigned long channel;
        char ssid_text[SSID_MAX_BYTES + 1U];
        const unsigned char *record;
        const unsigned char *mac;

        if (offset > actual || actual - offset < BSSID_EX_FIXED) {
            fputs("SSIDCONNECT: truncated scan-list record.\n", stderr);
            free(list);
            return 0;
        }
        record = list + offset;
        record_length = read_u32(record);
        if (record_length < BSSID_EX_FIXED ||
            (unsigned long)record_length > actual - offset) {
            fputs("SSIDCONNECT: invalid scan-list record length.\n", stderr);
            free(list);
            return 0;
        }
        mac = record + 4;
        ssid_length = read_u32(record + 12);
        if (ssid_length > SSID_MAX_BYTES) {
            fputs("SSIDCONNECT: invalid SSID length in scan cache.\n", stderr);
            free(list);
            return 0;
        }
        copy_printable_ssid(ssid_text, record + 16, ssid_length);
        privacy = read_u32(record + 48);
        rssi = read_i32(record + 52);
        frequency = read_u32(record + 72);
        channel = frequency_channel(frequency);
        printf("%-32s %02x:%02x:%02x:%02x:%02x:%02x %4lu %9ld %9lu %s\n",
               ssid_text, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
               channel, (long)rssi, (unsigned long)(frequency / 1000UL),
               privacy ? "secured" : "open");
        offset += (unsigned long)record_length;
    }
    free(list);
    return 1;
}

static int prepare_open_network(DRIVER_HANDLE handle, int scan)
{
    uint32_t value;
    unsigned i;

    if (!set_u32(handle, OID_802_11_AUTHENTICATION_MODE,
                 NDIS_802_11_AUTH_AUTO, "set automatic authentication"))
        return 0;

    for (i = 0; i < 4U; ++i) {
        value = (uint32_t)i;
        if (!set_u32(handle, OID_802_11_REMOVE_WEP, value,
                     "remove stale WEP key"))
            return 0;
    }

    if (!set_u32(handle, OID_802_11_INFRASTRUCTURE_MODE,
                 NDIS_802_11_INFRASTRUCTURE, "set infrastructure mode") ||
        !set_u32(handle, OID_802_11_AUTHENTICATION_MODE,
                 NDIS_802_11_AUTH_AUTO, "set authentication mode") ||
        !set_u32(handle, OID_802_11_ENCRYPTION_STATUS,
                 NDIS_802_11_ENCRYPTION_DISABLED, "disable encryption") ||
        !set_u32(handle, OID_802_11_PRIVACY_FILTER,
                 NDIS_802_11_PRIVACY_ACCEPT_ALL, "set privacy filter"))
        return 0;

    if (scan) {
        value = 0;
        if (!set_u32(handle, OID_802_11_BSSID_LIST_SCAN, value,
                     "request scan"))
            return 0;
    }
    return 1;
}

static int request_ssid(DRIVER_HANDLE handle, const char *ssid)
{
    struct ndis_ssid request;
    struct oid_error error;
    size_t length = strlen(ssid);

    memset(&request, 0, sizeof(request));
    request.length = (uint32_t)length;
    memcpy(request.ssid, ssid, length);
    if (!oid_set(handle, OID_802_11_SSID, &request, sizeof(request), &error)) {
        report_oid_failure("set SSID", OID_802_11_SSID, &error);
        return 0;
    }
    return 1;
}

static int verify_disconnect_target(DRIVER_HANDLE handle,
                                    const char *expected_ssid,
                                    unsigned char bssid[6])
{
    struct ndis_ssid current;
    struct oid_error error;
    uint32_t link = 0;
    uint32_t media = 1;

    if (!oid_get(handle, OID_PRIVATE_WRAPPER_LINKSTATUS,
                 &link, sizeof(link), &error)) {
        report_oid_failure("read link status",
                           OID_PRIVATE_WRAPPER_LINKSTATUS, &error);
        return 0;
    }
    if (!oid_get(handle, OID_GEN_MEDIA_CONNECT_STATUS,
                 &media, sizeof(media), &error)) {
        report_oid_failure("read media status",
                           OID_GEN_MEDIA_CONNECT_STATUS, &error);
        return 0;
    }
    if ((link & NDIS_LINK_CONNECTED) == 0 ||
        media != NDIS_MEDIA_CONNECTED) {
        fputs("SSIDCONNECT: adapter is not currently associated.\n", stderr);
        return 0;
    }
    if (!oid_get(handle, OID_802_11_SSID,
                 &current, sizeof(current), &error)) {
        report_oid_failure("read current SSID", OID_802_11_SSID, &error);
        return 0;
    }
    if (current.length > SSID_MAX_BYTES) {
        fputs("SSIDCONNECT: driver returned an invalid current SSID length.\n",
              stderr);
        return 0;
    }
    if (current.length != strlen(expected_ssid) ||
        memcmp(current.ssid, expected_ssid, current.length) != 0) {
        char current_text[SSID_MAX_BYTES + 1U];
        copy_printable_ssid(current_text, current.ssid, current.length);
        fprintf(stderr,
                "SSIDCONNECT: current SSID is \"%s\", not requested \"%s\"; disassociate not issued.\n",
                current_text, expected_ssid);
        return 0;
    }
    if (!oid_get(handle, OID_802_11_BSSID, bssid, 6, &error))
        memset(bssid, 0, 6);
    return 1;
}

static int request_disassociate(DRIVER_HANDLE handle)
{
    return set_u32(handle, OID_802_11_DISASSOCIATE, 0,
                   "request disassociation");
}

static int wait_for_disconnection(DRIVER_HANDLE handle,
                                  const struct options *opt)
{
    unsigned long elapsed = 0;
    uint32_t link = NDIS_LINK_CONNECTED;
    uint32_t media = NDIS_MEDIA_CONNECTED;
    struct ndis_ssid current;
    struct oid_error error;

    while (elapsed <= opt->timeout_seconds * 1000UL) {
        if (oid_get(handle, OID_PRIVATE_WRAPPER_LINKSTATUS,
                    &link, sizeof(link), &error) &&
            oid_get(handle, OID_GEN_MEDIA_CONNECT_STATUS,
                    &media, sizeof(media), &error) &&
            oid_get(handle, OID_802_11_SSID,
                    &current, sizeof(current), &error) &&
            (link & NDIS_LINK_CONNECTED) == 0 &&
            media != NDIS_MEDIA_CONNECTED && current.length == 0)
            return 1;
        if (elapsed == opt->timeout_seconds * 1000UL)
            break;
        platform_sleep(opt->poll_ms);
        elapsed += opt->poll_ms;
        if (elapsed > opt->timeout_seconds * 1000UL)
            elapsed = opt->timeout_seconds * 1000UL;
    }
    return 0;
}

static int read_link_details(DRIVER_HANDLE handle, const char *expected_ssid,
                             unsigned char bssid[6], int32_t *rssi)
{
    struct ndis_ssid current;
    struct oid_error error;

    if (!oid_get(handle, OID_802_11_SSID, &current, sizeof(current), &error))
        return 0;
    if (current.length != strlen(expected_ssid) || current.length > SSID_MAX_BYTES ||
        memcmp(current.ssid, expected_ssid, current.length) != 0)
        return 0;
    if (!oid_get(handle, OID_802_11_BSSID, bssid, 6, &error))
        memset(bssid, 0, 6);
    if (!oid_get(handle, OID_802_11_RSSI, rssi, sizeof(*rssi), &error))
        *rssi = 0;
    return 1;
}

static int wait_for_acknowledgement(DRIVER_HANDLE handle,
                                    const struct options *opt,
                                    unsigned char bssid[6], int32_t *rssi)
{
    unsigned long elapsed = 0;
    uint32_t link = 0;
    uint32_t media = 1;
    struct oid_error error;

    while (elapsed <= opt->timeout_seconds * 1000UL) {
        if (oid_get(handle, OID_PRIVATE_WRAPPER_LINKSTATUS,
                    &link, sizeof(link), &error) &&
            oid_get(handle, OID_GEN_MEDIA_CONNECT_STATUS,
                    &media, sizeof(media), &error) &&
            (link & NDIS_LINK_CONNECTED) != 0 &&
            media == NDIS_MEDIA_CONNECTED &&
            read_link_details(handle, opt->ssid, bssid, rssi))
            return 1;
        if (elapsed == opt->timeout_seconds * 1000UL)
            break;
        platform_sleep(opt->poll_ms);
        elapsed += opt->poll_ms;
        if (elapsed > opt->timeout_seconds * 1000UL)
            elapsed = opt->timeout_seconds * 1000UL;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct options opt;
    DRIVER_HANDLE driver;
    unsigned long os2_rc = 0;
    unsigned long driver_lan = 0;
    unsigned long session_id = 0;
    unsigned long wpa_session_id = 0;
    uint32_t wireless = 0;
    uint32_t lan_number = 0;
    struct oid_error error;
    unsigned char bssid[6];
    int32_t rssi = 0;
    const char *dhcp_program = NULL;
    char wpa_config[WPA_PATH_MAX_BYTES + 1U];
    int wpa_temporary = 0;
    int wpa_connect;
    int parsed;

    parsed = parse_options(argc, argv, &opt);
    if (parsed > 0)
        return 0;
    if (parsed < 0)
        return EXIT_USAGE;
    wpa_connect = opt.wpa != WPA_MODE_NONE || opt.wpa_conf_existing;

    if (!platform_open_driver(opt.device, &driver, &os2_rc)) {
        fprintf(stderr, "SSIDCONNECT: cannot open %s (OS/2 rc %lu).\n",
                opt.device, os2_rc);
        return EXIT_DRIVER;
    }

    if (!oid_get(driver, OID_PRIVATE_WRAPPER_ISWIRELESS,
                 &wireless, sizeof(wireless), &error) || wireless == 0) {
        fputs("SSIDCONNECT: selected GenMAC device is not a wireless adapter.\n",
              stderr);
        platform_close_driver(driver);
        return EXIT_DRIVER;
    }

    if (opt.list_mode) {
        int ok = list_scan_cache(driver, opt.scan, opt.quiet);
        platform_close_driver(driver);
        return ok ? 0 : EXIT_CONNECT;
    }

    if (opt.disconnect_mode) {
        if (!verify_disconnect_target(driver, opt.ssid, bssid)) {
            platform_close_driver(driver);
            return EXIT_CONNECT;
        }
        if (!opt.quiet)
            printf("SSIDCONNECT: disassociating from \"%s\"; BSSID %02x:%02x:%02x:%02x:%02x:%02x.\n",
                   opt.ssid, bssid[0], bssid[1], bssid[2], bssid[3],
                   bssid[4], bssid[5]);
        if (!request_disassociate(driver)) {
            platform_close_driver(driver);
            return EXIT_CONNECT;
        }
        if (!opt.quiet)
            printf("SSIDCONNECT: disassociate request acknowledged; waiting for link-down confirmation.\n");
        if (!wait_for_disconnection(driver, &opt)) {
            fprintf(stderr,
                    "SSIDCONNECT: no confirmed disassociation from \"%s\" within %lu seconds.\n",
                    opt.ssid, opt.timeout_seconds);
            platform_close_driver(driver);
            return EXIT_TIMEOUT;
        }
        if (!opt.quiet)
            printf("SSIDCONNECT: disassociated from \"%s\".\n", opt.ssid);
        platform_close_driver(driver);
        if (opt.stop_delete) {
            const char *failed_at = NULL;
            if (!platform_stop_delete(opt.iface, opt.quiet, &failed_at,
                                      &os2_rc)) {
                fprintf(stderr,
                        "SSIDCONNECT: disassociated, but /SD cleanup failed at %s for %s (OS/2 rc %lu).\n",
                        failed_at, opt.iface, os2_rc);
                return EXIT_DHCP;
            }
            if (!opt.quiet)
                printf("SSIDCONNECT: DHCP state stopped and %s deleted; DHCP was not restarted.\n",
                       opt.iface);
        }
        return 0;
    }

    if (oid_get(driver, OID_PRIVATE_WRAPPER_LANNUMBER,
                &lan_number, sizeof(lan_number), &error)) {
        driver_lan = (unsigned long)lan_number;
        if (!opt.quiet && driver_lan != opt.iface_number)
            fprintf(stderr,
                "SSIDCONNECT: warning: driver reports lan%lu; DHCP will use requested %s.\n",
                driver_lan, opt.iface);
    }

    if (wpa_connect) {
        if (!prepare_wpa_config(&opt, wpa_config, &wpa_temporary)) {
            fprintf(stderr,
                    "SSIDCONNECT: unable to %s WPA configuration file.\n",
                    opt.wpa_conf_existing ? "open existing" : "create temporary");
            platform_close_driver(driver);
            return EXIT_CONNECT;
        }
        if (!platform_launch_wpa(&opt, wpa_config, wpa_temporary,
                                 &wpa_session_id, &os2_rc)) {
            fprintf(stderr,
                    "SSIDCONNECT: unable to launch WPA_SUPPLICANT.EXE (OS/2 rc %lu).\n",
                    os2_rc);
            platform_close_driver(driver);
            return EXIT_CONNECT;
        }
        if (!opt.quiet) {
            if (wpa_temporary)
                printf("SSIDCONNECT: launched WPA_SUPPLICANT.EXE for %s using generated %s configuration (session %lu).\n",
                       opt.iface, wpa_mode_label(opt.wpa), wpa_session_id);
            else
                printf("SSIDCONNECT: launched WPA_SUPPLICANT.EXE for %s using persistent configuration \"%s\" (session %lu).\n",
                       opt.iface, wpa_config, wpa_session_id);
            if (wpa_temporary)
                printf("SSIDCONNECT: temporary WPA configuration will be deleted when WPA_SUPPLICANT.EXE exits.\n");
            printf("SSIDCONNECT: waiting for WPA association with \"%s\".\n",
                   opt.ssid);
        }
    } else {
        if (!opt.quiet)
            printf("SSIDCONNECT: requesting open-network association with \"%s\" via %s.\n",
                   opt.ssid, opt.device);

        if (!prepare_open_network(driver, opt.scan) ||
            !request_ssid(driver, opt.ssid)) {
            platform_close_driver(driver);
            return EXIT_CONNECT;
        }

        if (!opt.quiet)
            printf("SSIDCONNECT: SSID request acknowledged; waiting for link confirmation.\n");
    }

    if (!wait_for_acknowledgement(driver, &opt, bssid, &rssi)) {
        fprintf(stderr,
            "SSIDCONNECT: no confirmed association with \"%s\" within %lu seconds; DHCP not started.\n",
            opt.ssid, opt.timeout_seconds);
        platform_close_driver(driver);
        return EXIT_TIMEOUT;
    }

    if (!opt.quiet)
        printf("SSIDCONNECT: connected to \"%s\"; BSSID %02x:%02x:%02x:%02x:%02x:%02x, RSSI %ld dBm.\n",
               opt.ssid, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
               bssid[5], (long)rssi);

    platform_close_driver(driver);

    if (!platform_start_dhcp(opt.dhcp, opt.iface, opt.stop_delete, opt.quiet,
                             &dhcp_program, &session_id, &os2_rc)) {
        fprintf(stderr,
            "SSIDCONNECT: association succeeded, but DHCP preparation/launch failed at %s for %s (OS/2 rc %lu).\n",
            dhcp_program, opt.iface, os2_rc);
        return EXIT_DHCP;
    }

    if (!opt.quiet) {
        if (opt.dhcp == DHCP_NONE)
            printf("SSIDCONNECT: DHCP launch suppressed for %s.\n", opt.iface);
        else
            printf("SSIDCONNECT: started %s for %s (session %lu).\n",
                   dhcp_program, opt.iface, session_id);
    }
    return 0;
}
