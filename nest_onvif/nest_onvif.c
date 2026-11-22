/*
 * nest_onvif - Lightweight ONVIF Profile S + WS-Discovery server for UniFi Protect
 * Target: Gen 1 Nest Cam Outdoor (ARMv6 soft-float, Linux 2.6.38.8, glibc 2.13)
 *
 * Single-threaded select() server (no libpthread):
 *  - UDP multicast 239.255.255.250:3702 WS-Discovery Probe/ProbeMatches
 *  - TCP HTTP/SOAP on port 80 (/onvif/device_service, media_service, imaging_service)
 *  - Live JPEG snapshot on GET /snapshot.jpg (via nest_rtsp SIGUSR1 + /tmp/snapshot.jpg)
 *  - WS-Security UsernameToken (SHA-1 PasswordDigest & PasswordText) + HTTP Digest/Basic auth
 *  - ONVIF Imaging IrCutFilter <-> /tmp/night_vision (AUTO / ON / OFF)
 */

#include <stdio.h>
#include "../nest_rtsp/nest_audio.h"
#include "nest_events.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
extern int __xstat(int ver, const char *path, struct stat *buf);
static inline int stat_compat(const char *path, struct stat *buf) { return __xstat(3, path, buf); }
#define stat(p, b) stat_compat((p), (b))
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

static volatile int g_running = 1;
static void on_sig(int sig) { (void)sig; g_running = 0; }

static char g_auth_user[64] = "admin";
static char g_auth_pass[128] = "";
static int  g_http_port = 80;
static int  g_rtsp_port = 554;
static char g_iface[32] = "wlan0";

/* ---- Case-insensitive substring helper (avoids _GNU_SOURCE) ---- */
static const char *ci_strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return h;
    for (; *h; h++) {
        size_t i = 0;
        while (i < nl && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++;
        if (i == nl) return h;
    }
    return NULL;
}

/* ---- Base64 encode/decode ---- */
static const char b64_enc_tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t *in, size_t len, char *out, size_t out_max) {
    size_t i = 0, o = 0;
    while (i < len && o + 4 < out_max) {
        uint32_t v = ((uint32_t)in[i++]) << 16;
        size_t rem = len - i;
        if (rem >= 1) v |= ((uint32_t)in[i++]) << 8;
        if (rem >= 2) v |= ((uint32_t)in[i++]);
        out[o++] = b64_enc_tbl[(v >> 18) & 63];
        out[o++] = b64_enc_tbl[(v >> 12) & 63];
        out[o++] = (rem >= 1) ? b64_enc_tbl[(v >> 6) & 63] : '=';
        out[o++] = (rem >= 2) ? b64_enc_tbl[v & 63] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

static int b64_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int b64_decode(const char *in, uint8_t *out, size_t out_max) {
    uint32_t buf = 0;
    int bits = 0;
    size_t o = 0;
    for (; *in && *in != '='; in++) {
        int v = b64_val((unsigned char)*in);
        if (v < 0) continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o < out_max) out[o++] = (uint8_t)((buf >> bits) & 0xff);
        }
    }
    return (int)o;
}

/* ---- Minimal SHA-1 (RFC 3174) for WS-Security PasswordDigest ---- */
static uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_compute(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u };
    uint64_t bitlen = (uint64_t)len * 8u;
    size_t padded = ((len + 9 + 63) / 64) * 64;
    uint8_t *buf = (uint8_t *)calloc(1, padded);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bitlen >> (i * 8));

    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)buf[off + i*4] << 24) | ((uint32_t)buf[off + i*4 + 1] << 16) |
                   ((uint32_t)buf[off + i*4 + 2] << 8) | (uint32_t)buf[off + i*4 + 3];
        }
        for (int i = 16; i < 80; i++) w[i] = rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5a827999u; }
            else if (i < 40) { f = b ^ c ^ d;            k = 0x6ed9eba1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else             { f = b ^ c ^ d;            k = 0xca62c1d6u; }
            uint32_t t = rol32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol32(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(buf);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(h[i] >> 24);
        out[i*4+1] = (uint8_t)(h[i] >> 16);
        out[i*4+2] = (uint8_t)(h[i] >> 8);
        out[i*4+3] = (uint8_t)(h[i]);
    }
}

/* ---- Minimal MD5 (RFC 1321) for HTTP Digest Auth ---- */
static void md5_hex(const char *str, char out_hex[33]) {
    static const uint32_t k[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const uint8_t s[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    size_t len = strlen(str);
    uint64_t bitlen = (uint64_t)len * 8u;
    size_t padded = ((len + 9 + 63) / 64) * 64;
    uint8_t *buf = (uint8_t *)calloc(1, padded);
    if (!buf) { out_hex[0] = '\0'; return; }
    memcpy(buf, str, len);
    buf[len] = 0x80;
    for (int i = 0; i < 8; i++) buf[padded - 8 + i] = (uint8_t)(bitlen >> (i * 8));

    uint32_t a0 = 0x67452301u, b0 = 0xefcdab89u, c0 = 0x98badcfeu, d0 = 0x10325476u;
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            m[i] = (uint32_t)buf[off+i*4] | ((uint32_t)buf[off+i*4+1] << 8) |
                   ((uint32_t)buf[off+i*4+2] << 16) | ((uint32_t)buf[off+i*4+3] << 24);
        }
        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16)      { f = (b & c) | ((~b) & d); g = (uint32_t)i; }
            else if (i < 32) { f = (d & b) | ((~d) & c); g = (uint32_t)((5*i + 1) & 15); }
            else if (i < 48) { f = b ^ c ^ d;            g = (uint32_t)((3*i + 5) & 15); }
            else             { f = c ^ (b | (~d));       g = (uint32_t)((7*i) & 15); }
            uint32_t tmp = d;
            d = c; c = b;
            b = b + rol32(a + f + k[i] + m[g], s[i]);
            a = tmp;
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    free(buf);
    uint8_t dig[16];
    uint32_t st[4] = { a0, b0, c0, d0 };
    for (int i = 0; i < 4; i++) {
        dig[i*4]   = (uint8_t)(st[i]);
        dig[i*4+1] = (uint8_t)(st[i] >> 8);
        dig[i*4+2] = (uint8_t)(st[i] >> 16);
        dig[i*4+3] = (uint8_t)(st[i] >> 24);
    }
    for (int i = 0; i < 16; i++) sprintf(out_hex + i*2, "%02x", dig[i]);
    out_hex[32] = '\0';
}

/* ---- Network interface info (IP, netmask prefix, MAC, UUID) ---- */
typedef struct {
    char ip[32];
    char mac_colon[24];
    char mac_hex[16];
    char uuid[48];
    int  prefix_len;
} net_info_t;

static void get_net_info(const char *ifname, net_info_t *ni) {
    memset(ni, 0, sizeof(*ni));
    strcpy(ni->ip, "0.0.0.0");
    strcpy(ni->mac_colon, "02:00:00:00:00:00");
    strcpy(ni->mac_hex, "020000000000");
    ni->prefix_len = 24;

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
            inet_ntop(AF_INET, &sin->sin_addr, ni->ip, sizeof(ni->ip));
        }
        if (ioctl(s, SIOCGIFNETMASK, &ifr) == 0) {
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_netmask;
            uint32_t mask = ntohl(sin->sin_addr.s_addr);
            int p = 0;
            while (mask & 0x80000000u) { p++; mask <<= 1; }
            if (p > 0 && p <= 32) ni->prefix_len = p;
        }
        if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0) {
            const uint8_t *m = (const uint8_t *)ifr.ifr_hwaddr.sa_data;
            snprintf(ni->mac_colon, sizeof(ni->mac_colon),
                     "%02x:%02x:%02x:%02x:%02x:%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
            snprintf(ni->mac_hex, sizeof(ni->mac_hex),
                     "%02x%02x%02x%02x%02x%02x", m[0],m[1],m[2],m[3],m[4],m[5]);
        }
        close(s);
    }
    snprintf(ni->uuid, sizeof(ni->uuid), "2419d68a-2dd2-21b2-a205-%s", ni->mac_hex);
}

