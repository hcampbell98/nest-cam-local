/*
 * nest_rtsp - Lightweight local Ambarella A5s H.264 RTSP server
 * Target: Gen 1 Nest Cam Outdoor (hwver=5, AR0330/MT9T002, Linux 2.6.38.8)
 *
 * Single-threaded design to avoid the firmware 217/7800002
 * libpthread futex_wait kernel oops.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

static const char *ci_strstr(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    if (!nlen) return haystack;
    for (; *haystack; haystack++) {
        size_t i = 0;
        while (i < nlen && tolower((unsigned char)haystack[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen) return haystack;
    }
    return NULL;
}
#define strcasestr ci_strstr
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#ifndef TIOCOUTQ
#define TIOCOUTQ 0x5411
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/times.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include "nv12_jpeg.h"
#include "nest_audio.h"
#include "nest_motion.h"

/* ---- Ambarella A5s IAV IOCTLs (verified against NestCamOutdoor iav.ko) ---- */
#define IAV_IOC_GET_STATE                       0x80047652
#define IAV_IOC_ENTER_IDLE                      0x40047654
#define IAV_IOC_MAP_DSP                         0x80047612
#define IAV_IOC_MAP_BSB2                        0x80047614
#define IAV_IOC_GET_SYSTEM_RESOURCE_LIMIT_EX    0x80047674
#define IAV_IOC_SET_SYSTEM_RESOURCE_LIMIT_EX    0x40047673

#define IAV_IOC_VIN_GET_SOURCE_NUM              0x80046901
#define IAV_IOC_VIN_SET_CURRENT_SRC             0x40046902
#define IAV_IOC_VIN_SRC_SET_VIDEO_MODE          0x40047303
#define IAV_IOC_VIN_SRC_SET_FRAME_RATE          0x40047304
#define IAV_IOC_VIN_SRC_SET_SHUTTER_TIME        0x40047307
#define IAV_IOC_VIN_SRC_SET_AGC_DB              0x40047308
#define IAV_IOC_VIN_SRC_SET_MIRROR_MODE         0x4004730b

#define IAV_IOC_ENABLE_PREVIEW                  0x0000650b
#define IAV_IOC_DISABLE_PREVIEW                 0x0000650c
#define IAV_IOC_SET_SOURCE_BUFFER_TYPE_ALL_EX   0x40046520
#define IAV_IOC_GET_SOURCE_BUFFER_FORMAT_ALL_EX 0x80046525
#define IAV_IOC_SET_SOURCE_BUFFER_FORMAT_ALL_EX 0x40046524
#define IAV_IOC_SET_ENCODE_FORMAT_EX            0x40046528
#define IAV_IOC_GET_ENCODE_FORMAT_EX            0xc0046529
#define IAV_IOC_READ_YUV_BUFFER_INFO_EX         0xc0046530
#define IAV_IOC_READ_BITSTREAM_EX               0x80046537
#define IAV_IOC_SET_BITRATE_EX                  0x40046538
#define IAV_IOC_GET_BITRATE_EX                  0xc0046539
#define IAV_IOC_FORCE_IDR_EX                    0x4004653b
#define IAV_IOC_START_ENCODE_EX                 0x40046540
#define IAV_IOC_STOP_ENCODE_EX                  0x40046541
#define IAV_IOC_SET_AUDIO_CLK_FREQ_EX           0x40046549
#define IAV_IOC_IMG_BLACK_LEVEL_GLOBAL_OFFSET   0x40047003
#define IAV_IOC_IMG_RGB_GAIN_ADJUST             0x40047007
#define IAV_IOC_IMG_COLOR_CORRECTION            0x4004700a
#define IAV_IOC_IMG_RGB_TO_YUV_SETUP            0x4004700b

struct iav_blc_global_offset {
    int32_t  gr_offset;
    int32_t  r_offset;
    int32_t  b_offset;
    int32_t  gb_offset;
    int16_t  def_r;
    int16_t  def_g;
    int16_t  def_b;
    int16_t  pad;
};

struct iav_color_correction {
    uint8_t  enable;
    uint8_t  in_lookup_bypass;
    uint8_t  matrix_bypass;
    uint8_t  out_lookup_bypass;
    uint32_t r_gain;
    uint32_t g_gain;
    uint32_t b_gain;
    uint32_t reg_addr;
    uint32_t lut3d_addr;
    uint32_t no_tone_curve;
    uint32_t tone_curve_addr;
    uint32_t reserved;
};

struct iav_rgb_to_yuv {
    int16_t  matrix_values[9]; /* 3x3 Q10 (1024 = 1.0): Y, U, V rows */
    int16_t  y_offset;
    int16_t  u_offset;
    int16_t  v_offset;
    uint32_t reserved;
};

struct iav_mmap_info {
    uint8_t *addr;
    uint32_t length;
};

struct iav_bits_info_ex {
    uint32_t frame_num;
    uint32_t pts;
    uint32_t start_addr;
    uint32_t flags_size;
    uint8_t  more_frames;
    uint8_t  stream_id;
    uint8_t  is_cavlc;
    uint8_t  pad0;
    uint32_t session_id;
    uint32_t reserved[6];
    uint64_t mono_pts;
};

struct iav_yuv_info_ex {
    uint32_t source;
    uint32_t y_addr;
    uint32_t uv_addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t seq_num;
    uint32_t format;
};

/* ---- Globals ---- */
static volatile int g_running = 1;
static volatile int g_snap_req = 1; /* capture initial snapshot right away */
static void on_sig(int sig) { (void)sig; g_running = 0; }
static void on_usr1(int sig) { (void)sig; g_snap_req = 1; }

/* Cached SPS & PPS for SDP and late-joining clients */
static uint8_t g_sps[256];
static int     g_sps_len = 0;
static uint8_t g_pps[256];
static int     g_pps_len = 0;

/* Optional RTSP auth credentials loaded from /mnt/dropcam/onvif.conf (user:pass) */
static char    g_auth_user[64] = "admin";
static char    g_auth_pass[128] = "";
static char    g_rtsp_nonce[33] = "9f8e7d6c5b4a39281726354453627180";

static void load_auth_conf(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    if (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *sep = strchr(line, ':');
        if (sep) {
            *sep = '\0';
            if (line[0]) snprintf(g_auth_user, sizeof(g_auth_user), "%.63s", line);
            strncpy(g_auth_pass, sep + 1, sizeof(g_auth_pass) - 1);
        }
    }
    fclose(f);
}