/* ---- XML tag extraction helper (namespace-prefix agnostic) ---- */
bool onvif_xml_get_tag(const char *xml, const char *local_name, char *out, size_t out_max) {
    size_t nlen = strlen(local_name);
    const char *p = xml;
    while ((p = strchr(p, '<')) != NULL) {
        p++;
        if (*p == '/' || *p == '?' || *p == '!') continue;
        const char *tag_start = p;
        while (*p && *p != '>' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '/') p++;
        const char *colon = memchr(tag_start, ':', (size_t)(p - tag_start));
        const char *lname = colon ? (colon + 1) : tag_start;
        size_t llen = (size_t)(p - lname);
        if (llen == nlen && strncasecmp(lname, local_name, nlen) == 0) {
            const char *gt = strchr(p, '>');
            if (!gt) return false;
            if (gt > tag_start && *(gt - 1) == '/') {
                out[0] = '\0';
                return true;
            }
            const char *val_start = gt + 1;
            const char *val_end = strchr(val_start, '<');
            if (!val_end) return false;
            while (val_start < val_end && isspace((unsigned char)*val_start)) val_start++;
            while (val_end > val_start && isspace((unsigned char)*(val_end - 1))) val_end--;
            size_t vlen = (size_t)(val_end - val_start);
            if (vlen >= out_max) vlen = out_max - 1;
            memcpy(out, val_start, vlen);
            out[vlen] = '\0';
            return true;
        }
    }
    return false;
}

/* Extract the local name of the first child element inside <...:Body> */
static bool xml_get_soap_action(const char *xml, char *action, size_t max_len) {
    const char *p = xml;
    while ((p = strchr(p, '<')) != NULL) {
        p++;
        if (*p == '/' || *p == '?' || *p == '!') continue;
        const char *ts = p;
        while (*p && *p != '>' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        const char *colon = memchr(ts, ':', (size_t)(p - ts));
        const char *lname = colon ? (colon + 1) : ts;
        if ((size_t)(p - lname) == 4 && strncasecmp(lname, "Body", 4) == 0) {
            const char *gt = strchr(p, '>');
            if (!gt) return false;
            p = gt + 1;
            while ((p = strchr(p, '<')) != NULL) {
                p++;
                if (*p == '?' || *p == '!') {
                    const char *e = strchr(p, '>');
                    if (!e) return false;
                    p = e + 1;
                    continue;
                }
                if (*p == '/') return false;
                const char *as = p;
                while (*p && *p != '>' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '/') p++;
                const char *ac = memchr(as, ':', (size_t)(p - as));
                const char *al = ac ? (ac + 1) : as;
                size_t alen = (size_t)(p - al);
                if (alen >= max_len) alen = max_len - 1;
                memcpy(action, al, alen);
                action[alen] = '\0';
                return alen > 0;
            }
        }
    }
    return false;
}

/* ---- Credentials & Authentication (WS-Security + HTTP Digest + HTTP Basic) ---- */
static void load_credentials(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *sep = strchr(line, ':');
        if (sep) {
            *sep = '\0';
            if (line[0]) strncpy(g_auth_user, line, sizeof(g_auth_user) - 1);
            strncpy(g_auth_pass, sep + 1, sizeof(g_auth_pass) - 1);
        }
    }
    fclose(f);
}

static bool username_ok(const char *u) {
    if (!u || !u[0]) return false;
    if (strcmp(u, g_auth_user) == 0) return true;
    /* Convenience alias: accept 'root' alongside 'admin' with the same password */
    if (strcmp(g_auth_user, "admin") == 0 && strcmp(u, "root") == 0) return true;
    return false;
}

static bool extract_digest_param(const char *hdr, const char *key, char *out, size_t max_len) {
    char pat[48];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = ci_strstr(hdr, pat);
    if (!p) return false;
    p += strlen(pat);
    if (*p == '"') {
        p++;
        const char *e = strchr(p, '"');
        if (!e) return false;
        size_t n = (size_t)(e - p);
        if (n >= max_len) n = max_len - 1;
        memcpy(out, p, n); out[n] = '\0';
        return true;
    } else {
        const char *e = p;
        while (*e && *e != ',' && *e != '\r' && *e != '\n' && *e != ' ') e++;
        size_t n = (size_t)(e - p);
        if (n >= max_len) n = max_len - 1;
        memcpy(out, p, n); out[n] = '\0';
        return true;
    }
}

static bool verify_auth(const char *method, const char *headers, const char *body) {
    if (!g_auth_pass[0]) return true; /* no password configured */

    /* 1. WS-Security UsernameToken in SOAP Header */
    if (body && body[0]) {
        char user[64] = "", pass[128] = "", nonce_b64[128] = "", created[64] = "";
        if (onvif_xml_get_tag(body, "Username", user, sizeof(user)) &&
            onvif_xml_get_tag(body, "Password", pass, sizeof(pass))) {
            if (!username_ok(user)) return false;
            /* Check PasswordText first */
            if (strcmp(pass, g_auth_pass) == 0) return true;
            /* Check PasswordDigest = Base64(SHA1(Base64Decode(Nonce) || Created || Password)) */
            if (onvif_xml_get_tag(body, "Nonce", nonce_b64, sizeof(nonce_b64)) &&
                onvif_xml_get_tag(body, "Created", created, sizeof(created))) {
                uint8_t raw_nonce[128];
                int nlen = b64_decode(nonce_b64, raw_nonce, sizeof(raw_nonce));
                size_t clen = strlen(created), plen = strlen(g_auth_pass);
                uint8_t msg[512];
                if (nlen > 0 && (size_t)nlen + clen + plen < sizeof(msg)) {
                    memcpy(msg, raw_nonce, (size_t)nlen);
                    memcpy(msg + nlen, created, clen);
                    memcpy(msg + nlen + clen, g_auth_pass, plen);
                    uint8_t sha[20];
                    sha1_compute(msg, (size_t)nlen + clen + plen, sha);
                    char expected_b64[64];
                    b64_encode(sha, 20, expected_b64, sizeof(expected_b64));
                    if (strcmp(pass, expected_b64) == 0) return true;
                }
            }
            return false;
        }
    }

    /* 2. HTTP Authorization header (Basic or Digest) */
    const char *auth = ci_strstr(headers, "\nAuthorization:");
    if (!auth) return false;
    auth += 15;
    while (*auth == ' ' || *auth == '\t') auth++;

    if (strncasecmp(auth, "Basic ", 6) == 0) {
        char b64[256] = "";
        const char *p = auth + 6;
        size_t i = 0;
        while (*p && *p != '\r' && *p != '\n' && i + 1 < sizeof(b64)) b64[i++] = *p++;
        b64[i] = '\0';
        uint8_t dec[256];
        int dlen = b64_decode(b64, dec, sizeof(dec) - 1);
        dec[dlen] = '\0';
        char *sep = strchr((char *)dec, ':');
        if (!sep) return false;
        *sep = '\0';
        return username_ok((char *)dec) && strcmp(sep + 1, g_auth_pass) == 0;
    }

    if (strncasecmp(auth, "Digest ", 7) == 0) {
        char line[1024] = "";
        const char *e = strpbrk(auth, "\r\n");
        size_t ll = e ? (size_t)(e - auth) : strlen(auth);
        if (ll >= sizeof(line)) ll = sizeof(line) - 1;
        memcpy(line, auth, ll); line[ll] = '\0';

        char user[64]="", realm[64]="", nonce[128]="", uri[256]="", resp[64]="", qop[32]="", nc[32]="", cnonce[128]="";
        if (!extract_digest_param(line, "username", user, sizeof(user))) return false;
        if (!username_ok(user)) return false;
        extract_digest_param(line, "realm", realm, sizeof(realm));
        extract_digest_param(line, "nonce", nonce, sizeof(nonce));
        extract_digest_param(line, "uri", uri, sizeof(uri));
        extract_digest_param(line, "response", resp, sizeof(resp));
        extract_digest_param(line, "qop", qop, sizeof(qop));
        extract_digest_param(line, "nc", nc, sizeof(nc));
        extract_digest_param(line, "cnonce", cnonce, sizeof(cnonce));

        char buf[512], ha1[33], ha2[33], expected[33];
        snprintf(buf, sizeof(buf), "%s:%s:%s", user, realm, g_auth_pass);
        md5_hex(buf, ha1);
        snprintf(buf, sizeof(buf), "%s:%s", method, uri);
        md5_hex(buf, ha2);
        if (qop[0]) {
            snprintf(buf, sizeof(buf), "%s:%s:%s:%s:%s:%s", ha1, nonce, nc, cnonce, qop, ha2);
        } else {
            snprintf(buf, sizeof(buf), "%s:%s:%s", ha1, nonce, ha2);
        }
        md5_hex(buf, expected);
        return strcasecmp(resp, expected) == 0;
    }

    return false;
}