static bool rtsp_user_ok(const char *u) {
    if (!u || !u[0]) return false;
    if (strcmp(u, g_auth_user) == 0) return true;
    if (strcmp(g_auth_user, "admin") == 0 && strcmp(u, "root") == 0) return true;
    return false;
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

static uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

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

static bool extract_digest_param(const char *hdr, const char *key, char *out, size_t max_len) {
    char pat[48];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strcasestr(hdr, pat);
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

static bool rtsp_verify_auth(const char *method, const char *url, const char *req) {
    if (!g_auth_pass[0]) return true;
    const char *auth = strcasestr(req, "\nAuthorization:");
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
        return rtsp_user_ok((char *)dec) && strcmp(sep + 1, g_auth_pass) == 0;
    }

    if (strncasecmp(auth, "Digest ", 7) == 0) {
        char line[1024] = "";
        const char *e = strpbrk(auth, "\r\n");
        size_t ll = e ? (size_t)(e - auth) : strlen(auth);
        if (ll >= sizeof(line)) ll = sizeof(line) - 1;
        memcpy(line, auth, ll); line[ll] = '\0';

        char user[64]="", realm[64]="", nonce[128]="", uri[256]="", resp[64]="", qop[32]="", nc[32]="", cnonce[128]="";
        if (!extract_digest_param(line, "username", user, sizeof(user))) return false;
        if (!rtsp_user_ok(user)) return false;
        extract_digest_param(line, "realm", realm, sizeof(realm));
        extract_digest_param(line, "nonce", nonce, sizeof(nonce));
        extract_digest_param(line, "uri", uri, sizeof(uri));
        extract_digest_param(line, "response", resp, sizeof(resp));
        extract_digest_param(line, "qop", qop, sizeof(qop));
        extract_digest_param(line, "nc", nc, sizeof(nc));
        extract_digest_param(line, "cnonce", cnonce, sizeof(cnonce));

        if (strcmp(realm, "NestCamOutdoor") != 0 ||
            strcmp(nonce, g_rtsp_nonce) != 0 || strcmp(uri, url) != 0 ||
            (qop[0] && strcmp(qop, "auth") != 0))
            return false;

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

/* ---- Base64 helper for SDP sprop-parameter-sets ---- */
static const char b64_tbl[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_encode(const uint8_t *in, int len, char *out, int out_max) {
    int i = 0, o = 0;
    while (i < len && o + 4 < out_max) {
        uint32_t v = ((uint32_t)in[i++]) << 16;
        int rem = len - i;
        if (rem >= 1) v |= ((uint32_t)in[i++]) << 8;
        if (rem >= 2) v |= ((uint32_t)in[i++]);
        out[o++] = b64_tbl[(v >> 18) & 0x3f];
        out[o++] = b64_tbl[(v >> 12) & 0x3f];
        out[o++] = (rem >= 1) ? b64_tbl[(v >> 6) & 0x3f] : '=';
        out[o++] = (rem >= 2) ? b64_tbl[v & 0x3f] : '=';
    }
    out[o] = '\0';
    return o;
}

/* ---- RTSP Client State ---- */
#define MAX_CLIENTS     8
#define RTP_MTU         1400
#define CLIENT_BUF_SIZE 4096
#define TCP_QUEUE_SIZE (256u * 1024u)
#define TCP_MEDIA_LIMIT (192u * 1024u)
#define TCP_RESUME_LIMIT (32u * 1024u)
#define TCP_STALL_MS 15000

enum { TRACK_VIDEO, TRACK_MIC, TRACK_SPEAKER, TRACK_COUNT };

#define BACKCHANNEL_TAG "www.onvif.org/ver20/backchannel"
#define TALK_BUFFER_SAMPLES (AUDIO_RTP_RATE / 5)
#define TALK_IDLE_MS 700

typedef struct {
    bool setup;
    bool use_tcp;
    uint8_t rtp_chan;
    uint8_t rtcp_chan;
    int rtp_fd;
    int rtcp_fd;
    struct sockaddr_in peer;
    uint16_t client_rtcp_port;
    uint16_t seq;
    uint32_t ssrc;
    uint32_t packet_count;
    uint32_t octet_count;
    uint32_t last_rtp_ts;
    uint32_t last_packet_ms;
    uint32_t last_report_ms;
    bool received;
    uint32_t receive_ssrc;
    uint32_t receive_first_seq;
    uint32_t receive_seq;
    uint32_t receive_packets;
    uint32_t receive_interval_expected;
    uint32_t receive_interval_packets;
    uint32_t receive_transit;
    uint32_t receive_jitter_fp;
    uint32_t remote_sr;
    uint32_t remote_sr_ms;
} rtsp_track_t;

typedef struct {
    int fd;
    bool playing;
    bool wait_idr;
    bool backchannel;
    char session[32];
    char req_buf[CLIENT_BUF_SIZE];
    int req_len;
    struct in_addr address;
    uint8_t *tx_queue;
    size_t tx_head;
    size_t tx_len;
    size_t peak_tx_len;
    uint32_t last_tx_progress;
    unsigned skipped_frames;
    bool had_skip;
    bool closing;
    rtsp_track_t tracks[TRACK_COUNT];
} rtsp_client_t;

static uint32_t mono_ms(void);
static rtsp_client_t g_clients[MAX_CLIENTS];

/* Bounded operational log in /tmp/rtsp.log (rotates at 32 KB) so RTSP disconnects are visible. */
static void rtsp_log(const char *fmt, ...) {
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    fprintf(stderr, "%s\n", msg);

    FILE *f = fopen("/tmp/rtsp.log", "a");
    if (f) {
        if (ftell(f) >= 32768) {
            fclose(f);
            rename("/tmp/rtsp.log", "/tmp/rtsp.log.old");
            f = fopen("/tmp/rtsp.log", "a");
        }
    }
    if (f) {
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        fprintf(f, "%02d:%02d:%02d %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
        fclose(f);
    }
}
static int g_audio_caps;
static rtsp_client_t *g_talker;
static uint32_t g_last_talk_ms;
static uint8_t g_talk_queue[TALK_BUFFER_SAMPLES];
static size_t g_talk_len;

static void release_talker(void) {
    g_talker = NULL;
    g_talk_len = 0;
    audio_stop_playback();
}

static void client_close(rtsp_client_t *c) {
    if (g_talker == c) release_talker();
    if (c->fd >= 0) close(c->fd);
    for (int i = 0; i < TRACK_COUNT; i++) {
        rtsp_track_t *t = &c->tracks[i];
        if (t->setup && !t->use_tcp) {
            close(t->rtp_fd);
            close(t->rtcp_fd);
        }
    }
    free(c->tx_queue);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void close_tcp(rtsp_client_t *c, const char *reason) {
    rtsp_log("[rtsp] %s fd=%d: %s (queued=%u skipped=%u)",
             inet_ntoa(c->address), c->fd, reason, (unsigned)c->tx_len, c->skipped_frames);
    client_close(c);
}

/* Each TCP socket preserves order across RTP, RTCP and RTSP control replies. */
static bool tcp_write(rtsp_client_t *c, const uint8_t *data, size_t size) {
    if (c->tx_len == 0) {
        ssize_t n;
        do {
            n = send(c->fd, data, size, MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n == (ssize_t)size) return true;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            close_tcp(c, strerror(errno));
            return false;
        }
        if (n > 0) {
            data += n;
            size -= (size_t)n;
        }
        c->last_tx_progress = mono_ms();
    }
    if (size > TCP_QUEUE_SIZE - c->tx_len) {
        close_tcp(c, "TCP reply queue full");
        return false;
    }
    if (!c->tx_queue) {
        c->tx_queue = malloc(TCP_QUEUE_SIZE);
        if (!c->tx_queue) {
            close_tcp(c, "TCP queue allocation failed");
            return false;
        }
    }

    size_t tail = (c->tx_head + c->tx_len) % TCP_QUEUE_SIZE;
    size_t first = TCP_QUEUE_SIZE - tail;
    if (first > size) first = size;
    memcpy(c->tx_queue + tail, data, first);
    if (first < size) memcpy(c->tx_queue, data + first, size - first);
    c->tx_len += size;
    if (c->tx_len > c->peak_tx_len) c->peak_tx_len = c->tx_len;
    return true;
}

static void flush_tcp(rtsp_client_t *c, uint32_t now) {
    while (c->tx_len) {
        size_t size = TCP_QUEUE_SIZE - c->tx_head;
        if (size > c->tx_len) size = c->tx_len;
        ssize_t n = send(c->fd, c->tx_queue + c->tx_head, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (now - c->last_tx_progress >= TCP_STALL_MS)
                close_tcp(c, "TCP stopped draining");
            return;
        }
        if (n <= 0) {
            close_tcp(c, n == 0 ? "TCP closed while writing" : strerror(errno));
            return;
        }
        c->tx_head = (c->tx_head + (size_t)n) % TCP_QUEUE_SIZE;
        c->tx_len -= (size_t)n;
        c->last_tx_progress = now;
    }
    if (c->closing) client_close(c);
}

static bool send_track_packet(rtsp_client_t *c, rtsp_track_t *t,
                              uint8_t *pkt, int len, bool rtcp) {
    if (t->use_tcp) {
        if (c->tx_len + (size_t)len + 4 > TCP_MEDIA_LIMIT) return false;
        pkt[0] = '$';
        pkt[1] = rtcp ? t->rtcp_chan : t->rtp_chan;
        pkt[2] = (uint8_t)(len >> 8);
        pkt[3] = (uint8_t)len;
        return tcp_write(c, pkt, 4 + (size_t)len);
    }
    struct sockaddr_in peer = t->peer;
    if (rtcp) peer.sin_port = t->client_rtcp_port;
    return sendto(rtcp ? t->rtcp_fd : t->rtp_fd, pkt + 4, len, MSG_DONTWAIT,
                  (struct sockaddr *)&peer, sizeof(peer)) == len;
}

static uint32_t read_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void write_be32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void rtp_send_packet(rtsp_client_t *c, rtsp_track_t *t, uint8_t type,
                            uint32_t pts, bool marker, const uint8_t *payload, int plen) {
    uint8_t pkt[4 + 12 + RTP_MTU];
    uint8_t *rtp = pkt + 4;
    rtp[0] = 0x80;
    rtp[1] = (marker ? 0x80 : 0) | type;
    rtp[2] = (uint8_t)(t->seq >> 8);
    rtp[3] = (uint8_t)t->seq++;
    write_be32(rtp + 4, pts);
    write_be32(rtp + 8, t->ssrc);
    memcpy(rtp + 12, payload, plen);

    if (send_track_packet(c, t, pkt, 12 + plen, false)) {
        t->packet_count++;
        t->octet_count += (uint32_t)plen;
        t->last_rtp_ts = pts;
        t->last_packet_ms = mono_ms();
    }
}

static void rtcp_sdes(uint8_t *sdes, uint32_t ssrc, const char *cname) {
    sdes[0] = 0x81;
    sdes[1] = 202;
    sdes[3] = 4;
    write_be32(sdes + 4, ssrc);
    sdes[8] = 1;
    sdes[9] = 8;
    memcpy(sdes + 10, cname, 8);
}

static void send_rtcp_reports(uint32_t now) {
    struct timeval wall;
    gettimeofday(&wall, NULL);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        rtsp_client_t *c = &g_clients[i];
        if (c->fd < 0 || !c->playing) continue;
        for (int track = 0; track < TRACK_COUNT && c->fd >= 0; track++) {
            rtsp_track_t *t = &c->tracks[track];
            if (t->last_report_ms && now - t->last_report_ms < 5000) continue;

            uint8_t pkt[4 + 32 + 20] = {0};
            uint8_t *rtcp = pkt + 4;
            int report_len;
            if (track == TRACK_SPEAKER) {
                if (!t->received) continue;
                uint32_t expected = t->receive_seq - t->receive_first_seq + 1;
                uint32_t lost = expected - t->receive_packets;
                uint32_t interval_expected = expected - t->receive_interval_expected;
                uint32_t interval_received = t->receive_packets - t->receive_interval_packets;
                t->receive_interval_expected = expected;
                t->receive_interval_packets = t->receive_packets;
                rtcp[0] = 0x81;
                rtcp[1] = 201;
                rtcp[3] = 7;
                write_be32(rtcp + 4, t->ssrc);
                write_be32(rtcp + 8, t->receive_ssrc);
                write_be32(rtcp + 12, lost > 0x7fffff ? 0x7fffff : lost);
                if (interval_expected > interval_received) {
                    uint32_t fraction =
                        (uint32_t)(((uint64_t)(interval_expected - interval_received) << 8) /
                                   interval_expected);
                    rtcp[12] = fraction > 255 ? 255 : (uint8_t)fraction;
                }
                write_be32(rtcp + 16, t->receive_seq);
                write_be32(rtcp + 20, t->receive_jitter_fp >> 4);
                write_be32(rtcp + 24, t->remote_sr);
                if (t->remote_sr)
                    write_be32(rtcp + 28, (uint32_t)(((uint64_t)(now - t->remote_sr_ms) << 16) / 1000));
                report_len = 32;
            } else {
                if (!t->packet_count) continue;
                unsigned clock_per_ms = track == TRACK_VIDEO ? 90 : AUDIO_RTP_RATE / 1000;
                rtcp[0] = 0x80;
                rtcp[1] = 200;
                rtcp[3] = 6;
                write_be32(rtcp + 4, t->ssrc);
                write_be32(rtcp + 8, (uint32_t)wall.tv_sec + UINT32_C(2208988800));
                write_be32(rtcp + 12, (uint32_t)(((uint64_t)wall.tv_usec << 32) / 1000000));
                write_be32(rtcp + 16, t->last_rtp_ts + (now - t->last_packet_ms) * clock_per_ms);
                write_be32(rtcp + 20, t->packet_count);
                write_be32(rtcp + 24, t->octet_count);
                report_len = 28;
            }
            rtcp_sdes(rtcp + report_len, t->ssrc, c->session);
            t->last_report_ms = now;
            send_track_packet(c, t, pkt, report_len + 20, true);
        }
    }
}

static void rtp_send_nal(rtsp_client_t *c, uint32_t pts90k,
                         const uint8_t *nal, int len, bool is_last_nal) {
    if (len <= 0 || c->fd < 0) return;

    if (len <= RTP_MTU) {
        rtp_send_packet(c, &c->tracks[TRACK_VIDEO], 96, pts90k, is_last_nal, nal, len);
        return;
    }

    /* RFC 6184 FU-A fragmentation */
    uint8_t nri_f = nal[0] & 0xE0;
    uint8_t ntype = nal[0] & 0x1F;
    const uint8_t *p = nal + 1;
    int rem = len - 1;
    bool first = true;
    uint8_t frag[RTP_MTU + 2];

    while (rem > 0 && c->fd >= 0) {
        int chunk = rem > (RTP_MTU - 2) ? (RTP_MTU - 2) : rem;
        bool last = (chunk == rem);
        frag[0] = nri_f | 28; /* FU-A */
        frag[1] = (first ? 0x80 : 0x00) | (last ? 0x40 : 0x00) | ntype;
        memcpy(frag + 2, p, chunk);
        rtp_send_packet(c, &c->tracks[TRACK_VIDEO], 96, pts90k, last && is_last_nal, frag, chunk + 2);
        p += chunk;
        rem -= chunk;
        first = false;
    }
}

static size_t nal_wire_size(int len) {
    if (len <= RTP_MTU) return 16 + (size_t)len;
    size_t payload = (size_t)len - 1;
    size_t fragments = (payload + RTP_MTU - 3) / (RTP_MTU - 2);
    return payload + fragments * 18;
}

/* Split an Annex-B frame into NAL units and broadcast to playing clients */
static void broadcast_annexb_frame(const uint8_t *data, int len, uint32_t pts90k) {
    const uint8_t *nals[32];
    int nal_lens[32];
    int ncount = 0;

    int i = 0;
    while (i + 3 < len && ncount < 32) {
        int sc_len = 0;
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) sc_len = 3;
        else if (i + 4 <= len && data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1) sc_len = 4;
        if (!sc_len) { i++; continue; }

        int start = i + sc_len;
        int j = start;
        while (j + 3 < len) {
            if (data[j] == 0 && data[j+1] == 0 &&
                (data[j+2] == 1 || (data[j+2] == 0 && data[j+3] == 1)))
                break;
            j++;
        }
        int end = (j + 3 < len) ? j : len;
        int nlen = end - start;
        if (nlen > 0) {
            uint8_t ntype = data[start] & 0x1F;
            if (ntype == 7 && nlen <= (int)sizeof(g_sps)) {
                memcpy(g_sps, data + start, nlen);
                g_sps_len = nlen;
            } else if (ntype == 8 && nlen <= (int)sizeof(g_pps)) {
                memcpy(g_pps, data + start, nlen);
                g_pps_len = nlen;
            }
            if (ntype != 9) { /* skip Access Unit Delimiter */
                nals[ncount] = data + start;
                nal_lens[ncount] = nlen;
                ncount++;
            }
        }
        i = end;
    }

    if (ncount == 0) return;

    bool has_idr = false;
    bool has_sps = false;
    size_t wire_size = 0;
    for (int k = 0; k < ncount; k++) {
        uint8_t nt = nals[k][0] & 0x1F;
        if (nt == 5) has_idr = true;
        if (nt == 7) has_sps = true;
        wire_size += nal_wire_size(nal_lens[k]);
    }

    for (int idx = 0; idx < MAX_CLIENTS; idx++) {
        rtsp_client_t *c = &g_clients[idx];
        if (c->fd < 0 || !c->playing || !c->tracks[TRACK_VIDEO].setup) continue;
        bool inject_params = c->wait_idr && !has_sps && g_sps_len > 0 && g_pps_len > 0;
        if (c->wait_idr && !has_idr) {
            if (c->skipped_frames) c->skipped_frames++;
            continue;
        }

        size_t frame_size = wire_size;
        if (inject_params) frame_size += nal_wire_size(g_sps_len) + nal_wire_size(g_pps_len);
        size_t media_cap = c->skipped_frames ? TCP_RESUME_LIMIT : TCP_MEDIA_LIMIT;
        if (c->tracks[TRACK_VIDEO].use_tcp &&
            (c->tx_len > media_cap || c->tx_len + frame_size > TCP_MEDIA_LIMIT)) {
            /* Drop complete frames until queue drains, then resume cleanly on a keyframe. */
            if (!c->skipped_frames++)
                rtsp_log("[rtsp] TCP backlog for %s; waiting for next keyframe (tx_len=%u frame=%u)",
                         inet_ntoa(c->address), (unsigned)c->tx_len, (unsigned)frame_size);
            c->had_skip = true;
            c->wait_idr = true;
            continue;
        }
        if (c->skipped_frames) {
            rtsp_log("[rtsp] TCP resumed for %s after skipping %u video frames",
                     inet_ntoa(c->address), c->skipped_frames);
            c->skipped_frames = 0;
        }
        c->wait_idr = false;
        if (inject_params) {
            rtp_send_nal(c, pts90k, g_sps, g_sps_len, false);
            rtp_send_nal(c, pts90k, g_pps, g_pps_len, false);
        }
        for (int k = 0; k < ncount && c->fd >= 0; k++) {
            rtp_send_nal(c, pts90k, nals[k], nal_lens[k], (k == ncount - 1));
        }
    }
}

/* ---- RTSP Request Handling ---- */
static bool rtsp_header(const char *req, const char *name, char *out, size_t cap) {
    size_t name_len = strlen(name);
    out[0] = '\0';
    for (const char *line = strchr(req, '\n'); line && line[1]; line = strchr(line, '\n')) {
        line++;
        if (line[0] == '\r') break;
        if (strncasecmp(line, name, name_len) != 0 || line[name_len] != ':') continue;
        const char *value = line + name_len + 1;
        while (*value == ' ' || *value == '\t') value++;
        size_t len = strcspn(value, "\r\n");
        while (len && (value[len - 1] == ' ' || value[len - 1] == '\t')) len--;
        if (len >= cap) return false;
        memcpy(out, value, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

static void rtsp_reply(rtsp_client_t *c, const char *response, int len) {
    if (len > 0) tcp_write(c, (const uint8_t *)response, (size_t)len);
}

static bool session_matches(const rtsp_client_t *c, const char *req) {
    char session[64];
    if (!rtsp_header(req, "Session", session, sizeof(session))) return true;
    session[strcspn(session, ";")] = '\0';
    return c->session[0] && strcmp(session, c->session) == 0;
}

static int rtsp_error(char *resp, size_t size, int cseq, const char *status) {
    return snprintf(resp, size, "RTSP/1.0 %s\r\nCSeq: %d\r\n\r\n", status, cseq);
}

static int new_udp_socket(uint16_t port, uint16_t *bound_port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(port)};
    socklen_t len = sizeof(local);
    if (fd >= FD_SETSIZE || bind(fd, (struct sockaddr *)&local, len) < 0 ||
        getsockname(fd, (struct sockaddr *)&local, &len) < 0) {
        close(fd);
        return -1;
    }
    *bound_port = ntohs(local.sin_port);
    return fd;
}

static bool channel_used(const rtsp_client_t *c, int channel) {
    for (int i = 0; i < TRACK_COUNT; i++) {
        const rtsp_track_t *t = &c->tracks[i];
        if (t->setup && t->use_tcp && (channel == t->rtp_chan || channel == t->rtcp_chan))
            return true;
    }
    return false;
}

static const char *setup_track(rtsp_client_t *c, int track, const char *transport,
                               char *reply, size_t size) {
    const char *unsupported = "461 Unsupported Transport";
    const char *fields = strchr(transport, ';');
    if (!fields) return unsupported;
    size_t protocol_len = (size_t)(fields - transport);
    if (strcasestr(fields, ";multicast")) return unsupported;
    rtsp_track_t t = {0};
    if (protocol_len == 11 && strncasecmp(transport, "RTP/AVP/TCP", 11) == 0) {
        int ch0 = track * 2, ch1 = ch0 + 1;
        const char *iv = strcasestr(fields, ";interleaved=");
        if ((iv && sscanf(iv + 13, "%d-%d", &ch0, &ch1) != 2) ||
            ch0 < 0 || ch0 > 255 || ch1 < 0 || ch1 > 255 || ch0 == ch1 ||
            channel_used(c, ch0) || channel_used(c, ch1))
            return unsupported;
        t.use_tcp = true;
        t.rtp_chan = (uint8_t)ch0;
        t.rtcp_chan = (uint8_t)ch1;
        snprintf(reply, size, "RTP/AVP/TCP;unicast;interleaved=%d-%d", ch0, ch1);
    } else if ((protocol_len == 7 && strncasecmp(transport, "RTP/AVP", 7) == 0) ||
               (protocol_len == 11 && strncasecmp(transport, "RTP/AVP/UDP", 11) == 0)) {
        int cp0, cp1;
        const char *cp = strcasestr(fields, ";client_port=");
        if (!cp || sscanf(cp + 13, "%d-%d", &cp0, &cp1) != 2 ||
            cp0 < 1 || cp0 > 65535 || cp1 < 1 || cp1 > 65535 || cp0 == cp1)
            return unsupported;
        socklen_t len = sizeof(t.peer);
        if (getpeername(c->fd, (struct sockaddr *)&t.peer, &len) < 0)
            return "500 Internal Server Error";
        t.peer.sin_port = htons((uint16_t)cp0);
        t.client_rtcp_port = htons((uint16_t)cp1);

        uint16_t sp0 = 0, sp1 = 0;
        t.rtcp_fd = -1;
        for (int attempt = 0; attempt < 20; attempt++) {
            t.rtp_fd = new_udp_socket(0, &sp0);
            if (t.rtp_fd < 0) break;
            /* Linux often selects an odd ephemeral port for bind(port=0). */
            if (sp0 & 1) {
                t.rtcp_fd = t.rtp_fd;
                sp1 = sp0;
                sp0--;
                t.rtp_fd = new_udp_socket(sp0, &sp0);
                if (t.rtp_fd >= 0) break;
                close(t.rtcp_fd);
                t.rtcp_fd = -1;
            } else {
                t.rtcp_fd = new_udp_socket(sp0 + 1, &sp1);
                if (t.rtcp_fd >= 0) break;
                close(t.rtp_fd);
                t.rtp_fd = -1;
            }
        }
        if (t.rtcp_fd < 0) return "453 Not Enough Bandwidth";
        snprintf(reply, size, "RTP/AVP;unicast;client_port=%d-%d;server_port=%u-%u",
                 cp0, cp1, sp0, sp1);
    } else {
        return unsupported;
    }
    t.ssrc = ((uint32_t)rand() << 1) ^ (uint32_t)rand();
    t.seq = (uint16_t)rand();
    t.setup = true;
    c->tracks[track] = t;
    return NULL;
}

static int track_from_url(const char *url) {
    const char *path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : url;
    if (!path) return -1;
    if (strcmp(path, "/stream1") == 0 || strcmp(path, "/stream1/") == 0 ||
        strcmp(path, "/stream1/track1") == 0 || strcmp(path, "/track1") == 0)
        return TRACK_VIDEO;
    if (strcmp(path, "/stream1/track2") == 0) return TRACK_MIC;
    if (strcmp(path, "/stream1/track3") == 0) return TRACK_SPEAKER;
    return -1;
}

static void handle_rtsp_request(rtsp_client_t *c, const char *req, int iav_fd) {
    char method[32] = {0}, url[512] = {0}, field[512];
    int cseq = 0;
    char resp[4096];
    int rlen;

    sscanf(req, "%31s %511s", method, url);
    if (rtsp_header(req, "CSeq", field, sizeof(field))) cseq = atoi(field);
    if (rtsp_header(req, "Require", field, sizeof(field)) &&
        (strcmp(field, BACKCHANNEL_TAG) != 0 || !(g_audio_caps & AUDIO_PLAYBACK))) {
        rlen = snprintf(resp, sizeof(resp),
            "RTSP/1.0 551 Option Not Supported\r\nCSeq: %d\r\nUnsupported: %s\r\n\r\n",
            cseq, field);
    } else if (strcmp(method, "OPTIONS") == 0) {
        rlen = snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\nCSeq: %d\r\n"
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n\r\n",
            cseq);
    } else if (!rtsp_verify_auth(method, url, req)) {
        if (c->playing)
            rtsp_log("[rtsp] %s rejected for active session fd=%d: authentication", method, c->fd);
        rlen = snprintf(resp, sizeof(resp),
            "RTSP/1.0 401 Unauthorized\r\nCSeq: %d\r\n"
            "WWW-Authenticate: Digest realm=\"NestCamOutdoor\", nonce=\"%s\"\r\n"
            "WWW-Authenticate: Basic realm=\"NestCamOutdoor\"\r\n\r\n",
            cseq, g_rtsp_nonce);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        c->backchannel = strcmp(field, BACKCHANNEL_TAG) == 0;
        char sps_b64[512] = "", pps_b64[512] = "";
        char fmtp[1200] = "a=fmtp:96 packetization-mode=1\r\n";
        if (g_sps_len >= 4 && g_pps_len > 0) {
            base64_encode(g_sps, g_sps_len, sps_b64, sizeof(sps_b64));
            base64_encode(g_pps, g_pps_len, pps_b64, sizeof(pps_b64));
            snprintf(fmtp, sizeof(fmtp),
                "a=fmtp:96 packetization-mode=1; profile-level-id=%02x%02x%02x; sprop-parameter-sets=%s,%s\r\n",
                g_sps[1], g_sps[2], g_sps[3], sps_b64, pps_b64);
        }
        char sdp[2048];
        int sdplen = snprintf(sdp, sizeof(sdp),
            "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=NestCamOutdoor\r\n"
            "c=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\n"
            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
            "%sa=control:track1\r\n%s%s%s", fmtp,
            c->backchannel ? "a=recvonly\r\n" : "",
            (g_audio_caps & AUDIO_CAPTURE) ?
                "m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n"
                "a=recvonly\r\na=control:track2\r\n" : "",
            c->backchannel ?
                "m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000\r\n"
                "a=sendonly\r\na=control:track3\r\n" : "");
        size_t url_len = strlen(url);
        rlen = snprintf(resp, sizeof(resp),
            "RTSP/1.0 200 OK\r\nCSeq: %d\r\nContent-Base: %s%s\r\n"
            "Content-Type: application/sdp\r\nContent-Length: %d\r\n\r\n%s",
            cseq, url, url_len && url[url_len - 1] == '/' ? "" : "/", sdplen, sdp);
    } else if (!session_matches(c, req)) {
        if (c->playing)
            rtsp_log("[rtsp] %s rejected for active session fd=%d: session mismatch", method, c->fd);
        rlen = rtsp_error(resp, sizeof(resp), cseq, "454 Session Not Found");
    } else if (strcmp(method, "SETUP") == 0) {
        int track = track_from_url(url);
        const char *error = NULL;
        char transport[256];
        if (track < 0 || (track == TRACK_MIC && !(g_audio_caps & AUDIO_CAPTURE)) ||
            (track == TRACK_SPEAKER && (!(g_audio_caps & AUDIO_PLAYBACK) || !c->backchannel)))
            error = "404 Not Found";
        else if (c->tracks[track].setup || c->playing)
            error = "455 Method Not Valid in This State";
        else if (!rtsp_header(req, "Transport", field, sizeof(field)))
            error = "461 Unsupported Transport";
        else
            error = setup_track(c, track, field, transport, sizeof(transport));
        if (error) {
            rlen = rtsp_error(resp, sizeof(resp), cseq, error);
        } else {
            if (!c->session[0]) snprintf(c->session, sizeof(c->session), "%08x", (unsigned)rand());
            rlen = snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: %d\r\nTransport: %s\r\nSession: %s\r\n\r\n",
                cseq, transport, c->session);
        }
    } else if (strcmp(method, "PLAY") == 0) {
        if (!c->session[0]) {
            rlen = rtsp_error(resp, sizeof(resp), cseq, "455 Method Not Valid in This State");
        } else {
            c->playing = true;
            c->wait_idr = c->tracks[TRACK_VIDEO].setup;
            if (c->wait_idr) ioctl(iav_fd, IAV_IOC_FORCE_IDR_EX, 1);
            rtsp_log("[rtsp] PLAY %s fd=%d tcp=%d video=%d mic=%d spk=%d",
                     inet_ntoa(c->address), c->fd, c->tracks[TRACK_VIDEO].use_tcp ? 1 : 0,
                     c->tracks[TRACK_VIDEO].setup, c->tracks[TRACK_MIC].setup,
                     c->tracks[TRACK_SPEAKER].setup);
            rlen = snprintf(resp, sizeof(resp),
                "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\nRange: npt=0.000-\r\n\r\n",
                cseq, c->session);
        }
    } else if (strcmp(method, "TEARDOWN") == 0) {
        rlen = snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\nSession: %s\r\n\r\n",
                        cseq, c->session);
        rtsp_log("[rtsp] TEARDOWN from %s fd=%d", inet_ntoa(c->address), c->fd);
        rtsp_reply(c, resp, rlen);
        c->playing = false;
        if (c->tx_len) c->closing = true;
        else client_close(c);
        return;
    } else if (strcmp(method, "GET_PARAMETER") == 0 || strcmp(method, "SET_PARAMETER") == 0) {
        rtsp_log("[rtsp] %s %s fd=%d", method, inet_ntoa(c->address), c->fd);
        rlen = snprintf(resp, sizeof(resp), "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%s%s%s\r\n",
            cseq, c->session[0] ? "Session: " : "", c->session, c->session[0] ? "\r\n" : "");
    } else {
        rlen = rtsp_error(resp, sizeof(resp), cseq, "501 Not Implemented");
    }

    if (rlen >= (int)sizeof(resp)) client_close(c);
    else rtsp_reply(c, resp, rlen);
}

static void receive_talk_rtp(rtsp_client_t *c, const uint8_t *rtp, size_t len) {
    if (!c->playing || len < 12 || (rtp[0] >> 6) != 2 || (rtp[1] & 0x7f) != 0)
        return;
    size_t start = 12 + (rtp[0] & 15) * 4;
    if (rtp[0] & 0x10) {
        if (start + 4 > len) return;
        start += 4 + ((size_t)rtp[start + 2] * 256 + rtp[start + 3]) * 4;
    }
    if (rtp[0] & 0x20) {
        size_t pad = rtp[len - 1];
        if (pad == 0 || pad > len) return;
        len -= pad;
    }
    if (start >= len || len - start > sizeof(g_talk_queue)) return;

    uint32_t now = mono_ms();
    if (g_talker && now - g_last_talk_ms >= TALK_IDLE_MS) release_talker();
    if (g_talker && g_talker != c) return;
    rtsp_track_t *t = &c->tracks[TRACK_SPEAKER];
    uint16_t seq = (uint16_t)(rtp[2] * 256 + rtp[3]);
    uint32_t ssrc = read_be32(rtp + 8);
    uint32_t transit = now * (AUDIO_RTP_RATE / 1000) - read_be32(rtp + 4);
    if (g_talker == c && t->received && t->receive_ssrc == ssrc) {
        uint16_t step = (uint16_t)(seq - t->receive_seq);
        if (step == 0 || step > INT16_MAX) return;
        t->receive_seq += step;
        int64_t delta = (int32_t)(transit - t->receive_transit);
        uint32_t abs_delta = (uint32_t)(delta < 0 ? -delta : delta);
        t->receive_jitter_fp += abs_delta - ((t->receive_jitter_fp + 8) >> 4);
    } else {
        t->receive_first_seq = t->receive_seq = seq;
        t->receive_packets = 0;
        t->receive_interval_expected = 0;
        t->receive_interval_packets = 0;
        t->receive_jitter_fp = 0;
        t->remote_sr = 0;
        t->last_report_ms = 0;
    }
    t->received = true;
    t->receive_ssrc = ssrc;
    t->receive_transit = transit;
    t->receive_packets++;
    g_talker = c;
    g_last_talk_ms = now;

    size_t samples = len - start;
    if (g_talk_len + samples > sizeof(g_talk_queue)) {
        size_t drop = g_talk_len + samples - sizeof(g_talk_queue);
        g_talk_len -= drop;
        memmove(g_talk_queue, g_talk_queue + drop, g_talk_len);
    }
    memcpy(g_talk_queue + g_talk_len, rtp + start, samples);
    g_talk_len += samples;
}

static void receive_talk_rtcp(rtsp_client_t *c, const uint8_t *rtcp, size_t len) {
    rtsp_track_t *t = &c->tracks[TRACK_SPEAKER];
    if (!c->playing || !t->received || len < 28 || (rtcp[0] >> 6) != 2 ||
        rtcp[1] != 200 || read_be32(rtcp + 4) != t->receive_ssrc)
        return;
    size_t size = ((size_t)rtcp[2] * 256 + rtcp[3] + 1) * 4;
    if (size < 28 || size > len) return;
    t->remote_sr = (read_be32(rtcp + 8) << 16) | (read_be32(rtcp + 12) >> 16);
    t->remote_sr_ms = mono_ms();
}

static void service_audio(uint32_t now) {
    static uint8_t mic[AUDIO_PACKET_SAMPLES];
    static size_t mic_len;
    static uint32_t mic_ts;
    static uint32_t last_capture_ms;

    if (g_audio_caps & AUDIO_CAPTURE) {
        for (int packet = 0; packet < 8; packet++) {
            size_t n = audio_capture(mic + mic_len, sizeof(mic) - mic_len);
            if (!n) break;
            if (now - last_capture_ms >= 120) {
                mic_ts = now * (AUDIO_RTP_RATE / 1000);
            }
            last_capture_ms = now;
            mic_len += n;
            if (mic_len != sizeof(mic)) continue;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                rtsp_client_t *c = &g_clients[i];
                rtsp_track_t *t = &c->tracks[TRACK_MIC];
                if (c->fd >= 0 && c->playing && t->setup)
                    rtp_send_packet(c, t, 0, mic_ts, false, mic, sizeof(mic));
            }
            mic_len = 0;
            mic_ts += AUDIO_PACKET_SAMPLES;
        }
    }

    if (g_talker && now - g_last_talk_ms >= TALK_IDLE_MS) release_talker();
    while (g_talk_len) {
        size_t n = audio_play(g_talk_queue, g_talk_len);
        if (!n) break;
        g_talk_len -= n;
        memmove(g_talk_queue, g_talk_queue + n, g_talk_len);
    }
    if (g_talker && now - g_last_talk_ms >= 45) audio_start_pending();
}

static void read_talk_udp(rtsp_client_t *c, bool rtcp) {
    const rtsp_track_t *t = &c->tracks[TRACK_SPEAKER];
    int fd = rtcp ? t->rtcp_fd : t->rtp_fd;
    uint16_t port = rtcp ? t->client_rtcp_port : t->peer.sin_port;
    uint8_t packet[CLIENT_BUF_SIZE];
    for (int i = 0; i < 16; i++) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        ssize_t n = recvfrom(fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC,
                             (struct sockaddr *)&peer, &len);
        if (n < 0) break;
        if (n > (ssize_t)sizeof(packet) || peer.sin_addr.s_addr != t->peer.sin_addr.s_addr ||
            peer.sin_port != port)
            continue;
        if (rtcp) receive_talk_rtcp(c, packet, (size_t)n);
        else receive_talk_rtp(c, packet, (size_t)n);
    }
}

static void poll_rtsp(int srv_fd, int iav_fd) {
    fd_set rfds;
    FD_ZERO(&rfds);
    uint32_t now = mono_ms();
    FD_SET(srv_fd, &rfds);
    int maxfd = srv_fd;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        rtsp_client_t *c = &g_clients[i];
        if (c->fd >= 0 && c->tx_len) flush_tcp(c, now);
        if (c->fd >= 0 && !c->closing) {
            FD_SET(c->fd, &rfds);
            if (c->fd > maxfd) maxfd = c->fd;
            rtsp_track_t *t = &c->tracks[TRACK_SPEAKER];
            if (c->playing && t->setup && !t->use_tcp) {
                FD_SET(t->rtp_fd, &rfds);
                FD_SET(t->rtcp_fd, &rfds);
                if (t->rtp_fd > maxfd) maxfd = t->rtp_fd;
                if (t->rtcp_fd > maxfd) maxfd = t->rtcp_fd;
            }
        }
    }
    struct timeval tv = {0, 0};
    int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
    if (n <= 0) return;

    if (FD_ISSET(srv_fd, &rfds)) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(srv_fd, (struct sockaddr *)&cli, &clen);
        if (cfd >= 0) {
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_clients[i].fd < 0) { slot = i; break; }
            }
            if (slot < 0 || cfd >= FD_SETSIZE) {
                close(cfd);
            } else {
                int fl = fcntl(cfd, F_GETFL, 0);
                fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
                int one = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                int sndbuf = 65536;
                setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                rtsp_client_t *c = &g_clients[slot];
                client_close(c);
                c->fd = cfd;
                c->address = cli.sin_addr;
                rtsp_log("[rtsp] client connected from %s fd=%d",
                         inet_ntoa(cli.sin_addr), cfd);
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        rtsp_client_t *c = &g_clients[i];
        if (c->fd < 0 || c->closing) continue;
        rtsp_track_t *speaker = &c->tracks[TRACK_SPEAKER];
        if (c->playing && speaker->setup && !speaker->use_tcp) {
            if (FD_ISSET(speaker->rtp_fd, &rfds)) read_talk_udp(c, false);
            if (FD_ISSET(speaker->rtcp_fd, &rfds)) read_talk_udp(c, true);
        }
        if (!FD_ISSET(c->fd, &rfds)) continue;
        int space = CLIENT_BUF_SIZE - 1 - c->req_len;
        if (space <= 0) {
            close_tcp(c, "RTSP receive buffer full");
            continue;
        }
        ssize_t r = recv(c->fd, c->req_buf + c->req_len, space, 0);
        if (r <= 0) {
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
            close_tcp(c, r == 0 ? "client closed connection" : strerror(errno));
            continue;
        }
        c->req_len += (int)r;
        c->req_buf[c->req_len] = '\0';

        /* A read can contain partial messages or several RTSP/RTP frames. */
        while (c->fd >= 0 && !c->closing && c->req_len > 0) {
            int consumed;
            if (c->req_buf[0] == '$') {
                if (c->req_len < 4) break;
                const uint8_t *frame = (const uint8_t *)c->req_buf;
                consumed = 4 + (frame[2] * 256 + frame[3]);
                if (c->req_len < consumed) break;
                if (speaker->setup && speaker->use_tcp) {
                    if (frame[1] == speaker->rtp_chan)
                        receive_talk_rtp(c, frame + 4, (size_t)consumed - 4);
                    else if (frame[1] == speaker->rtcp_chan)
                        receive_talk_rtcp(c, frame + 4, (size_t)consumed - 4);
                }
            } else {
                char *end = strstr(c->req_buf, "\r\n\r\n");
                if (!end) break;
                int header_len = (int)((end + 4) - c->req_buf);
                char field[32];
                *end = '\0';
                long body_len = 0;
                if (rtsp_header(c->req_buf, "Content-Length", field, sizeof(field))) {
                    char *stop;
                    body_len = strtol(field, &stop, 10);
                    if (*stop || body_len < 0 || body_len > CLIENT_BUF_SIZE - header_len - 1) {
                        client_close(c);
                        break;
                    }
                }
                consumed = header_len + (int)body_len;
                if (c->req_len < consumed) {
                    *end = '\r';
                    break;
                }
                handle_rtsp_request(c, c->req_buf, iav_fd);
            }
            if (c->fd < 0) break;
            c->req_len -= consumed;
            memmove(c->req_buf, c->req_buf + consumed, c->req_len);
            c->req_buf[c->req_len] = '\0';
        }
    }
}