/* ---- Night vision / Imaging helpers ---- */
static const char *nv_get_ircut_onvif(void) {
    /* ONVIF IrCutFilter: AUTO | ON (filter in = Day) | OFF (filter out = Night) */
    FILE *f = fopen("/tmp/night_vision", "r");
    if (!f) return "AUTO";
    char buf[32] = "";
    if (fgets(buf, sizeof(buf), f)) {
        if (strncasecmp(buf, "off", 3) == 0) { fclose(f); return "ON"; }
        if (strncasecmp(buf, "on", 2) == 0)  { fclose(f); return "OFF"; }
    }
    fclose(f);
    return "AUTO";
}

static void nv_set_ircut_onvif(const char *mode) {
    const char *val = "auto\n";
    if (strcasecmp(mode, "ON") == 0) val = "off\n";       /* IR-cut IN -> Day */
    else if (strcasecmp(mode, "OFF") == 0) val = "on\n";  /* IR-cut OUT -> Night */
    FILE *f = fopen("/tmp/night_vision", "w");
    if (f) { fputs(val, f); fclose(f); }
}

/* ---- HTTP Send Helpers ---- */
bool onvif_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static void send_soap_ok(int fd, const char *body_xml) {
    static char xml[24576];
    int xlen = snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tev=\"" EVENTS_NS "\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
        " xmlns:tmd=\"http://www.onvif.org/ver10/deviceIO/wsdl\""
        " xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\">\r\n"
        "<s:Body>%s</s:Body>\r\n"
        "</s:Envelope>", body_xml);
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/soap+xml; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", xlen);
    onvif_send_all(fd, hdr, (size_t)hlen);
    onvif_send_all(fd, xml, (size_t)xlen);
}

static void send_401(int fd) {
    char nonce_in[64], nonce[33];
    snprintf(nonce_in, sizeof(nonce_in), "nestcam-%ld-%d", (long)time(NULL), (int)getpid());
    md5_hex(nonce_in, nonce);

    const char *fault =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:ter=\"http://www.onvif.org/ver10/error\">\r\n"
        "<s:Body><s:Fault>"
        "<s:Code><s:Value>s:Sender</s:Value>"
        "<s:Subcode><s:Value>ter:NotAuthorized</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">Sender not Authorized</s:Text></s:Reason>"
        "</s:Fault></s:Body></s:Envelope>";
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Digest realm=\"NestCamOutdoor\", qop=\"auth\", nonce=\"%s\", opaque=\"nestcam\"\r\n"
        "WWW-Authenticate: Basic realm=\"NestCamOutdoor\"\r\n"
        "Content-Type: application/soap+xml; charset=utf-8\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n", nonce, (unsigned)strlen(fault));
    onvif_send_all(fd, hdr, (size_t)hlen);
    onvif_send_all(fd, fault, strlen(fault));
}

static void send_soap_unsupported(int fd, const char *action) {
    char xml[1024];
    int xlen = snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:ter=\"http://www.onvif.org/ver10/error\">\r\n"
        "<s:Body><s:Fault>"
        "<s:Code><s:Value>s:Receiver</s:Value>"
        "<s:Subcode><s:Value>ter:ActionNotSupported</s:Value></s:Subcode></s:Code>"
        "<s:Reason><s:Text xml:lang=\"en\">Action %s not supported</s:Text></s:Reason>"
        "</s:Fault></s:Body></s:Envelope>", action);
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: application/soap+xml; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", xlen);
    onvif_send_all(fd, hdr, (size_t)hlen);
    onvif_send_all(fd, xml, (size_t)xlen);
}

/* ---- Live JPEG Snapshot Endpoint (/snapshot.jpg) ---- */
static void handle_snapshot(int fd) {
    struct stat st;
    time_t now = time(NULL);
    bool stale = (stat("/tmp/snapshot.jpg", &st) != 0) || ((now - st.st_mtime) >= 2);
    if (stale) {
        FILE *pf = fopen("/tmp/nest_rtsp.pid", "r");
        if (pf) {
            int pid = 0;
            if (fscanf(pf, "%d", &pid) == 1 && pid > 1) {
                time_t old_mt = (stat("/tmp/snapshot.jpg", &st) == 0) ? st.st_mtime : 0;
                kill((pid_t)pid, SIGUSR1);
                for (int i = 0; i < 12; i++) {
                    usleep(20000);
                    if (stat("/tmp/snapshot.jpg", &st) == 0 && st.st_mtime > old_mt) break;
                }
            }
            fclose(pf);
        }
    }

    FILE *fp = fopen("/tmp/snapshot.jpg", "rb");
    if (!fp) {
        const char *nf = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        onvif_send_all(fd, nf, strlen(nf));
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 2000000) {
        fclose(fp);
        const char *nf = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        onvif_send_all(fd, nf, strlen(nf));
        return;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);

    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: image/jpeg\r\n"
        "Cache-Control: no-cache\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n\r\n", (unsigned)rd);
    onvif_send_all(fd, hdr, (size_t)hlen);
    onvif_send_all(fd, buf, rd);
    free(buf);
}

#define AUDIO_SOURCE_TOKEN "AudioSource_1"
#define AUDIO_OUTPUT_TOKEN "AudioOutput_1"
#define AUDIO_SOURCE_CONFIG_TOKEN "AudioSourceConfig_1"
#define AUDIO_ENCODER_TOKEN "AudioEncoderConfig_1"
#define AUDIO_OUTPUT_CONFIG_TOKEN "AudioOutputConfig_1"
#define AUDIO_DECODER_TOKEN "AudioDecoderConfig_1"

#define AUDIO_ENTITY(name) "<tt:Name>" name "</tt:Name><tt:UseCount>1</tt:UseCount>"
#define AUDIO_SOURCE_CONFIG \
    AUDIO_ENTITY("Microphone") \
    "<tt:SourceToken>" AUDIO_SOURCE_TOKEN "</tt:SourceToken>"
#define AUDIO_ENCODER_CONFIG \
    AUDIO_ENTITY("G.711 mu-law") \
    "<tt:Encoding>G711</tt:Encoding><tt:Bitrate>64</tt:Bitrate><tt:SampleRate>8</tt:SampleRate>" \
    "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address>" \
    "</tt:Address><tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast>" \
    "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
#define AUDIO_OUTPUT_CONFIG \
    AUDIO_ENTITY("Speaker") \
    "<tt:OutputToken>" AUDIO_OUTPUT_TOKEN "</tt:OutputToken><tt:OutputLevel>100</tt:OutputLevel>"
#define AUDIO_DECODER_CONFIG AUDIO_ENTITY("G.711 mu-law")

#define MEDIA_AUDIO_OPTIONS \
    "<tt:Options><tt:Encoding>G711</tt:Encoding>" \
    "<tt:BitrateList><tt:Items>64</tt:Items></tt:BitrateList>" \
    "<tt:SampleRateList><tt:Items>8</tt:Items></tt:SampleRateList></tt:Options>"
#define AUDIO_OUTPUT_OPTIONS \
    "<tt:OutputTokensAvailable>" AUDIO_OUTPUT_TOKEN "</tt:OutputTokensAvailable>" \
    "<tt:OutputLevelRange><tt:Min>100</tt:Min><tt:Max>100</tt:Max></tt:OutputLevelRange>"
#define AUDIO_DECODER_OPTIONS \
    "<tt:G711DecOptions><tt:Bitrate><tt:Items>64</tt:Items></tt:Bitrate>" \
    "<tt:SampleRateRange><tt:Items>8</tt:Items></tt:SampleRateRange></tt:G711DecOptions>"

static int audio_capabilities(void) {
    int caps = 0;
    FILE *status = fopen(NEST_AUDIO_STATUS, "r");
    if (status) {
        if (fscanf(status, "%d", &caps) != 1) caps = 0;
        fclose(status);
    }
    return caps & (AUDIO_CAPTURE | AUDIO_PLAYBACK);
}

static bool handle_audio_soap(int fd, const char *action, bool device_io, int caps) {
    static const struct {
        const char *type;
        const char *token;
        const char *config;
        const char *options;
        int direction;
    } configs[] = {
        {"Source", AUDIO_SOURCE_CONFIG_TOKEN, AUDIO_SOURCE_CONFIG,
         "<tt:InputTokensAvailable>" AUDIO_SOURCE_TOKEN "</tt:InputTokensAvailable>", AUDIO_CAPTURE},
        {"Encoder", AUDIO_ENCODER_TOKEN, AUDIO_ENCODER_CONFIG, MEDIA_AUDIO_OPTIONS, AUDIO_CAPTURE},
        {"Output", AUDIO_OUTPUT_CONFIG_TOKEN, AUDIO_OUTPUT_CONFIG, AUDIO_OUTPUT_OPTIONS, AUDIO_PLAYBACK},
        {"Decoder", AUDIO_DECODER_TOKEN, AUDIO_DECODER_CONFIG, AUDIO_DECODER_OPTIONS, AUDIO_PLAYBACK},
    };
    char response[2048];
    if (strcasecmp(action, "GetAudioSources") == 0) {
        if (device_io)
            snprintf(response, sizeof(response), "<tmd:GetAudioSourcesResponse>%s</tmd:GetAudioSourcesResponse>",
                     caps & AUDIO_CAPTURE ? "<tmd:Token>" AUDIO_SOURCE_TOKEN "</tmd:Token>" : "");
        else
            snprintf(response, sizeof(response), "<trt:GetAudioSourcesResponse>%s</trt:GetAudioSourcesResponse>",
                caps & AUDIO_CAPTURE ? "<trt:AudioSources token=\"" AUDIO_SOURCE_TOKEN
                "\"><tt:Channels>1</tt:Channels></trt:AudioSources>" : "");
    } else if (strcasecmp(action, "GetAudioOutputs") == 0) {
        if (device_io)
            snprintf(response, sizeof(response), "<tmd:GetAudioOutputsResponse>%s</tmd:GetAudioOutputsResponse>",
                     caps & AUDIO_PLAYBACK ? "<tmd:Token>" AUDIO_OUTPUT_TOKEN "</tmd:Token>" : "");
        else
            snprintf(response, sizeof(response), "<trt:GetAudioOutputsResponse>%s</trt:GetAudioOutputsResponse>",
                     caps & AUDIO_PLAYBACK ? "<trt:AudioOutputs token=\"" AUDIO_OUTPUT_TOKEN "\"/>" : "");
    } else {
        if (device_io) return false;
        for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
            char name[64];
            const char *type = configs[i].type;
            bool available = (caps & configs[i].direction) != 0;
            snprintf(name, sizeof(name), "GetAudio%sConfigurationOptions", type);
            if (strcasecmp(action, name) == 0) {
                if (!available) return false;
                snprintf(response, sizeof(response), "<trt:%sResponse><trt:Options>%s</trt:Options></trt:%sResponse>",
                         name, configs[i].options, name);
                send_soap_ok(fd, response);
                return true;
            }
            static const char *operations[] = {"GetAudio%sConfiguration", "GetAudio%sConfigurations",
                                               "GetCompatibleAudio%sConfigurations"};
            for (size_t op = 0; op < sizeof(operations) / sizeof(operations[0]); op++) {
                snprintf(name, sizeof(name), operations[op], type);
                if (strcasecmp(action, name) != 0) continue;
                if (available) {
                    const char *tag = op == 0 ? "Configuration" : "Configurations";
                    snprintf(response, sizeof(response),
                             "<trt:%sResponse><trt:%s token=\"%s\">%s</trt:%s></trt:%sResponse>",
                             name, tag, configs[i].token, configs[i].config, tag, name);
                } else if (op != 0) {
                    snprintf(response, sizeof(response), "<trt:%sResponse/>", name);
                } else {
                    return false;
                }
                send_soap_ok(fd, response);
                return true;
            }
        }
        return false;
    }
    send_soap_ok(fd, response);
    return true;
}