/* ---- Night Vision Hardware Control (hwver=5 Nest Cam Outdoor) ---- */
#define NV_IR_PWR_GPIO_DIR "/sys/class/gpio/gpio45/direction"
#define NV_IR_PWR_GPIO_VAL "/sys/class/gpio/gpio45/value"
#define NV_IR_DIR_GPIO_DIR "/sys/class/gpio/gpio46/direction"
#define NV_IR_DIR_GPIO_VAL "/sys/class/gpio/gpio46/value"
#define NV_IR_PWM_PATH     "/sys/class/backlight/pwm-backlight.0/brightness"
#define NV_CTRL_PATH       "/tmp/night_vision"
#define NV_STATUS_PATH     "/tmp/night_vision.status"

typedef enum {
    NV_MODE_AUTO = 0,
    NV_MODE_OFF  = 1,
    NV_MODE_ON   = 2,
} nv_mode_t;

typedef struct {
    nv_mode_t mode;
    bool      active;            /* false = day, true = night */
    bool      hw_ready;
    int       ir_pwm;            /* 0 (off) or 5..500 (on) */
    bool      coil_energized;
    uint32_t  coil_off_ms;       /* monotonic ms when gpio45 must return to '1' */
    uint32_t  entered_night_ms;
    uint32_t  exited_night_ms;
    uint32_t  cooldown_ms;       /* night->day suppression window (ms) */
    uint32_t  probe_backoff_until_ms; /* suppress IR-off probe after close-reflection drop */
    int       probe_ready_ticks; /* consecutive ticks at dim floor before IR-off probe */
    int       dark_ticks;        /* consecutive dark AE ticks (~3 Hz) */
    int       bright_ticks;      /* consecutive bright AE ticks (~3 Hz) */
    int       last_luma;
} nv_state_t;

static uint32_t mono_ms(void) {
    static long clk_tck = 0;
    if (clk_tck <= 0) {
        clk_tck = sysconf(_SC_CLK_TCK);
        if (clk_tck <= 0) clk_tck = 100;
    }
    struct tms t;
    uint32_t ticks = (uint32_t)times(&t);
    return (uint32_t)(((uint64_t)ticks * 1000u) / (uint64_t)clk_tck);
}

static bool write_sysfs_str(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;
    size_t len = strlen(val);
    ssize_t n = write(fd, val, len);
    close(fd);
    return n == (ssize_t)len;
}

static void nv_set_ir_pwm(nv_state_t *nv, int pwm) {
    if (pwm < 0) pwm = 0;
    if (pwm > 500) pwm = 500;
    if (nv->ir_pwm == pwm && nv->hw_ready) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", pwm);
    if (write_sysfs_str(NV_IR_PWM_PATH, buf)) {
        nv->ir_pwm = pwm;
    }
}

/* ---- Ambarella A5s ISP Calibration (Black Level + AR0330 3D LUT + Tone Curve) ---- */
#define IDSP_REG_PATH        "/etc/idsp/reg.bin"
#define IDSP_DAY_3D_PATH     "/etc/idsp/sensors/mt9t002_03_3D.bin"
#define IDSP_NIGHT_3D_PATH   "/etc/idsp/sensors/mt9t002_04_3D.bin"
#define IDSP_REG_FILE_SIZE   18752
#define IDSP_3D_FILE_SIZE    17536
#define IDSP_REG_TABLE_OFF   0x3f80 /* matches -512 internal BLC pedestal used by stock photon */
#define IDSP_3D_TABLE_OFF    0x0080 /* 128-byte header before 16KB 16x16x16 3D LUT */
#define AR0330_BLC_14BIT     (-672) /* 12-bit sensor pedestal (~168) << 2 */
#define AR0330_DEF_BLC       (-512)