/* ---- SOAP Action Dispatcher ---- */
/* Return true when the event loop owns the HTTP connection. */
static bool handle_soap(int fd, const char *path, const char *headers, const char *body,
                        const char *host_ip, const char *peer_ip, const net_info_t *ni) {
    char action[96] = "";
    xml_get_soap_action(body, action, sizeof(action));

    bool is_time = (strcasecmp(action, "GetSystemDateAndTime") == 0);
    bool authed = is_time || verify_auth("POST", headers, body);
    if (strcasecmp(action, "PullMessages") != 0 || !authed)
        fprintf(stderr, "[onvif] POST %s action=%s auth=%s peer=%s\n",
                path, action[0] ? action : "(none)", authed ? "OK" : "DENY", peer_ip);

    if (!authed) {
        send_401(fd);
        return false;
    }

    char base_url[96];
    if (g_http_port == 80) snprintf(base_url, sizeof(base_url), "http://%s", host_ip);
    else snprintf(base_url, sizeof(base_url), "http://%s:%d", host_ip, g_http_port);

    events_result_t event = events_request(fd, path, action, body, base_url);
    if (event != EVENTS_UNHANDLED) return event == EVENTS_PENDING;

    static char resp[20480];
    int audio = audio_capabilities();
    int audio_in = (audio & AUDIO_CAPTURE) != 0;
    int audio_out = (audio & AUDIO_PLAYBACK) != 0;
    bool device_io = strcmp(path, "/onvif/deviceio_service") == 0;

    if (handle_audio_soap(fd, action, device_io, audio)) return false;

    if (strcasecmp(action, "GetSystemDateAndTime") == 0) {
        time_t now = time(NULL);
        struct tm utc;
        gmtime_r(&now, &utc);
        snprintf(resp, sizeof(resp),
            "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>"
            "<tt:DateTimeType>NTP</tt:DateTimeType>"
            "<tt:DaylightSavings>false</tt:DaylightSavings>"
            "<tt:TimeZone><tt:TZ>UTC0</tt:TZ></tt:TimeZone>"
            "<tt:UTCDateTime>"
            "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
            "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
            "</tt:UTCDateTime>"
            "<tt:LocalDateTime>"
            "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d</tt:Second></tt:Time>"
            "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
            "</tt:LocalDateTime>"
            "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>",
            utc.tm_hour, utc.tm_min, utc.tm_sec, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
            utc.tm_hour, utc.tm_min, utc.tm_sec, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetDeviceInformation") == 0) {
        char serial[16];
        for (int i = 0; i < 12; i++) serial[i] = (char)toupper((unsigned char)ni->mac_hex[i]);
        serial[12] = '\0';
        snprintf(resp, sizeof(resp),
            "<tds:GetDeviceInformationResponse>"
            "<tds:Manufacturer>Nest</tds:Manufacturer>"
            "<tds:Model>NestCamOutdoor</tds:Model>"
            "<tds:FirmwareVersion>1.0.0-local</tds:FirmwareVersion>"
            "<tds:SerialNumber>%s</tds:SerialNumber>"
            "<tds:HardwareId>A5S-HW5</tds:HardwareId>"
            "</tds:GetDeviceInformationResponse>", serial);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetCapabilities") == 0) {
        char device_io_xml[512] = "";
        if (audio)
            snprintf(device_io_xml, sizeof(device_io_xml),
                     "<tt:Extension><tt:DeviceIO><tt:XAddr>%s/onvif/deviceio_service</tt:XAddr>"
                     "<tt:VideoSources>1</tt:VideoSources><tt:VideoOutputs>0</tt:VideoOutputs>"
                     "<tt:AudioSources>%d</tt:AudioSources><tt:AudioOutputs>%d</tt:AudioOutputs>"
                     "<tt:RelayOutputs>0</tt:RelayOutputs></tt:DeviceIO></tt:Extension>",
                     base_url, audio_in, audio_out);
        snprintf(resp, sizeof(resp),
            "<tds:GetCapabilitiesResponse><tds:Capabilities>"
            "<tt:Device>"
              "<tt:XAddr>%s/onvif/device_service</tt:XAddr>"
              "<tt:Network>"
                "<tt:IPFilter>false</tt:IPFilter><tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
                "<tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS>"
              "</tt:Network>"
              "<tt:System>"
                "<tt:DiscoveryResolve>false</tt:DiscoveryResolve>"
                "<tt:DiscoveryBye>false</tt:DiscoveryBye>"
                "<tt:RemoteDiscovery>false</tt:RemoteDiscovery>"
                "<tt:SystemBackup>false</tt:SystemBackup>"
                "<tt:SystemLogging>false</tt:SystemLogging>"
                "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>"
                "<tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>50</tt:Minor></tt:SupportedVersions>"
              "</tt:System>"
              "<tt:IO><tt:InputConnectors>0</tt:InputConnectors><tt:RelayOutputs>0</tt:RelayOutputs></tt:IO>"
              "<tt:Security>"
                "<tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
                "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration>"
                "<tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
                "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken>"
                "<tt:KerberosToken>false</tt:KerberosToken><tt:RELToken>false</tt:RELToken>"
              "</tt:Security>"
            "</tt:Device>"
            "<tt:Events><tt:XAddr>%s" EVENTS_PATH "</tt:XAddr>"
              "<tt:WSSubscriptionPolicySupport>false</tt:WSSubscriptionPolicySupport>"
              "<tt:WSPullPointSupport>true</tt:WSPullPointSupport>"
              "<tt:WSPausableSubscriptionManagerInterfaceSupport>false</tt:WSPausableSubscriptionManagerInterfaceSupport>"
            "</tt:Events>"
            "<tt:Imaging><tt:XAddr>%s/onvif/imaging_service</tt:XAddr></tt:Imaging>"
            "<tt:Media>"
              "<tt:XAddr>%s/onvif/media_service</tt:XAddr>"
              "<tt:StreamingCapabilities>"
                "<tt:RTPMulticast>false</tt:RTPMulticast>"
                "<tt:RTP_TCP>true</tt:RTP_TCP>"
                "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
              "</tt:StreamingCapabilities>"
            "</tt:Media>%s"
            "</tds:Capabilities></tds:GetCapabilitiesResponse>",
            base_url, base_url, base_url, base_url, device_io_xml);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetServices") == 0) {
        char inc[16] = "";
        bool with_cap = onvif_xml_get_tag(body, "IncludeCapability", inc, sizeof(inc)) &&
                        (strcasecmp(inc, "true") == 0 || strcmp(inc, "1") == 0);
        char device_io_xml[512] = "";
        if (audio) {
            char caps_xml[160] = "";
            if (with_cap)
                snprintf(caps_xml, sizeof(caps_xml),
                         "<tds:Capabilities><tmd:Capabilities VideoSources=\"1\" AudioSources=\"%d\" AudioOutputs=\"%d\"/></tds:Capabilities>",
                         audio_in, audio_out);
            snprintf(device_io_xml, sizeof(device_io_xml),
                     "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/deviceIO/wsdl</tds:Namespace>"
                     "<tds:XAddr>%s/onvif/deviceio_service</tds:XAddr>%s"
                     "<tds:Version><tt:Major>2</tt:Major><tt:Minor>50</tt:Minor></tds:Version></tds:Service>",
                     base_url, caps_xml);
        }
        snprintf(resp, sizeof(resp),
            "<tds:GetServicesResponse>"
            "<tds:Service>"
              "<tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>"
              "<tds:XAddr>%s/onvif/device_service</tds:XAddr>"
              "%s"
              "<tds:Version><tt:Major>2</tt:Major><tt:Minor>50</tt:Minor></tds:Version>"
            "</tds:Service>"
            "<tds:Service>"
              "<tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>"
              "<tds:XAddr>%s/onvif/media_service</tds:XAddr>"
              "%s"
              "<tds:Version><tt:Major>2</tt:Major><tt:Minor>50</tt:Minor></tds:Version>"
            "</tds:Service>"
            "<tds:Service>"
              "<tds:Namespace>" EVENTS_NS "</tds:Namespace>"
              "<tds:XAddr>%s" EVENTS_PATH "</tds:XAddr>"
              "%s"
              "<tds:Version><tt:Major>2</tt:Major><tt:Minor>50</tt:Minor></tds:Version>"
            "</tds:Service>"
            "<tds:Service>"
              "<tds:Namespace>http://www.onvif.org/ver20/imaging/wsdl</tds:Namespace>"
              "<tds:XAddr>%s/onvif/imaging_service</tds:XAddr>"
              "%s"
              "<tds:Version><tt:Major>2</tt:Major><tt:Minor>50</tt:Minor></tds:Version>"
            "</tds:Service>"
            "%s</tds:GetServicesResponse>",
            base_url,
            with_cap ? "<tds:Capabilities><tds:Capabilities>"
                       "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/>"
                       "<tds:Security HttpDigest=\"true\" UsernameToken=\"true\"/>"
                       "<tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"false\" RemoteDiscovery=\"false\"/>"
                       "</tds:Capabilities></tds:Capabilities>" : "",
            base_url,
            with_cap ? "<tds:Capabilities><trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">"
                       "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>"
                       "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\"/>"
                       "</trt:Capabilities></tds:Capabilities>" : "",
            base_url, with_cap ? "<tds:Capabilities>" EVENT_CAPABILITIES "</tds:Capabilities>" : "",
            base_url,
            with_cap ? "<tds:Capabilities><timg:Capabilities ImageStabilization=\"false\"/></tds:Capabilities>" : "", device_io_xml);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetServiceCapabilities") == 0) {
        if (device_io) {
            snprintf(resp, sizeof(resp),
                "<tmd:GetServiceCapabilitiesResponse>"
                "<tmd:Capabilities VideoSources=\"1\" AudioSources=\"%d\" AudioOutputs=\"%d\"/>"
                "</tmd:GetServiceCapabilitiesResponse>", audio_in, audio_out);
        } else if (ci_strstr(path, "media")) {
            snprintf(resp, sizeof(resp),
                "<trt:GetServiceCapabilitiesResponse>"
                "<trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" VideoSourceMode=\"false\" OSD=\"false\">"
                "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"1\"/>"
                "<trt:StreamingCapabilities RTPMulticast=\"false\" RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\"/>"
                "</trt:Capabilities></trt:GetServiceCapabilitiesResponse>");
        } else if (ci_strstr(path, "imaging")) {
            snprintf(resp, sizeof(resp),
                "<timg:GetServiceCapabilitiesResponse>"
                "<timg:Capabilities ImageStabilization=\"false\"/>"
                "</timg:GetServiceCapabilitiesResponse>");
        } else {
            snprintf(resp, sizeof(resp),
                "<tds:GetServiceCapabilitiesResponse><tds:Capabilities>"
                "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" IPVersion6=\"false\" DynDNS=\"false\"/>"
                "<tds:Security HttpDigest=\"true\" UsernameToken=\"true\"/>"
                "<tds:System DiscoveryResolve=\"false\" DiscoveryBye=\"false\" RemoteDiscovery=\"false\"/>"
                "</tds:Capabilities></tds:GetServiceCapabilitiesResponse>");
        }
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetNetworkInterfaces") == 0) {
        snprintf(resp, sizeof(resp),
            "<tds:GetNetworkInterfacesResponse>"
            "<tds:NetworkInterfaces token=\"%s\">"
              "<tt:Enabled>true</tt:Enabled>"
              "<tt:Info>"
                "<tt:Name>%s</tt:Name>"
                "<tt:HwAddress>%s</tt:HwAddress>"
                "<tt:MTU>1500</tt:MTU>"
              "</tt:Info>"
              "<tt:IPv4>"
                "<tt:Enabled>true</tt:Enabled>"
                "<tt:Config>"
                  "<tt:FromDHCP>"
                    "<tt:Address>%s</tt:Address>"
                    "<tt:PrefixLength>%d</tt:PrefixLength>"
                  "</tt:FromDHCP>"
                  "<tt:DHCP>true</tt:DHCP>"
                "</tt:Config>"
              "</tt:IPv4>"
            "</tds:NetworkInterfaces>"
            "</tds:GetNetworkInterfacesResponse>",
            g_iface, g_iface, ni->mac_colon, ni->ip, ni->prefix_len);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetNetworkProtocols") == 0) {
        snprintf(resp, sizeof(resp),
            "<tds:GetNetworkProtocolsResponse>"
            "<tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port></tds:NetworkProtocols>"
            "<tds:NetworkProtocols><tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port></tds:NetworkProtocols>"
            "</tds:GetNetworkProtocolsResponse>", g_http_port, g_rtsp_port);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetHostname") == 0) {
        snprintf(resp, sizeof(resp),
            "<tds:GetHostnameResponse><tds:HostnameInformation>"
            "<tt:FromDHCP>false</tt:FromDHCP><tt:Name>NestCamOutdoor</tt:Name>"
            "</tds:HostnameInformation></tds:GetHostnameResponse>");
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetScopes") == 0) {
        snprintf(resp, sizeof(resp),
            "<tds:GetScopesResponse>"
            "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/type/video_encoder</tt:ScopeItem></tds:Scopes>"
            "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/Profile/Streaming</tt:ScopeItem></tds:Scopes>"
            "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/hardware/NestCamOutdoor</tt:ScopeItem></tds:Scopes>"
            "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/name/NestCamOutdoor</tt:ScopeItem></tds:Scopes>"
            "<tds:Scopes><tt:ScopeDef>Configurable</tt:ScopeDef><tt:ScopeItem>onvif://www.onvif.org/location/local</tt:ScopeItem></tds:Scopes>"
            "</tds:GetScopesResponse>");
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetDiscoveryMode") == 0) {
        send_soap_ok(fd, "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode></tds:GetDiscoveryModeResponse>");
    }
    else if (strcasecmp(action, "GetDNS") == 0) {
        send_soap_ok(fd, "<tds:GetDNSResponse><tds:DNSInformation><tt:FromDHCP>true</tt:FromDHCP></tds:DNSInformation></tds:GetDNSResponse>");
    }
    else if (strcasecmp(action, "GetNTP") == 0) {
        send_soap_ok(fd, "<tds:GetNTPResponse><tds:NTPInformation><tt:FromDHCP>true</tt:FromDHCP></tds:NTPInformation></tds:GetNTPResponse>");
    }
    else if (strcasecmp(action, "GetUsers") == 0) {
        snprintf(resp, sizeof(resp),
            "<tds:GetUsersResponse><tds:User>"
            "<tt:Username>%s</tt:Username><tt:UserLevel>Administrator</tt:UserLevel>"
            "</tds:User></tds:GetUsersResponse>", g_auth_user);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetProfiles") == 0 || strcasecmp(action, "GetProfile") == 0) {
        const char *tag = (strcasecmp(action, "GetProfile") == 0) ? "Profile" : "Profiles";
        snprintf(resp, sizeof(resp),
            "<trt:%sResponse>"
            "<trt:%s token=\"Profile_1\" fixed=\"true\">"
              "<tt:Name>MainStream</tt:Name>"
              "<tt:VideoSourceConfiguration token=\"" MOTION_SOURCE_TOKEN "\">"
                "<tt:Name>VideoSourceConfig_1</tt:Name>"
                "<tt:UseCount>1</tt:UseCount>"
                "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
                "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/>"
              "</tt:VideoSourceConfiguration>"
              "%s<tt:VideoEncoderConfiguration token=\"VideoEncoderConfig_1\">"
                "<tt:Name>VideoEncoderConfig_1</tt:Name>"
                "<tt:UseCount>1</tt:UseCount>"
                "<tt:Encoding>H264</tt:Encoding>"
                "<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
                "<tt:Quality>5</tt:Quality>"
                "<tt:RateControl>"
                  "<tt:FrameRateLimit>30</tt:FrameRateLimit>"
                  "<tt:EncodingInterval>1</tt:EncodingInterval>"
                  "<tt:BitrateLimit>3000</tt:BitrateLimit>"
                "</tt:RateControl>"
                "<tt:H264><tt:GovLength>30</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>"
                "<tt:Multicast>"
                  "<tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>"
                  "<tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart>"
                "</tt:Multicast>"
                "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
              "</tt:VideoEncoderConfiguration>"
              "%s"
              "<tt:VideoAnalyticsConfiguration token=\"" MOTION_ANALYTICS_TOKEN "\">"
                "<tt:Name>Motion Detection</tt:Name><tt:UseCount>1</tt:UseCount>"
                "<tt:AnalyticsEngineConfiguration/><tt:RuleEngineConfiguration>"
                  "<tt:Rule Name=\"" MOTION_RULE "\" Type=\"tt:CellMotionDetector\"><tt:Parameters/></tt:Rule>"
                "</tt:RuleEngineConfiguration>"
              "</tt:VideoAnalyticsConfiguration>"
              "%s"
            "</trt:%s>"
            "</trt:%sResponse>", action, tag,
            audio_in ? "<tt:AudioSourceConfiguration token=\"" AUDIO_SOURCE_CONFIG_TOKEN "\">"
                AUDIO_SOURCE_CONFIG "</tt:AudioSourceConfiguration>" : "",
            audio_in ? "<tt:AudioEncoderConfiguration token=\"" AUDIO_ENCODER_TOKEN "\">"
                AUDIO_ENCODER_CONFIG "</tt:AudioEncoderConfiguration>" : "",
            audio_out ? "<tt:Extension><tt:AudioOutputConfiguration token=\"" AUDIO_OUTPUT_CONFIG_TOKEN "\">"
                AUDIO_OUTPUT_CONFIG "</tt:AudioOutputConfiguration>"
                "<tt:AudioDecoderConfiguration token=\"" AUDIO_DECODER_TOKEN "\">"
                AUDIO_DECODER_CONFIG "</tt:AudioDecoderConfiguration></tt:Extension>" : "",
            tag, action);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetVideoSources") == 0) {
        snprintf(resp, sizeof(resp),
            "<trt:GetVideoSourcesResponse>"
            "<trt:VideoSources token=\"VideoSource_1\">"
              "<tt:Framerate>30</tt:Framerate>"
              "<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
              "<tt:Imaging><tt:IrCutFilter>%s</tt:IrCutFilter></tt:Imaging>"
            "</trt:VideoSources>"
            "</trt:GetVideoSourcesResponse>", nv_get_ircut_onvif());
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetVideoSourceConfigurations") == 0 ||
             strcasecmp(action, "GetVideoSourceConfiguration") == 0) {
        const char *tag = (strcasecmp(action, "GetVideoSourceConfiguration") == 0) ? "Configuration" : "Configurations";
        snprintf(resp, sizeof(resp),
            "<trt:%sResponse>"
            "<trt:%s token=\"VideoSourceConfig_1\">"
              "<tt:Name>VideoSourceConfig_1</tt:Name>"
              "<tt:UseCount>1</tt:UseCount>"
              "<tt:SourceToken>VideoSource_1</tt:SourceToken>"
              "<tt:Bounds x=\"0\" y=\"0\" width=\"1920\" height=\"1080\"/>"
            "</trt:%s>"
            "</trt:%sResponse>", action, tag, tag, action);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetVideoEncoderConfigurations") == 0 ||
             strcasecmp(action, "GetVideoEncoderConfiguration") == 0) {
        const char *tag = (strcasecmp(action, "GetVideoEncoderConfiguration") == 0) ? "Configuration" : "Configurations";
        snprintf(resp, sizeof(resp),
            "<trt:%sResponse>"
            "<trt:%s token=\"VideoEncoderConfig_1\">"
              "<tt:Name>VideoEncoderConfig_1</tt:Name>"
              "<tt:UseCount>1</tt:UseCount>"
              "<tt:Encoding>H264</tt:Encoding>"
              "<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:Resolution>"
              "<tt:Quality>5</tt:Quality>"
              "<tt:RateControl>"
                "<tt:FrameRateLimit>30</tt:FrameRateLimit>"
                "<tt:EncodingInterval>1</tt:EncodingInterval>"
                "<tt:BitrateLimit>3000</tt:BitrateLimit>"
              "</tt:RateControl>"
              "<tt:H264><tt:GovLength>30</tt:GovLength><tt:H264Profile>Main</tt:H264Profile></tt:H264>"
              "<tt:Multicast>"
                "<tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address>"
                "<tt:Port>0</tt:Port><tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart>"
              "</tt:Multicast>"
              "<tt:SessionTimeout>PT60S</tt:SessionTimeout>"
            "</trt:%s>"
            "</trt:%sResponse>", action, tag, tag, action);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetVideoEncoderConfigurationOptions") == 0) {
        send_soap_ok(fd,
            "<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options>"
            "<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>10</tt:Max></tt:QualityRange>"
            "<tt:H264>"
              "<tt:ResolutionsAvailable><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height></tt:ResolutionsAvailable>"
              "<tt:GovLengthRange><tt:Min>30</tt:Min><tt:Max>30</tt:Max></tt:GovLengthRange>"
              "<tt:FrameRateRange><tt:Min>30</tt:Min><tt:Max>30</tt:Max></tt:FrameRateRange>"
              "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>"
              "<tt:H264ProfilesSupported>Main</tt:H264ProfilesSupported>"
            "</tt:H264>"
            "</trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>");
    }
    else if (strcasecmp(action, "SetVideoEncoderConfiguration") == 0) {
        send_soap_ok(fd, "<trt:SetVideoEncoderConfigurationResponse/>");
    }
    else if (strcasecmp(action, "GetStreamUri") == 0) {
        snprintf(resp, sizeof(resp),
            "<trt:GetStreamUriResponse><trt:MediaUri>"
            "<tt:Uri>rtsp://%s:%d/stream1</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT60S</tt:Timeout>"
            "</trt:MediaUri></trt:GetStreamUriResponse>",
            host_ip, g_rtsp_port);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "GetSnapshotUri") == 0) {
        snprintf(resp, sizeof(resp),
            "<trt:GetSnapshotUriResponse><trt:MediaUri>"
            "<tt:Uri>%s/snapshot.jpg</tt:Uri>"
            "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
            "<tt:Timeout>PT60S</tt:Timeout>"
            "</trt:MediaUri></trt:GetSnapshotUriResponse>",
            base_url);
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "SetSynchronizationPoint") == 0) {
        send_soap_ok(fd, "<trt:SetSynchronizationPointResponse/>");
    }
    else if (strcasecmp(action, "GetImagingSettings") == 0) {
        snprintf(resp, sizeof(resp),
            "<timg:GetImagingSettingsResponse><timg:ImagingSettings>"
            "<tt:IrCutFilter>%s</tt:IrCutFilter>"
            "</timg:ImagingSettings></timg:GetImagingSettingsResponse>",
            nv_get_ircut_onvif());
        send_soap_ok(fd, resp);
    }
    else if (strcasecmp(action, "SetImagingSettings") == 0) {
        char mode[16] = "";
        if (onvif_xml_get_tag(body, "IrCutFilter", mode, sizeof(mode))) {
            nv_set_ircut_onvif(mode);
            fprintf(stderr, "[onvif] SetImagingSettings IrCutFilter=%s\n", mode);
        }
        send_soap_ok(fd, "<timg:SetImagingSettingsResponse/>");
    }
    else if (strcasecmp(action, "GetOptions") == 0) {
        send_soap_ok(fd,
            "<timg:GetOptionsResponse><timg:ImagingOptions>"
            "<tt:IrCutFilterModes>ON</tt:IrCutFilterModes>"
            "<tt:IrCutFilterModes>OFF</tt:IrCutFilterModes>"
            "<tt:IrCutFilterModes>AUTO</tt:IrCutFilterModes>"
            "</timg:ImagingOptions></timg:GetOptionsResponse>");
    }
    else {
        send_soap_unsupported(fd, action[0] ? action : "Unknown");
    }
    return false;
}