static uint8_t  g_idsp_reg[IDSP_REG_FILE_SIZE];
static uint8_t  g_idsp_day_3d[IDSP_3D_FILE_SIZE];
static uint8_t  g_idsp_night_3d[IDSP_3D_FILE_SIZE];
static uint32_t g_idsp_tone[256];
static bool     g_idsp_day_ok = false;
static bool     g_idsp_night_ok = false;

static bool read_exact_file(const char *path, void *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, (uint8_t *)buf + got, len - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    return got == len;
}

static void isp_init_calibration(int iav_fd) {
    for (int i = 0; i < 256; i++) {
        uint32_t v = (uint32_t)((i * 1023 + 127) / 255);
        g_idsp_tone[i] = (v << 20) | (v << 10) | v;
    }
    bool reg_ok = read_exact_file(IDSP_REG_PATH, g_idsp_reg, sizeof(g_idsp_reg));
    g_idsp_day_ok = reg_ok && read_exact_file(IDSP_DAY_3D_PATH, g_idsp_day_3d, sizeof(g_idsp_day_3d));
    g_idsp_night_ok = reg_ok && read_exact_file(IDSP_NIGHT_3D_PATH, g_idsp_night_3d, sizeof(g_idsp_night_3d));
    if (!g_idsp_day_ok) {
        fprintf(stderr, "[isp] warning: failed to load %s / %s\n", IDSP_REG_PATH, IDSP_DAY_3D_PATH);
        return;
    }
    struct iav_blc_global_offset blc = {
        .gr_offset = AR0330_BLC_14BIT - AR0330_DEF_BLC,
        .r_offset  = AR0330_BLC_14BIT - AR0330_DEF_BLC,
        .b_offset  = AR0330_BLC_14BIT - AR0330_DEF_BLC,
        .gb_offset = AR0330_BLC_14BIT - AR0330_DEF_BLC,
        .def_r     = AR0330_DEF_BLC,
        .def_g     = AR0330_DEF_BLC,
        .def_b     = AR0330_DEF_BLC,
        .pad       = 0,
    };
    if (ioctl(iav_fd, IAV_IOC_IMG_BLACK_LEVEL_GLOBAL_OFFSET, &blc) < 0) {
        fprintf(stderr, "[isp] BLACK_LEVEL_GLOBAL_OFFSET failed errno=%d\n", errno);
    }
}

static void isp_apply_color_lut(int iav_fd, bool night) {
    const uint8_t *lut = (night && g_idsp_night_ok) ? g_idsp_night_3d :
                         g_idsp_day_ok ? g_idsp_day_3d : NULL;
    if (!lut) return;
    struct iav_color_correction cc;
    memset(&cc, 0, sizeof(cc));
    cc.enable = 1;
    cc.r_gain = 65536;
    cc.g_gain = 65536;
    cc.b_gain = 65536;
    cc.reg_addr = (uint32_t)(uintptr_t)(g_idsp_reg + IDSP_REG_TABLE_OFF);
    cc.lut3d_addr = (uint32_t)(uintptr_t)(lut + IDSP_3D_TABLE_OFF);
    cc.no_tone_curve = 0;
    cc.tone_curve_addr = (uint32_t)(uintptr_t)g_idsp_tone;
    if (ioctl(iav_fd, IAV_IOC_IMG_COLOR_CORRECTION, &cc) < 0) {
        fprintf(stderr, "[isp] COLOR_CORRECTION(%s) failed errno=%d\n",
                night ? "NIGHT" : "DAY", errno);
    }
}

static void nv_set_isp_bw(int iav_fd, bool bw) {
    isp_apply_color_lut(iav_fd, bw);
    struct iav_rgb_to_yuv mat;
    memset(&mat, 0, sizeof(mat));
    if (bw) {
        /* Monochrome: keep full luminance Y, zero U/V chroma rows */
        mat.matrix_values[0] = 306;  mat.matrix_values[1] = 601;  mat.matrix_values[2] = 117;
        mat.matrix_values[3] = 0;    mat.matrix_values[4] = 0;    mat.matrix_values[5] = 0;
        mat.matrix_values[6] = 0;    mat.matrix_values[7] = 0;    mat.matrix_values[8] = 0;
    } else {
        /* Full-range BT.601 color matrix (matches stock Ambarella/Nest defaults) */
        mat.matrix_values[0] = 306;  mat.matrix_values[1] = 601;  mat.matrix_values[2] = 117;
        mat.matrix_values[3] = -173; mat.matrix_values[4] = -339; mat.matrix_values[5] = 512;
        mat.matrix_values[6] = 512;  mat.matrix_values[7] = -429; mat.matrix_values[8] = -83;
    }
    mat.y_offset = 0;
    mat.u_offset = 128;
    mat.v_offset = 128;
    mat.reserved = 0;
    if (ioctl(iav_fd, IAV_IOC_IMG_RGB_TO_YUV_SETUP, &mat) < 0) {
        fprintf(stderr, "[nv] RGB_TO_YUV_SETUP(%s) failed errno=%d\n", bw ? "BW" : "COLOR", errno);
    }
}

static void nv_pulse_ir_filter(nv_state_t *nv, bool night_open) {
    /* Set mount direction (0=day/filter-in, 1=night/filter-out), then energize active-low coil for 300ms */
    write_sysfs_str(NV_IR_DIR_GPIO_VAL, night_open ? "1" : "0");
    write_sysfs_str(NV_IR_PWR_GPIO_VAL, "0");
    nv->coil_energized = true;
    nv->coil_off_ms = mono_ms() + 300u;
}

static void nv_poll_coil(nv_state_t *nv) {
    if (!nv->coil_energized) return;
    if ((int32_t)(mono_ms() - nv->coil_off_ms) >= 0) {
        write_sysfs_str(NV_IR_PWR_GPIO_VAL, "1");
        nv->coil_energized = false;
    }
}

static void nv_init(nv_state_t *nv, nv_mode_t mode) {
    memset(nv, 0, sizeof(*nv));
    nv->mode = mode;
    nv->cooldown_ms = 30000u;
    nv->ir_pwm = -1;

    /* Ensure gpio45/gpio46 are exported; set gpio45 HIGH (coil off) atomically first */
    write_sysfs_str("/sys/class/gpio/export", "45");
    write_sysfs_str("/sys/class/gpio/export", "46");
    bool ok1 = write_sysfs_str(NV_IR_PWR_GPIO_DIR, "high");
    bool ok2 = write_sysfs_str(NV_IR_DIR_GPIO_DIR, "low");
    nv->hw_ready = ok1 && ok2;
    nv_set_ir_pwm(nv, 0);

    /* Create default control file if missing */
    if (access(NV_CTRL_PATH, F_OK) != 0) {
        const char *mstr = (mode == NV_MODE_ON) ? "on\n" : (mode == NV_MODE_OFF) ? "off\n" : "auto\n";
        int fd = open(NV_CTRL_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t _w = write(fd, mstr, strlen(mstr));
            (void)_w;
            close(fd);
        }
    }
}

static void nv_shutdown(nv_state_t *nv) {
    write_sysfs_str(NV_IR_PWR_GPIO_VAL, "1");
    nv->coil_energized = false;
    nv_set_ir_pwm(nv, 0);
}