/* ---- HTTP Client Connection Handler ---- */
static void handle_http_client(int cfd, const struct in_addr *peer) {
    struct timeval tv = { 3, 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    static char req[16384];
    size_t total = 0;
    char *hdr_end = NULL;
    size_t content_len = 0;

    while (total + 1 < sizeof(req)) {
        ssize_t n = recv(cfd, req + total, sizeof(req) - 1 - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        req[total] = '\0';
        if (!hdr_end) {
            hdr_end = strstr(req, "\r\n\r\n");
            if (hdr_end) {
                const char *cl = ci_strstr(req, "Content-Length:");
                if (cl && cl < hdr_end) content_len = (size_t)strtoul(cl + 15, NULL, 10);
                if (ci_strstr(req, "100-continue") && ci_strstr(req, "100-continue") < hdr_end) {
                    const char *c100 = "HTTP/1.1 100 Continue\r\n\r\n";
                    onvif_send_all(cfd, c100, strlen(c100));
                }
            }
        }
        if (hdr_end) {
            size_t body_got = total - (size_t)((hdr_end + 4) - req);
            if (body_got >= content_len) break;
        }
    }
    if (total == 0 || !hdr_end) { close(cfd); return; }

    *hdr_end = '\0';
    const char *body = hdr_end + 4;

    char method[16] = "", path[256] = "";
    sscanf(req, "%15s %255s", method, path);

    net_info_t ni;
    get_net_info(g_iface, &ni);

    char host_ip[32];
    strncpy(host_ip, ni.ip, sizeof(host_ip) - 1);
    host_ip[sizeof(host_ip) - 1] = '\0';
    struct sockaddr_in loc;
    socklen_t llen = sizeof(loc);
    if (getsockname(cfd, (struct sockaddr *)&loc, &llen) == 0) {
        inet_ntop(AF_INET, &loc.sin_addr, host_ip, sizeof(host_ip));
    }

    if (strcasecmp(method, "GET") == 0 && ci_strstr(path, "snapshot")) {
        if (!verify_auth("GET", req, "")) {
            send_401(cfd);
        } else {
            handle_snapshot(cfd);
        }
    } else if (strcasecmp(method, "POST") == 0) {
        char peer_ip[INET_ADDRSTRLEN] = "";
        inet_ntop(AF_INET, peer, peer_ip, sizeof(peer_ip));
        if (handle_soap(cfd, path, req, body, host_ip, peer_ip, &ni)) return;
    } else {
        const char *ok = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 21\r\nConnection: close\r\n\r\nNestCamOutdoor ONVIF\n";
        onvif_send_all(cfd, ok, strlen(ok));
    }
    close(cfd);
}

/* ---- WS-Discovery (UDP 239.255.255.250:3702) ---- */
static int open_wsd_socket(const char *ifname) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(3702);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[onvif] bind UDP 3702");
        close(fd);
        return -1;
    }

    net_info_t ni;
    get_net_info(ifname, &ni);
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr("239.255.255.250");
    mreq.imr_interface.s_addr = inet_addr(ni.ip);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    }
    return fd;
}

static void handle_wsd_packet(int wfd) {
    char buf[4096];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    ssize_t n = recvfrom(wfd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&peer, &plen);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Only answer WS-Discovery Probe packets */
    if (!ci_strstr(buf, "Probe") || ci_strstr(buf, "ProbeMatches")) return;

    char msg_id[128] = "uuid:00000000-0000-0000-0000-000000000000";
    onvif_xml_get_tag(buf, "MessageID", msg_id, sizeof(msg_id));

    net_info_t ni;
    get_net_info(g_iface, &ni);

    char xaddr[128];
    if (g_http_port == 80) snprintf(xaddr, sizeof(xaddr), "http://%s/onvif/device_service", ni.ip);
    else snprintf(xaddr, sizeof(xaddr), "http://%s:%d/onvif/device_service", ni.ip, g_http_port);

    char reply[2048];
    int rlen = snprintf(reply, sizeof(reply),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
        " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
        " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">\r\n"
        "<s:Header>"
          "<wsa:MessageID>urn:uuid:%s-%lx</wsa:MessageID>"
          "<wsa:RelatesTo>%s</wsa:RelatesTo>"
          "<wsa:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:To>"
          "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</wsa:Action>"
        "</s:Header>"
        "<s:Body><d:ProbeMatches><d:ProbeMatch>"
          "<wsa:EndpointReference><wsa:Address>urn:uuid:%s</wsa:Address></wsa:EndpointReference>"
          "<d:Types>dn:NetworkVideoTransmitter tds:Device</d:Types>"
          "<d:Scopes>onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/Profile/Streaming"
          " onvif://www.onvif.org/hardware/NestCamOutdoor onvif://www.onvif.org/name/NestCamOutdoor"
          " onvif://www.onvif.org/location/local</d:Scopes>"
          "<d:XAddrs>%s</d:XAddrs>"
          "<d:MetadataVersion>1</d:MetadataVersion>"
        "</d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>",
        ni.uuid, (unsigned long)time(NULL), msg_id, ni.uuid, xaddr);

    sendto(wfd, reply, (size_t)rlen, 0, (struct sockaddr *)&peer, plen);
    fprintf(stderr, "[onvif] WSD ProbeMatch -> %s:%u (%s)\n",
            inet_ntoa(peer.sin_addr), (unsigned)ntohs(peer.sin_port), xaddr);
}