static void nv_read_ctrl_file(nv_state_t *nv) {
    int fd = open(NV_CTRL_PATH, O_RDONLY);
    if (fd < 0) return;
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    for (int i = 0; buf[i]; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
    if (strncmp(buf, "on", 2) == 0 || buf[0] == '1') nv->mode = NV_MODE_ON;
    else if (strncmp(buf, "off", 3) == 0 || buf[0] == '0') nv->mode = NV_MODE_OFF;
    else if (strncmp(buf, "auto", 4) == 0) nv->mode = NV_MODE_AUTO;
}

/* ---- Lightweight Auto-Exposure, AWB & Auto Night Vision via YUV sampling ---- */
typedef struct {
    bool     enabled;
    uint32_t shutter;     /* 512MHz ticks: 128000 (1/4000s) .. 17066667 (1/30s) */
    int      agc_db_x10;  /* 0 .. 240 (day) / 300 (night) */
    uint32_t r_gain;      /* 65536 = 1.0x */
    uint32_t g_gain;      /* 65536 = 1.0x */
    uint32_t b_gain;      /* 65536 = 1.0x */
    uint32_t day_r_gain;  /* saved daylight WB gains across night mode */
    uint32_t day_b_gain;
    int      step_count;
} ae_state_t;

static void ae_apply(int fd, ae_state_t *ae) {
    ioctl(fd, IAV_IOC_VIN_SRC_SET_SHUTTER_TIME, ae->shutter);
    /* 8.24 fixed-point dB */
    uint32_t agc_fp = (uint32_t)(((uint64_t)ae->agc_db_x10 << 24) / 10);
    ioctl(fd, IAV_IOC_VIN_SRC_SET_AGC_DB, agc_fp);
}

static void awb_apply(int fd, ae_state_t *ae) {
    uint32_t wb[5] = { ae->r_gain, ae->g_gain, ae->g_gain, ae->b_gain, 0 };
    ioctl(fd, IAV_IOC_IMG_RGB_GAIN_ADJUST, wb);
}

static void nv_apply_state(int iav_fd, nv_state_t *nv, ae_state_t *ae, bool enable_night, const char *reason) {
    uint32_t now = mono_ms();
    if (enable_night) {
        /* Anti-chatter: if we just exited night < 20s ago, double cooldown up to 5 min */
        if (nv->exited_night_ms && (now - nv->exited_night_ms) < 20000u) {
            nv->cooldown_ms *= 2u;
            if (nv->cooldown_ms > 300000u) nv->cooldown_ms = 300000u;
        } else {
            nv->cooldown_ms = 30000u;
        }
        ae->day_r_gain = ae->r_gain;
        ae->day_b_gain = ae->b_gain;
        ae->r_gain = 65536;
        ae->g_gain = 65536;
        ae->b_gain = 65536;
        awb_apply(iav_fd, ae);
        nv_set_isp_bw(iav_fd, true);
        nv_pulse_ir_filter(nv, true);
        nv_set_ir_pwm(nv, 250);
        nv->active = true;
        nv->entered_night_ms = now;
        nv->probe_backoff_until_ms = 0;
        nv->probe_ready_ticks = 0;
        nv->dark_ticks = 0;
        nv->bright_ticks = 0;
        fprintf(stderr, "[nv] NIGHT ON (%s, pwm=%d, cooldown=%us)\n",
                reason, nv->ir_pwm, nv->cooldown_ms / 1000u);
    } else {
        nv_set_ir_pwm(nv, 0);
        nv_pulse_ir_filter(nv, false);
        nv_set_isp_bw(iav_fd, false);
        ae->r_gain = ae->day_r_gain ? ae->day_r_gain : 96000;
        ae->g_gain = 65536;
        ae->b_gain = ae->day_b_gain ? ae->day_b_gain : 92000;
        awb_apply(iav_fd, ae);
        if (ae->agc_db_x10 > 240) {
            ae->agc_db_x10 = 240;
            ae_apply(iav_fd, ae);
        }
        nv->active = false;
        nv->exited_night_ms = now;
        nv->probe_backoff_until_ms = 0;
        nv->probe_ready_ticks = 0;
        nv->dark_ticks = 0;
        nv->bright_ticks = 0;
        fprintf(stderr, "[nv] NIGHT OFF (%s)\n", reason);
    }
}

static void nv_write_status(const nv_state_t *nv, const ae_state_t *ae) {
    int fd = open(NV_STATUS_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const char *mstr = (nv->mode == NV_MODE_ON) ? "on" : (nv->mode == NV_MODE_OFF) ? "off" : "auto";
    char buf[160];
    int len = snprintf(buf, sizeof(buf),
        "mode=%s state=%s ir_pwm=%d luma=%d shutter=%u agc=%.1f\n",
        mstr, nv->active ? "night" : "day", nv->ir_pwm,
        nv->last_luma, ae->shutter, ae->agc_db_x10 / 10.0);
    if (len > 0) {
        ssize_t _w = write(fd, buf, (size_t)len);
        (void)_w;
    }
    close(fd);
}

static void sample_preview(int fd, ae_state_t *ae, nv_state_t *nv,
                           motion_state_t *motion, bool update_ae, bool update_motion) {
    struct iav_yuv_info_ex yuv;
    memset(&yuv, 0, sizeof(yuv));
    yuv.source = 1; /* 2nd buffer (Preview A / 720x480) for low-overhead AE/AWB */
    int yr = ioctl(fd, IAV_IOC_READ_YUV_BUFFER_INFO_EX, &yuv);
    if (yr < 0 || !yuv.y_addr) {
        memset(&yuv, 0, sizeof(yuv));
        yuv.source = 3;
        yr = ioctl(fd, IAV_IOC_READ_YUV_BUFFER_INFO_EX, &yuv);
    }
    if (yr < 0) {
        if ((ae->step_count % 30) == 0)
            fprintf(stderr, "[ae] READ_YUV err=%d errno=%d\n", yr, errno);
        ae->step_count++;
        return;
    }
    if (!yuv.y_addr || yuv.width < 64 || yuv.height < 64 || yuv.pitch < yuv.width) {
        if ((ae->step_count % 30) == 0)
            fprintf(stderr, "[ae] YUV empty: y=0x%x uv=0x%x %ux%u pitch=%u\n",
                    yuv.y_addr, yuv.uv_addr, yuv.width, yuv.height, yuv.pitch);
        ae->step_count++;
        return;
    }

    const uint8_t *y = (const uint8_t *)(uintptr_t)yuv.y_addr;
    const uint8_t *uv = (const uint8_t *)(uintptr_t)yuv.uv_addr;

    if (update_motion) {
        static bool published, published_state;
        unsigned changed = motion_sample(motion, y, yuv.width, yuv.height, yuv.pitch,
                                         mono_ms(), nv->active);
        if ((!published || motion->active != published_state) && motion_publish(motion->active)) {
            published = true;
            published_state = motion->active;
            fprintf(stderr, "[motion] %s (%u changed cells)\n",
                    published_state ? "active" : "idle", changed);
        }
    }

    /* On-demand (SIGUSR1) or periodic (~5s) live JPEG snapshot for ONVIF GetSnapshotUri */
    if (g_snap_req || (update_ae && (ae->step_count % 15) == 0)) {
        g_snap_req = 0;
        if (nv12_save_jpeg("/tmp/snapshot.jpg.tmp", y, uv, (int)yuv.width, (int)yuv.height, (int)yuv.pitch)) {
            rename("/tmp/snapshot.jpg.tmp", "/tmp/snapshot.jpg");
        }
    }

    if (!update_ae) return;
    uint64_t y_sum = 0, u_sum = 0, v_sum = 0;
    int count = 0, uv_count = 0;
    int x0 = yuv.width / 10, x1 = yuv.width - x0;
    int y0 = yuv.height / 10, y1 = yuv.height - y0;
    int sx = (x1 - x0) / 16; if (sx < 2) sx = 2; sx &= ~1;
    int sy = (y1 - y0) / 16; if (sy < 2) sy = 2; sy &= ~1;
    for (int py = y0; py < y1; py += sy) {
        const uint8_t *row = y + py * yuv.pitch;
        const uint8_t *uvrow = uv ? (uv + (py / 2) * yuv.pitch) : NULL;
        for (int px = x0; px < x1; px += sx) {
            uint8_t lv = row[px];
            y_sum += lv;
            count++;
            if (uvrow && lv > 25 && lv < 235) {
                u_sum += uvrow[px & ~1];
                v_sum += uvrow[(px & ~1) + 1];
                uv_count++;
            }
        }
    }
    if (count == 0) return;
    int luma = (int)(y_sum / count);
    nv->last_luma = luma;

    /* Check /tmp/night_vision control file ~1x/sec */
    if ((ae->step_count % 3) == 0) {
        nv_read_ctrl_file(nv);
    }

    /* Handle forced Night Vision modes */
    if (nv->mode == NV_MODE_ON && !nv->active) {
        nv_apply_state(fd, nv, ae, true, "forced on");
    } else if (nv->mode == NV_MODE_OFF && nv->active) {
        nv_apply_state(fd, nv, ae, false, "forced off");
    }

    /* Target luma: 112 (deadband 98..126) */
    const uint32_t SH_MIN = 128000;    /* 1/4000 s */
    const uint32_t SH_MAX = 17066667;  /* 1/30 s */
    const int      AGC_MAX = nv->active ? 300 : 240; /* 30.0 dB night, 24.0 dB day */

    bool exp_changed = false;
    if (luma < 98) {
        if (ae->shutter < SH_MAX) {
            uint32_t step = ae->shutter / 5;
            if (step < 50000) step = 50000;
            if (luma < 50) step *= 2;
            ae->shutter = (ae->shutter + step > SH_MAX) ? SH_MAX : (ae->shutter + step);
            exp_changed = true;
        } else if (ae->agc_db_x10 < AGC_MAX) {
            ae->agc_db_x10 += (luma < 50) ? 20 : 10;
            if (ae->agc_db_x10 > AGC_MAX) ae->agc_db_x10 = AGC_MAX;
            exp_changed = true;
        }
    } else if (luma > 126) {
        if (ae->agc_db_x10 > 0) {
            ae->agc_db_x10 -= (luma > 180) ? 20 : 10;
            if (ae->agc_db_x10 < 0) ae->agc_db_x10 = 0;
            exp_changed = true;
        } else if (ae->shutter > SH_MIN) {
            uint32_t step = ae->shutter / 5;
            if (step < 20000) step = 20000;
            if (luma > 180) step *= 2;
            ae->shutter = (ae->shutter > SH_MIN + step) ? (ae->shutter - step) : SH_MIN;
            exp_changed = true;
        }
    }
    if (exp_changed) ae_apply(fd, ae);

    /* Dynamic IR LED PWM tuning + IR-off ambient probe in night mode.
     * Dim IR whenever exposure has headroom (agc==0 && shutter < 15M), not only when luma
     * overshoots, so gradual sunrise reduces IR PWM to 0 instead of deadlocking AE vs PWM. */
    const int      PWM_MIN_ON        = 5;
    const int      PWM_MAX           = 500;
    const uint32_t PWM_DIM_SHUTTER   = 15000000; /* ~1/34s (first 20% AE step below SH_MAX) */
    const uint32_t DAY_EXIT_SHUTTER  = 14000000; /* ~1/36.6s with IR off & 0 dB gain */
    const uint32_t FAST_EXIT_SHUTTER = 4000000;  /* ~1/128s unmistakable daylight */

    bool was_ir_off = (nv->ir_pwm == 0);
    if (nv->active) {
        uint32_t now_ms = mono_ms();
        bool probe_allowed = (nv->mode == NV_MODE_AUTO) &&
                             (nv->probe_backoff_until_ms == 0 ||
                              (int32_t)(now_ms - nv->probe_backoff_until_ms) >= 0 ||
                              luma > 180);

        if (nv->ir_pwm == 0) {
            if (nv->mode != NV_MODE_AUTO) {
                nv_set_ir_pwm(nv, PWM_MIN_ON);
                nv->probe_ready_ticks = 0;
            } else if (ae->agc_db_x10 > 0 || ae->shutter > DAY_EXIT_SHUTTER || luma < 90) {
                /* Scene got dark with IR off -> light was IR reflection, not ambient daylight */
                nv_set_ir_pwm(nv, PWM_MIN_ON);
                nv->probe_backoff_until_ms = now_ms + 60000u;
                nv->probe_ready_ticks = 0;
                nv->bright_ticks = 0;
                fprintf(stderr, "[nv] IR-off probe dark (luma=%d shutter=%u agc=%.1fdB), restoring IR\n",
                        luma, ae->shutter, ae->agc_db_x10 / 10.0);
            }
        } else {
            bool too_much_light = (ae->agc_db_x10 == 0 &&
                                   ((ae->shutter < PWM_DIM_SHUTTER && luma >= 98) || luma > 140));
            bool need_more_ir = (ae->shutter >= SH_MAX &&
                                 ((luma < 92 && ae->agc_db_x10 >= 100) ||
                                  (luma <= 120 && ae->agc_db_x10 >= 180)));

            if (too_much_light) {
                if (nv->ir_pwm > PWM_MIN_ON) {
                    int step = (ae->shutter < 6000000 || luma > 160) ? 50 :
                               (nv->ir_pwm <= 40) ? 10 : 20;
                    int next_pwm = nv->ir_pwm - step;
                    if (next_pwm < PWM_MIN_ON) next_pwm = PWM_MIN_ON;
                    nv_set_ir_pwm(nv, next_pwm);
                    nv->probe_ready_ticks = 0;
                } else if (probe_allowed && ae->shutter <= DAY_EXIT_SHUTTER) {
                    if (++nv->probe_ready_ticks >= 6) { /* ~2s steady at dim floor -> probe IR off */
                        nv_set_ir_pwm(nv, 0);
                        nv->probe_ready_ticks = 0;
                    }
                } else {
                    nv->probe_ready_ticks = 0;
                }
            } else {
                nv->probe_ready_ticks = 0;
                if (need_more_ir && nv->ir_pwm < PWM_MAX) {
                    int next_pwm = nv->ir_pwm + ((luma < 55) ? 40 : 20);
                    if (next_pwm > PWM_MAX) next_pwm = PWM_MAX;
                    nv_set_ir_pwm(nv, next_pwm);
                }
            }
        }
    }

    /* Automatic Day <-> Night switching with hysteresis and anti-oscillation cooldown */
    if (nv->mode == NV_MODE_AUTO) {
        if (!nv->active) {
            bool is_dark = (ae->shutter >= SH_MAX) &&
                           ((ae->agc_db_x10 >= 220 && luma < 92) ||
                            (ae->agc_db_x10 >= 240 && luma < 105));
            if (is_dark) {
                if (++nv->dark_ticks >= 18) { /* ~6s sustained darkness */
                    nv_apply_state(fd, nv, ae, true, "auto dark");
                }
            } else if (nv->dark_ticks > 0) {
                nv->dark_ticks--;
            }
        } else {
            uint32_t in_night_ms = mono_ms() - nv->entered_night_ms;
            bool ir_verified_off = (was_ir_off && nv->ir_pwm == 0);
            bool fast_daylight = ir_verified_off && ae->agc_db_x10 == 0 &&
                                 ae->shutter < FAST_EXIT_SHUTTER && luma >= 96;
            bool normal_daylight = ir_verified_off && ae->agc_db_x10 == 0 &&
                                   ae->shutter <= DAY_EXIT_SHUTTER && luma >= 96;
            bool extreme_override = (luma > 185 && ae->agc_db_x10 == 0 && ae->shutter < 1500000);
            uint32_t fast_min_ms = (nv->cooldown_ms > 30000u) ? nv->cooldown_ms : 10000u;

            if (extreme_override || (fast_daylight && in_night_ms >= fast_min_ms)) {
                if (++nv->bright_ticks >= 9) { /* ~3s override in unmistakable daylight */
                    nv_apply_state(fd, nv, ae, false,
                                   extreme_override ? "auto high-luma" : "auto daylight");
                }
            } else if (normal_daylight && in_night_ms >= nv->cooldown_ms) {
                if (++nv->bright_ticks >= 18) { /* ~6s sustained ambient daylight with IR off */
                    nv_apply_state(fd, nv, ae, false, "auto bright");
                }
            } else if (nv->bright_ticks > 0) {
                nv->bright_ticks--;
            }
        }
    }

    /* Gentle Grey-World AWB in Day mode only (Night mode stays neutral 1:1:1 + B&W) */
    if (!nv->active && uv_count >= 16) {
        int u_err = (int)(u_sum / uv_count) - 128;
        int v_err = (int)(v_sum / uv_count) - 128;
        bool wb_changed = false;
        if (u_err > 4 || u_err < -4) {
            int32_t nb = (int32_t)ae->b_gain - u_err * 220;
            if (nb < 32768) nb = 32768;
            if (nb > 163840) nb = 163840;
            ae->b_gain = (uint32_t)nb;
            wb_changed = true;
        }
        if (v_err > 4 || v_err < -4) {
            int32_t nr = (int32_t)ae->r_gain - v_err * 220;
            if (nr < 32768) nr = 32768;
            if (nr > 163840) nr = 163840;
            ae->r_gain = (uint32_t)nr;
            wb_changed = true;
        }
        if (wb_changed) awb_apply(fd, ae);
    }

    if ((++ae->step_count % 6) == 1) {
        nv_write_status(nv, ae);
    }
    if ((ae->step_count % 15) == 1) {
        fprintf(stderr, "[ae] luma=%d shutter=%u agc=%.1fdB rgb=(%u,%u,%u) nv=%s pwm=%d\n",
                luma, ae->shutter, ae->agc_db_x10 / 10.0,
                ae->r_gain, ae->g_gain, ae->b_gain,
                nv->active ? "night" : "day", nv->ir_pwm);
    }
}

/* ---- Adaptive Bitrate (AIMD over live TCP send-queue pressure) ---- */
typedef struct {
    bool     enabled;
    uint32_t min_bitrate;
    uint32_t max_bitrate;
    uint32_t cur_bitrate;
    uint32_t last_tick_ms;
    uint32_t last_drop_ms;
    uint32_t hold_until_ms;
    int      high_ticks;
    int      mild_ticks;
    int      clean_ticks;
} abr_state_t;

static bool set_encoder_bitrate(int iav_fd, uint32_t bitrate) {
    uint32_t br[5] = { 1, 0, bitrate, bitrate / 4, bitrate };
    return ioctl(iav_fd, IAV_IOC_SET_BITRATE_EX, br) >= 0;
}

static void abr_update(int iav_fd, abr_state_t *abr, uint32_t now) {
    if (!abr->enabled || abr->max_bitrate <= abr->min_bitrate) return;
    if (abr->last_tick_ms && (now - abr->last_tick_ms) < 500u) return;
    abr->last_tick_ms = now;

    bool any_tcp_playing = false;
    bool any_skipped = false;
    size_t max_queued = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        rtsp_client_t *c = &g_clients[i];
        if (c->fd < 0 || !c->playing || !c->tracks[TRACK_VIDEO].setup || !c->tracks[TRACK_VIDEO].use_tcp)
            continue;
        any_tcp_playing = true;
        int outq = 0;
        if (ioctl(c->fd, TIOCOUTQ, &outq) < 0 || outq < 0) outq = 0;
        /* Use instantaneous kernel + userspace backlog to avoid double-counting IDR bursts. */
        size_t total_q = (size_t)outq + c->tx_len;
        if (total_q > max_queued) max_queued = total_q;
        if (c->had_skip || c->skipped_frames > 0) any_skipped = true;
        c->peak_tx_len = c->tx_len;
        c->had_skip = false;
    }

    if (!any_tcp_playing) {
        abr->high_ticks = 0;
        abr->mild_ticks = 0;
        abr->clean_ticks = 0;
        return;
    }

    uint32_t next = abr->cur_bitrate;
    const char *reason = NULL;

    if (any_skipped || max_queued >= 128u * 1024u) {
        abr->clean_ticks = 0;
        abr->high_ticks = 0;
        abr->mild_ticks = 0;
        abr->hold_until_ms = now + 20000u;
        if (!abr->last_drop_ms || (now - abr->last_drop_ms) >= 2000u) {
            next = (abr->cur_bitrate * 7u) / 10u;
            if (next < abr->min_bitrate) next = abr->min_bitrate;
            abr->last_drop_ms = now;
            reason = any_skipped ? "frame-skip" : "spike-queue";
        }
    } else if (max_queued >= 80u * 1024u) {
        abr->clean_ticks = 0;
        abr->mild_ticks = 0;
        if (++abr->high_ticks >= 2) { /* 1.0s sustained heavy queue */
            abr->high_ticks = 0;
            abr->hold_until_ms = now + 20000u;
            if (!abr->last_drop_ms || (now - abr->last_drop_ms) >= 2000u) {
                next = (abr->cur_bitrate * 3u) / 4u;
                if (next < abr->min_bitrate) next = abr->min_bitrate;
                abr->last_drop_ms = now;
                reason = "high-queue";
            }
        }
    } else if (max_queued >= 40u * 1024u) {
        abr->clean_ticks = 0;
        abr->high_ticks = 0;
        if (++abr->mild_ticks >= 4) { /* 2.0s sustained elevated TCP send queue */
            abr->mild_ticks = 0;
            abr->hold_until_ms = now + 20000u;
            if (!abr->last_drop_ms || (now - abr->last_drop_ms) >= 2000u) {
                next = (abr->cur_bitrate * 85u) / 100u;
                if (next < abr->min_bitrate) next = abr->min_bitrate;
                abr->last_drop_ms = now;
                reason = "mild-queue";
            }
        }
    } else {
        abr->high_ticks = 0;
        abr->mild_ticks = 0;
        if (max_queued < 20u * 1024u && abr->cur_bitrate < abr->max_bitrate &&
            (!abr->hold_until_ms || (int32_t)(now - abr->hold_until_ms) >= 0)) {
            if (++abr->clean_ticks >= 30) { /* 15s clean delivery -> gentle +150 kbps step */
                abr->clean_ticks = 0;
                next = abr->cur_bitrate + 150000u;
                if (next > abr->max_bitrate) next = abr->max_bitrate;
                reason = "clean-ramp";
            }
        } else if (max_queued >= 20u * 1024u) {
            abr->clean_ticks = 0;
        }
    }

    /* Round to nearest 50 kbps for clean encoder steps */
    next = ((next + 25000u) / 50000u) * 50000u;
    if (next < abr->min_bitrate) next = abr->min_bitrate;
    if (next > abr->max_bitrate) next = abr->max_bitrate;

    if (reason && next != abr->cur_bitrate) {
        if (set_encoder_bitrate(iav_fd, next)) {
            rtsp_log("[abr] bitrate %u -> %u kbps (%s, max_q=%uB)",
                     abr->cur_bitrate / 1000u, next / 1000u, reason, (unsigned)max_queued);
            abr->cur_bitrate = next;
        } else {
            rtsp_log("[abr] SET_BITRATE_EX(%u) failed errno=%d", next, errno);
        }
    }
}

static void publish_audio_status(int caps) {
    const char *pending = NEST_AUDIO_STATUS ".new";
    FILE *file = fopen(pending, "w");
    if (!file) {
        perror("[audio] capability status");
        return;
    }
    fprintf(file, "%d\n", caps);
    if (fclose(file) == 0 && rename(pending, NEST_AUDIO_STATUS) == 0) return;
    perror("[audio] capability status");
    unlink(pending);
}

int main(int argc, char **argv) {
    int width = 1920, height = 1080;
    uint32_t bitrate = 3000000;
    uint32_t min_bitrate = 0;
    bool abr_on = true;
    int port = 554;
    uint32_t bayer = 2; /* GR - verified true daylight color on hwver=5 AR0330 */
    uint32_t mirror = 0;
    bool ae_on = true;
    int audio_requested = 0;
    nv_mode_t nv_mode = NV_MODE_AUTO;
    const char *auth_conf = "/mnt/dropcam/onvif.conf";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) width = atoi(argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) height = atoi(argv[++i]);
        else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) bitrate = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--min-bitrate") == 0 && i + 1 < argc) min_bitrate = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-abr") == 0) abr_on = false;
        else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-y") == 0 && i + 1 < argc) bayer = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) mirror = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) auth_conf = argv[++i];
        else if (strcmp(argv[i], "--no-auth") == 0) auth_conf = NULL;
        else if (strcmp(argv[i], "--no-ae") == 0) ae_on = false;
        else if (strcmp(argv[i], "--mic") == 0) audio_requested |= AUDIO_CAPTURE;
        else if (strcmp(argv[i], "--speaker") == 0) audio_requested |= AUDIO_PLAYBACK;
        else if (strcmp(argv[i], "--nv") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            if (strcmp(m, "on") == 0) nv_mode = NV_MODE_ON;
            else if (strcmp(m, "off") == 0) nv_mode = NV_MODE_OFF;
            else nv_mode = NV_MODE_AUTO;
        }
    }

    if (auth_conf) load_auth_conf(auth_conf);
    if ((audio_requested & AUDIO_PLAYBACK) && !g_auth_pass[0]) {
        fprintf(stderr, "[audio] speaker requires configured RTSP credentials\n");
        audio_requested &= ~AUDIO_PLAYBACK;
    }
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    {
        char nseed[64];
        snprintf(nseed, sizeof(nseed), "nest-rtsp-%ld-%d", (long)time(NULL), (int)getpid());
        md5_hex(nseed, g_rtsp_nonce);
    }

    setlinebuf(stderr);
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGUSR1, on_usr1);
    signal(SIGPIPE, SIG_IGN);
    {
        FILE *pf = fopen("/tmp/nest_rtsp.pid", "w");
        if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_clients[i].fd = -1;
    }

    int fd = open("/dev/iav", O_RDWR);
    if (fd < 0) { perror("open /dev/iav"); return 1; }

    if (ioctl(fd, IAV_IOC_ENTER_IDLE, 0) < 0) {
        perror("IAV_IOC_ENTER_IDLE");
        return 1;
    }

    int src = 0;
    ioctl(fd, IAV_IOC_VIN_SET_CURRENT_SRC, &src);
    if (ioctl(fd, IAV_IOC_VIN_SRC_SET_VIDEO_MODE, 30) < 0) {
        perror("VIN_SET_VIDEO_MODE(30)");
        return 1;
    }
    ioctl(fd, IAV_IOC_VIN_SRC_SET_FRAME_RATE, 17083750); /* 29.97 fps */

    uint32_t mcfg[2] = { mirror, bayer };
    ioctl(fd, IAV_IOC_VIN_SRC_SET_MIRROR_MODE, mcfg);

    ae_state_t ae = {
        .enabled = ae_on,
        .shutter = 8533333,
        .agc_db_x10 = 60,
        .r_gain = 96000,
        .g_gain = 65536,
        .b_gain = 92000,
        .step_count = 0,
    };
    ae_apply(fd, &ae);

    /* Raise system resource limit to allow up to 1920x1080 */
    uint32_t res[30] = {0};
    ioctl(fd, IAV_IOC_GET_SYSTEM_RESOURCE_LIMIT_EX, res);
    uint32_t max_wh = ((uint32_t)(height > 1080 ? height : 1080) << 16) |
                      (uint32_t)(width > 1920 ? width : 1920);
    res[0] = max_wh;
    res[2] = max_wh;
    if (ioctl(fd, IAV_IOC_SET_SYSTEM_RESOURCE_LIMIT_EX, res) < 0) {
        perror("SET_SYSTEM_RESOURCE_LIMIT_EX");
        return 1;
    }

    /* Main buffer = ENCODE(1), 2nd buffer = ENCODE(1) (gives live YUV for AE/AWB), disable VOUT preview */
    uint32_t btype[4] = {1, 1, 0, 0};
    ioctl(fd, IAV_IOC_SET_SOURCE_BUFFER_TYPE_ALL_EX, btype);

    struct iav_mmap_info bsb = {0};
    if (ioctl(fd, IAV_IOC_MAP_BSB2, &bsb) < 0) {
        perror("IAV_IOC_MAP_BSB2");
        return 1;
    }
    struct iav_mmap_info dsp = {0};
    bool preview_ready = ioctl(fd, IAV_IOC_MAP_DSP, &dsp) >= 0;
    if (!preview_ready) {
        perror("[preview] MAP_DSP");
        ae.enabled = false;
    }

    if (ioctl(fd, IAV_IOC_ENABLE_PREVIEW, 0) < 0) {
        perror("IAV_IOC_ENABLE_PREVIEW");
        return 1;
    }
    isp_init_calibration(fd);
    awb_apply(fd, &ae);

    nv_state_t nv;
    nv_init(&nv, nv_mode);
    if (nv.mode == NV_MODE_ON) {
        nv_apply_state(fd, &nv, &ae, true, "startup on");
    } else {
        /* Seat IR-cut filter in Day position on startup in case camera rebooted from Night mode */
        nv_set_isp_bw(fd, false);
        nv_pulse_ir_filter(&nv, false);
    }

    /* Configure Stream 0 H.264 */
    uint32_t ef[4] = { 1, 1, ((uint32_t)height << 16) | (uint32_t)width, 0 };
    if (ioctl(fd, IAV_IOC_SET_ENCODE_FORMAT_EX, ef) < 0) {
        perror("IAV_IOC_SET_ENCODE_FORMAT_EX");
        return 1;
    }

    if (!min_bitrate) {
        min_bitrate = bitrate / 5u;
        if (min_bitrate < 600000u) min_bitrate = 600000u;
    }
    if (min_bitrate > bitrate) min_bitrate = bitrate;
    uint32_t start_bitrate = bitrate;
    if (abr_on && bitrate > 1500000u) {
        start_bitrate = 1500000u;
        if (start_bitrate < min_bitrate) start_bitrate = min_bitrate;
    }
    abr_state_t abr = {
        .enabled = abr_on && (bitrate > min_bitrate),
        .min_bitrate = min_bitrate,
        .max_bitrate = bitrate,
        .cur_bitrate = start_bitrate,
    };
    set_encoder_bitrate(fd, abr.cur_bitrate);

    /* ALSA changes the I²S clock. Update the DSP timebase before encoding. */
    g_audio_caps = audio_open(audio_requested);
    if (g_audio_caps && ioctl(fd, IAV_IOC_SET_AUDIO_CLK_FREQ_EX, AUDIO_RTP_RATE) < 0) {
        perror("[audio] IAV_IOC_SET_AUDIO_CLK_FREQ_EX");
        audio_close();
        g_audio_caps = 0;
    }

    if (ioctl(fd, IAV_IOC_START_ENCODE_EX, 1) < 0) {
        perror("IAV_IOC_START_ENCODE_EX");
        return 1;
    }

    /* Open RTSP listening socket */
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    int fl = fcntl(srv_fd, F_GETFL, 0);
    fcntl(srv_fd, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv_fd, 8) < 0) {
        perror("bind/listen RTSP");
        return 1;
    }

    rtsp_log("[nest_rtsp] streaming %dx%d H.264 (%u..%u kbps, start=%u kbps, abr=%d, bayer=%u, nv=%s)",
             width, height, abr.min_bitrate / 1000u, abr.max_bitrate / 1000u,
             abr.cur_bitrate / 1000u, abr.enabled ? 1 : 0, bayer,
             nv.mode == NV_MODE_ON ? "on" : nv.mode == NV_MODE_OFF ? "off" : "auto");

    fprintf(stderr, "[audio] microphone=%s speaker=%s\n",
            g_audio_caps & AUDIO_CAPTURE ? "enabled" : "off",
            g_audio_caps & AUDIO_PLAYBACK ? "enabled" : "off");
    publish_audio_status(g_audio_caps);

    motion_state_t motion = {0};
    unlink(NEST_MOTION_PATH);
    uint32_t frame_cnt = 0;
    while (g_running) {
        nv_poll_coil(&nv);
        poll_rtsp(srv_fd, fd);
        uint32_t now = mono_ms();
        service_audio(now);
        send_rtcp_reports(now);
        abr_update(fd, &abr, now);

        struct iav_bits_info_ex bi;
        memset(&bi, 0, sizeof(bi));
        int r = ioctl(fd, IAV_IOC_READ_BITSTREAM_EX, &bi);
        if (r < 0) {
            if (errno == EINTR) continue;
            usleep(5000);
            continue;
        }

        uint32_t sz = (bi.flags_size >> 8) & 0x7fffff;
        if (bi.start_addr && sz > 0 && sz < 2000000) {
            broadcast_annexb_frame((const uint8_t *)(uintptr_t)bi.start_addr, (int)sz, bi.pts);
        }

        frame_cnt++;
        bool update_ae = ae.enabled && (frame_cnt % 10 == 0);
        bool update_motion = preview_ready && frame_cnt % 5 == 0;
        if (update_ae || update_motion || (preview_ready && g_snap_req))
            sample_preview(fd, &ae, &nv, &motion, update_ae, update_motion);
    }

    fprintf(stderr, "[nest_rtsp] shutting down...\n");
    nv_shutdown(&nv);
    for (int i = 0; i < MAX_CLIENTS; i++) client_close(&g_clients[i]);
    audio_close();
    unlink(NEST_AUDIO_STATUS);
    unlink(NEST_MOTION_PATH);
    close(srv_fd);
    ioctl(fd, IAV_IOC_STOP_ENCODE_EX, 1);
    ioctl(fd, IAV_IOC_ENTER_IDLE, 0);
    close(fd);
    return 0;
}