int main(int argc, char **argv) {
    const char *conf_path = "/mnt/dropcam/onvif.conf";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) g_http_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) g_rtsp_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) strncpy(g_iface, argv[++i], sizeof(g_iface) - 1);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) conf_path = argv[++i];
    }

    setlinebuf(stderr);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    load_credentials(conf_path);

    int hfd = socket(AF_INET, SOCK_STREAM, 0);
    if (hfd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(hfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_http_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(hfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(hfd, 8) < 0) {
        perror("[onvif] bind/listen HTTP");
        return 1;
    }

    int wfd = open_wsd_socket(g_iface);

    net_info_t ni;
    get_net_info(g_iface, &ni);
    fprintf(stderr, "[onvif] listening HTTP:%d WSD:%s iface=%s ip=%s mac=%s user=%s auth=%s\n",
            g_http_port, (wfd >= 0) ? "3702" : "off", g_iface, ni.ip, ni.mac_colon,
            g_auth_user, g_auth_pass[0] ? "enabled" : "open");

    while (g_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(hfd, &rfds);
        int maxfd = hfd;
        if (wfd >= 0) {
            FD_SET(wfd, &rfds);
            if (wfd > maxfd) maxfd = wfd;
        }
        struct timeval tv;
        events_poll(&rfds, &maxfd, &tv);
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        events_readable(&rfds);
        if (wfd >= 0 && FD_ISSET(wfd, &rfds)) {
            handle_wsd_packet(wfd);
        }
        if (FD_ISSET(hfd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof(cli);
            int cfd = accept(hfd, (struct sockaddr *)&cli, &clen);
            if (cfd >= 0) {
                handle_http_client(cfd, &cli.sin_addr);
            }
        }
    }

    events_close();
    if (wfd >= 0) close(wfd);
    close(hfd);
    return 0;
}
