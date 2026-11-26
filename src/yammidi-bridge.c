/* yammidi-bridge.c — vendor-class USB-MIDI <-> CoreMIDI bridge for macOS.
 *
 * v0.7 release build: bidirectional, SysEx, descriptor discovery.
 *
 * The device is discovered from its USB descriptors, not hardcoded. The
 * program looks for an interface with class 0xFF / subclass 3 — the habit
 * Yamaha and Steinberg share, where standard USB-MIDI streaming is hidden
 * behind a vendor-specific interface — carrying a bulk IN endpoint and,
 * preferably, a bulk OUT partner. Interface number, endpoint addresses and
 * wMaxPacketSize all come from the descriptor.
 *
 * Verified on: Yamaha MX61 (0499:1508), macOS 27 arm64. The device reports the
 * shared product string "MX49/MX61" — one PID covers both models.
 *
 * Features
 *   IN   USB-MIDI 1.0 event packets -> CoreMIDI virtual source
 *   OUT  CoreMIDI virtual destination -> USB-MIDI packets, writer thread
 *   SysEx is reassembled in both directions (CIN 0x4 / 0x5 / 0x6 / 0x7)
 *   Cable number honoured: one source/destination pair per cable 0..4
 *   Re-attaches after replug; re-opens on transfer error
 *
 * Measurement that must not be "improved":
 *   Read exactly wMaxPacketSize bytes (32 on the MX61). Requesting more makes
 *   libusb wait for a full buffer the device never sends, so every read blocks
 *   until the timeout. Measured on MX61: 1024B -> 0/19 clean at 319ms; 32B ->
 *   31/31 clean. Always process bytes returned together with
 *   LIBUSB_ERROR_TIMEOUT — macOS reports partial transfers that way.
 *
 * Build:  make
 * Test:   ./yammidi-bridge-v7 --selftest    codec only, no hardware
 * Probe:  ./yammidi-bridge-v7 --probe       describe a matching device
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <libusb-1.0/libusb.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

/* Discovery is the default. Set these non-zero to pin one specific device. */
#define FORCE_VID    0x0000
#define FORCE_PID    0x0000

#define RETRY_S      3
#define HEART_S      300
#define READ_TIMEOUT 200
#define WRITE_TIMEOUT 20
#define MAX_CABLES   5
#define OUT_RING     65536
#define PL_BUF_SIZE  8192
#define SYSEX_MAX    32768
#define MAX_READ     4096
#define DEFAULT_READ 64
#define NAME_MAX_    128

/* Number of valid MIDI bytes per USB-MIDI CIN nibble (0 = reserved, skip). */
static const unsigned char cin_len[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

static volatile sig_atomic_t stop = 0;
static int g_selftest = 0;

/* ---------------- descriptor discovery ---------------- */

typedef struct {
    int           matched;
    unsigned short vid, pid;
    int           ifnum, altsetting;
    int           if_class, if_subclass, if_protocol;
    unsigned char ep_in, ep_out;
    int           ep_in_maxpkt, ep_out_maxpkt;
    char          product[NAME_MAX_];
    char          manufacturer[NAME_MAX_];
} iface_info_t;

static unsigned char g_ep_in = 0, g_ep_out = 0;
static int  g_read_size = DEFAULT_READ;
static char g_name[NAME_MAX_] = "USB MIDI";
static int  g_name_forced = 0;

/* Yamaha and Steinberg both report <class 0xFF, subclass 3> on interfaces
 * that are standard USB-MIDI underneath. Linux carries quirks for exactly
 * this (sound/usb/quirks-table.h, Steinberg MI2/MI4, the Yamaha catch-all). */
static int is_midi_candidate(int cls, int sub) {
    return (cls == 0xFF && sub == 3);
}

/* Request exactly one max-packet: see the measurement note at the top. */
static int read_size_for(int maxpkt) {
    int n = (maxpkt > 0) ? maxpkt : DEFAULT_READ;
    if (n < 4) n = 4;
    if (n > MAX_READ) n = MAX_READ;
    return n - (n % 4);
}

static void first_word(const char *s, char *out, size_t cap) {
    size_t i = 0;
    while (s[i] && !isspace((unsigned char)s[i]) && i + 1 < cap) {
        out[i] = s[i];
        i++;
    }
    out[i] = '\0';
}

static void derive_name(const iface_info_t *in, char *dst, size_t cap) {
    if (in->product[0]) {
        /* "Yamaha Corporation" + "Yamaha MX49/MX61" -> "Yamaha MX49/MX61".
         * Without this the port reads "Yamaha Corporation Yamaha MX49/MX61",
         * which is technically accurate and practically absurd. */
        int dup = 0;
        if (in->manufacturer[0]) {
            char mw[32], pw[32];
            first_word(in->manufacturer, mw, sizeof(mw));
            first_word(in->product, pw, sizeof(pw));
            dup = (mw[0] && pw[0] && strcasecmp(mw, pw) == 0);
        }
        if (in->manufacturer[0] && !dup)
            snprintf(dst, cap, "%s %s", in->manufacturer, in->product);
        else
            snprintf(dst, cap, "%s", in->product);
    } else if (in->manufacturer[0]) {
        snprintf(dst, cap, "%s", in->manufacturer);
    } else {
        snprintf(dst, cap, "USB MIDI %04X:%04X", in->vid, in->pid);
    }
}

/* Walk the active configuration. Fills `out` and returns 1 on a match.
 * Prefers an interface that has both a bulk IN and a bulk OUT. */
static int discover(libusb_device *dev, iface_info_t *out) {
    struct libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(dev, &dd) < 0) return 0;

    memset(out, 0, sizeof(*out));
    out->vid = dd.idVendor;
    out->pid = dd.idProduct;
    out->ifnum = -1;

    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(dev, &cfg) < 0) {
        if (libusb_get_config_descriptor(dev, 0, &cfg) < 0) return 0;
    }

    int best = -1;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *id = &itf->altsetting[a];
            if (!is_midi_candidate(id->bInterfaceClass, id->bInterfaceSubClass)) continue;

            int epin = -1, epout = -1, inmax = 0, outmax = 0;
            for (int e = 0; e < id->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
                if ((ep->bmAttributes & 0x03) != 0x02) continue;   /* bulk only */
                if (ep->bEndpointAddress & 0x80) {
                    if (epin < 0) {
                        epin = ep->bEndpointAddress;
                        inmax = ep->wMaxPacketSize & 0x7FF;
                    }
                } else if (epout < 0) {
                    epout = ep->bEndpointAddress;
                    outmax = ep->wMaxPacketSize & 0x7FF;
                }
            }
            if (epin < 0) continue;

            int score = (epout >= 0 ? 20 : 0) + id->bNumEndpoints;
            if (score > best) {
                best = score;
                out->ifnum = id->bInterfaceNumber;
                out->altsetting = id->bAlternateSetting;
                out->if_class = id->bInterfaceClass;
                out->if_subclass = id->bInterfaceSubClass;
                out->if_protocol = id->bInterfaceProtocol;
                out->ep_in = (unsigned char)epin;
                out->ep_out = (unsigned char)(epout >= 0 ? epout : 0);
                out->ep_in_maxpkt = inmax;
                out->ep_out_maxpkt = outmax;
                out->matched = 1;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    if (!out->matched) return 0;

    libusb_device_handle *h = NULL;
    if (libusb_open(dev, &h) == 0) {
        if (dd.iProduct)
            libusb_get_string_descriptor_ascii(h, dd.iProduct,
                                               (unsigned char *)out->product,
                                               sizeof(out->product));
        if (dd.iManufacturer)
            libusb_get_string_descriptor_ascii(h, dd.iManufacturer,
                                               (unsigned char *)out->manufacturer,
                                               sizeof(out->manufacturer));
        libusb_close(h);
    }
    return 1;
}

/* ---------------- capture buffer (selftest only) ---------------- */

static char cap[8192];
static size_t caplen = 0;

static void cap_reset(void) { cap[0] = '\0'; caplen = 0; }

static void cap_add(const char *fmt, ...) {
    if (caplen + 1 >= sizeof(cap)) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cap + caplen, sizeof(cap) - caplen, fmt, ap);
    va_end(ap);
    if (n > 0) caplen += (size_t)n;
}

/* ---------------- shared USB handle ---------------- */

static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;
static libusb_device_handle *g_h = NULL;

/* ---------------- OUT ring buffer ---------------- */

static pthread_mutex_t rmux = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  rcv  = PTHREAD_COND_INITIALIZER;
static unsigned char rbuf[OUT_RING];
static size_t rhead = 0, rtail = 0, rlen = 0;
static unsigned long g_dropped = 0;
static unsigned long g_out_pkts = 0;
static unsigned long g_panics = 0;

static int ring_push(const unsigned char *p, size_t n) {
    int ok = 1;
    pthread_mutex_lock(&rmux);
    for (size_t i = 0; i < n; i++) {
        if (rlen >= OUT_RING) { ok = 0; g_dropped++; break; }
        rbuf[rtail] = p[i];
        rtail = (rtail + 1) % OUT_RING;
        rlen++;
    }
    pthread_cond_signal(&rcv);
    pthread_mutex_unlock(&rmux);
    return ok;
}

static void ring_reset(void) {
    pthread_mutex_lock(&rmux);
    rhead = rtail = rlen = 0;
    pthread_mutex_unlock(&rmux);
}

/* ---------------- USB-MIDI encoder (CoreMIDI -> EP 0x03) ---------------- */

typedef struct {
    unsigned char st;       /* current status byte, 0 = idle */
    int need;               /* bytes in this message including status */
    int npend;
    unsigned char pend[3];
    int sysex;
    unsigned char cin;      /* CIN for the message being assembled */
} enc_t;

static enc_t g_enc[MAX_CABLES];
static pthread_mutex_t enc_lock = PTHREAD_MUTEX_INITIALIZER;

static int emit_pkt(unsigned char cable, unsigned char cin, const unsigned char *b) {
    unsigned char p[4];
    p[0] = (unsigned char)(((cable & 0x0F) << 4) | (cin & 0x0F));
    p[1] = b[0]; p[2] = b[1]; p[3] = b[2];
    if (g_selftest) {
        cap_add("%02X %02X %02X %02X\n", p[0], p[1], p[2], p[3]);
        return 0;
    }
    return ring_push(p, 4);
}

static void enc_feed(int cable, unsigned char byte) {
    enc_t *e = &g_enc[cable];
    unsigned char b[3] = { 0, 0, 0 };

    /* Real-time bytes may appear anywhere, including mid-SysEx. Close any
     * partially filled group first so byte order is preserved, then send the
     * real-time byte in its own packet (CIN 0xF). */
    if (byte >= 0xF8) {
        if (e->sysex && e->npend > 0) {
            b[0] = e->pend[0];
            b[1] = (e->npend > 1) ? e->pend[1] : 0;
            b[2] = (e->npend > 2) ? e->pend[2] : 0;
            emit_pkt((unsigned char)cable, 0x4, b);
            e->npend = 0;
        }
        b[0] = byte; b[1] = 0; b[2] = 0;
        emit_pkt((unsigned char)cable, 0x0F, b);
        return;
    }

    if (e->sysex) {
        e->pend[e->npend++] = byte;
        if (byte == 0xF7) {
            unsigned char cin = (unsigned char)(0x4 + e->npend);  /* 0x5/0x6/0x7 */
            for (int i = 0; i < 3; i++) b[i] = (i < e->npend) ? e->pend[i] : 0;
            emit_pkt((unsigned char)cable, cin, b);
            e->npend = 0; e->sysex = 0; e->st = 0;
        } else if (e->npend == 3) {
            for (int i = 0; i < 3; i++) b[i] = e->pend[i];
            emit_pkt((unsigned char)cable, 0x4, b);
            e->npend = 0;
        }
        return;
    }

    if (byte >= 0x80) {
        if (byte < 0xF0) {                          /* channel message */
            e->st = byte;
            e->npend = 1;
            e->pend[0] = byte;
            e->need = ((byte & 0xF0) == 0xC0 || (byte & 0xF0) == 0xD0) ? 2 : 3;
            e->cin = (unsigned char)(byte >> 4);     /* 0x8..0xE, matches spec */
        } else if (byte == 0xF0) {                   /* SysEx start */
            e->sysex = 1; e->npend = 1; e->pend[0] = 0xF0; e->st = byte;
            return;
        } else if (byte == 0xF1 || byte == 0xF3) {    /* 2-byte system common */
            e->st = byte; e->npend = 1; e->pend[0] = byte; e->need = 2; e->cin = 0x2;
        } else if (byte == 0xF2) {                   /* 3-byte system common */
            e->st = byte; e->npend = 1; e->pend[0] = byte; e->need = 3; e->cin = 0x3;
        } else if (byte == 0xF6) {                   /* tune request, 1 byte */
            b[0] = byte;
            emit_pkt((unsigned char)cable, 0x5, b);
            e->st = 0;                               /* system common kills running status */
            return;
        } else {
            return;                                  /* F4/F5/F7 orphan: drop */
        }
        if (e->npend == e->need) {
            for (int i = 0; i < 3; i++) b[i] = (i < e->npend) ? e->pend[i] : 0;
            emit_pkt((unsigned char)cable, e->cin, b);
            e->npend = 0;
            /* Channel messages set running status; system common clears it. */
            if (e->cin < 0x8) e->st = 0;
        }
        return;
    }

    /* data byte (possibly running status) */
    if (e->npend == 0) {
        if (e->st == 0 || e->st >= 0xF0) return;     /* stray data, drop */
        e->pend[0] = e->st;
        e->npend = 1;
    }
    e->pend[e->npend++] = byte;
    if (e->npend == e->need) {
        for (int i = 0; i < 3; i++) b[i] = (i < e->npend) ? e->pend[i] : 0;
        emit_pkt((unsigned char)cable, e->cin, b);
        e->npend = 0;                                /* st survives: running status */
    }
}

/* ---------------- CoreMIDI channels ---------------- */

typedef struct {
    MIDIEndpointRef src, dst;
    int ready;
    unsigned char syx[SYSEX_MAX];
    size_t sylen;
    unsigned char plbuf[PL_BUF_SIZE];
    MIDIPacketList *pl;
    MIDIPacket *cur;
    int any;
} chan_t;

static chan_t g_ch[MAX_CABLES];
static MIDIClientRef g_client = 0;

/* Called when the device goes away. The NoteOffs that were never sent leave
 * notes held in every listening app, which in a dense arrangement is a stuck
 * drone under everything. CC 120 (All Sound Off) then CC 123 (All Notes Off)
 * on all 16 channels is the standard panic, and 32 events costs nothing. */
static void panic_all_sources(void) {
    for (int i = 0; i < MAX_CABLES; i++) {
        chan_t *c = &g_ch[i];
        if (!c->ready) continue;

        if (g_selftest) {
            for (int ch = 0; ch < 16; ch++)
                cap_add("panic c%d %02X %02X 00\n", i, 0xB0 | ch, 120);
            for (int ch = 0; ch < 16; ch++)
                cap_add("panic c%d %02X %02X 00\n", i, 0xB0 | ch, 123);
            continue;
        }
        if (!c->src) continue;

        static unsigned char pbuf[1024];      /* reader thread only */
        MIDIPacketList *pl = (MIDIPacketList *)pbuf;
        MIDIPacket *p = MIDIPacketListInit(pl);
        int ok = 1;
        for (int ch = 0; ch < 16 && ok; ch++) {
            unsigned char sound_off[3] = { (unsigned char)(0xB0 | ch), 120, 0 };
            unsigned char notes_off[3] = { (unsigned char)(0xB0 | ch), 123, 0 };
            p = MIDIPacketListAdd(pl, (ByteCount)sizeof(pbuf), p, 0, 3, sound_off);
            if (!p) { ok = 0; break; }
            p = MIDIPacketListAdd(pl, (ByteCount)sizeof(pbuf), p, 0, 3, notes_off);
            if (!p) { ok = 0; break; }
        }
        if (ok) MIDIReceived(c->src, pl);
        else    printf("warning: panic packet list overflowed on cable %d\n", i);
        g_panics++;
    }
    if (!g_selftest && g_panics)
        printf("panic: all-notes-off sent to %lu source(s)\n", g_panics);
}

static void out_read_proc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon);

static void reset_pl(chan_t *c) {
    c->pl = (MIDIPacketList *)c->plbuf;
    c->cur = MIDIPacketListInit(c->pl);
    c->any = 0;
}

static chan_t *ensure_chan(int cable) {
    chan_t *c = &g_ch[cable];
    if (c->ready) return c;

    if (!g_selftest) {
        char inbuf[NAME_MAX_ + 24], outbuf[NAME_MAX_ + 24];
        if (cable == 0) {
            snprintf(inbuf, sizeof(inbuf), "%s", g_name);
            snprintf(outbuf, sizeof(outbuf), "%s Out", g_name);
        } else {
            snprintf(inbuf, sizeof(inbuf), "%s Port %d", g_name, cable + 1);
            snprintf(outbuf, sizeof(outbuf), "%s Out Port %d", g_name, cable + 1);
        }
        CFStringRef nm = CFStringCreateWithCString(NULL, inbuf, kCFStringEncodingUTF8);
        if (!nm || MIDISourceCreate(g_client, nm, &c->src) != noErr) {
            if (nm) CFRelease(nm);
            return NULL;
        }
        CFRelease(nm);
        nm = CFStringCreateWithCString(NULL, outbuf, kCFStringEncodingUTF8);
        if (!nm || MIDIDestinationCreate(g_client, nm, out_read_proc,
                                         (void *)(intptr_t)cable, &c->dst) != noErr) {
            c->dst = 0;   /* source survives; OUT unavailable on this cable */
        }
        if (nm) CFRelease(nm);
        printf("published cable %d: \"%s\"%s\n", cable, inbuf,
               c->dst ? " + destination" : " (no OUT)");
    }

    reset_pl(c);
    c->sylen = 0;
    c->ready = 1;
    return c;
}

static void add_event(chan_t *c, int cable, const unsigned char *m, int n) {    if (g_selftest) {
        cap_add("c%d len=%d", cable, n);
        for (int i = 0; i < n && i < 16; i++) cap_add(" %02X", m[i]);
        cap_add("\n");
        return;
    }
    MIDIPacket *np = MIDIPacketListAdd(c->pl, PL_BUF_SIZE, c->cur, 0, n, m);
    if (np) { c->cur = np; c->any = 1; }
}

static void pl_flush(chan_t *c) {
    if (!c->any) return;
    if (!g_selftest && c->src) MIDIReceived(c->src, c->pl);
    reset_pl(c);
}

static void sysex_flush(chan_t *c, int cable) {
    if (!c->sylen) return;
    if (g_selftest) {
        cap_add("c%d sysex len=%zu", cable, c->sylen);
        for (size_t i = 0; i < c->sylen && i < 16; i++) cap_add(" %02X", c->syx[i]);
        cap_add("\n");
        c->sylen = 0;
        return;
    }
    {
        static unsigned char bufp[SYSEX_MAX + 512];   /* reader thread only */
        MIDIPacketList *pl = (MIDIPacketList *)bufp;
        MIDIPacket *p = MIDIPacketListInit(pl);
        p = MIDIPacketListAdd(pl, sizeof(bufp), p, 0, (UInt32)c->sylen, c->syx);
        if (p) MIDIReceived(c->src, pl);
    }
    c->sylen = 0;
}

/* CoreMIDI hands us bytes destined for the MX; queue them for the writer. */
static void out_read_proc(const MIDIPacketList *pktlist, void *refCon, void *connRefCon) {
    (void)refCon;
    int cable = (int)(intptr_t)connRefCon;
    if (cable < 0 || cable >= MAX_CABLES) return;

    pthread_mutex_lock(&enc_lock);
    const MIDIPacket *p = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; i++) {
        for (UInt16 k = 0; k < p->length; k++) enc_feed(cable, p->data[k]);
        p = MIDIPacketNext(p);
    }
    pthread_mutex_unlock(&enc_lock);
}

/* ---------------- USB-MIDI decoder (EP 0x84 -> CoreMIDI) ---------------- */

static void decode_chunk(const unsigned char *buf, int got) {
    for (int i = 0; i + 3 < got; i += 4) {
        unsigned char b0 = buf[i], b1 = buf[i+1], b2 = buf[i+2], b3 = buf[i+3];
        if (!b0 && !b1 && !b2 && !b3) continue;          /* padding */
        /* Byte 0 of a USB-MIDI event packet: high nibble = cable number,
         * low nibble = CIN. (Linux sound/usb/midi.c does the same.) */
        int cable = b0 >> 4;
        int cin = b0 & 0x0F;
        int n = cin_len[cin];
        if (!n || cable >= MAX_CABLES) continue;

        chan_t *c = ensure_chan(cable);
        if (!c) continue;

        const unsigned char m[3] = { b1, b2, b3 };

        if (cin >= 0x4 && cin <= 0x7) {                   /* SysEx */
            if (c->sylen == 0 && m[0] != 0xF0) continue;  /* orphan continuation */
            if (m[0] == 0xF0) c->sylen = 0;
            int done = 0, overflow = 0;
            for (int k = 0; k < n; k++) {
                if (c->sylen >= SYSEX_MAX) { overflow = 1; break; }
                c->syx[c->sylen++] = m[k];
                if (m[k] == 0xF7) { done = 1; break; }
            }
            if (overflow) {
                printf("sysex overflow on cable %d, %zu bytes dropped\n", cable, c->sylen);
                c->sylen = 0;
            } else if (done) {
                sysex_flush(c, cable);
            }
        } else {
            if (c->sylen) {                               /* unterminated, abandon */
                printf("unterminated sysex on cable %d (%zu bytes), dropped\n", cable, c->sylen);
                c->sylen = 0;
            }
            add_event(c, cable, m, n);
        }
    }
    for (int i = 0; i < MAX_CABLES; i++) pl_flush(&g_ch[i]);
}

/* ---------------- selftest entry (defined at end of file) ---------------- */

static int selftest_run(void);

/* ---------------- OUT writer thread ---------------- */

static void *out_writer(void *arg) {
    (void)arg;
    unsigned char chunk[32];

    for (;;) {
        pthread_mutex_lock(&rmux);
        while (rlen < 4 && !stop) pthread_cond_wait(&rcv, &rmux);
        if (stop && rlen < 4) { pthread_mutex_unlock(&rmux); break; }

        size_t n = 0;
        while (rlen >= 4 && n + 4 <= sizeof(chunk)) {
            for (int k = 0; k < 4; k++) {
                chunk[n++] = rbuf[rhead];
                rhead = (rhead + 1) % OUT_RING;
                rlen--;
            }
        }
        pthread_mutex_unlock(&rmux);

        pthread_mutex_lock(&hlock);
        if (g_h && g_ep_out) {
            int sent = 0;
            int rc = libusb_bulk_transfer(g_h, g_ep_out, chunk, (int)n, &sent, WRITE_TIMEOUT);
            if (rc < 0 && rc != LIBUSB_ERROR_TIMEOUT && rc != LIBUSB_ERROR_NO_DEVICE) {
                printf("EP_OUT error: %s (sent=%d/%zu)\n", libusb_error_name(rc), sent, n);
                libusb_clear_halt(g_h, g_ep_out);
            } else if (sent > 0) {
                g_out_pkts += (unsigned long)(sent / 4);
            }
        }
        pthread_mutex_unlock(&hlock);
    }
    return NULL;
}

/* ---------------- attach / read loop ---------------- */

static void onsig(int s) { (void)s; stop = 1; pthread_cond_broadcast(&rcv); }

static libusb_device_handle *attach_device(libusb_device **list, ssize_t n,
                                           iface_info_t *out, int *out_rc) {
    *out_rc = -1;
    for (ssize_t i = 0; i < n; i++) {
        iface_info_t cand;
        if (!discover(list[i], &cand)) continue;
        if (FORCE_VID && (cand.vid != FORCE_VID || cand.pid != FORCE_PID)) continue;

        libusb_device_handle *h = NULL;
        int rc = libusb_open(list[i], &h);
        if (rc < 0) { *out_rc = rc; continue; }

        rc = libusb_claim_interface(h, cand.ifnum);
        if (rc < 0) { libusb_close(h); *out_rc = rc; continue; }

        int r1 = libusb_clear_halt(h, cand.ep_in);
        int r2 = cand.ep_out ? libusb_clear_halt(h, cand.ep_out) : 0;

        g_ep_in = cand.ep_in;
        g_ep_out = cand.ep_out;
        g_read_size = read_size_for(cand.ep_in_maxpkt);
        *out = cand;
        *out_rc = 0;

        printf("attached: %04X:%04X %s\n", cand.vid, cand.pid,
               cand.product[0] ? cand.product : "(unnamed)");
        printf("  interface %d alt %d, class %02X/%d/%d, read %d B\n",
               cand.ifnum, cand.altsetting, cand.if_class,
               cand.if_subclass, cand.if_protocol, g_read_size);
        printf("  clear_halt IN %#04x=%s  OUT %#04x=%s\n",
               cand.ep_in, r1 == 0 ? "ok" : libusb_error_name(r1),
               cand.ep_out, !cand.ep_out ? "n/a"
                                         : (r2 == 0 ? "ok" : libusb_error_name(r2)));
        return h;
    }
    return NULL;
}

static int probe_mode(libusb_context *ctx) {
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(ctx, &list);
    if (n < 0) { fprintf(stderr, "device enumeration failed\n"); return 1; }

    printf("scanned %zd USB device(s)\n\n", n);
    int found = 0;
    for (ssize_t i = 0; i < n; i++) {
        iface_info_t in;
        if (!discover(list[i], &in)) continue;
        if (FORCE_VID && (in.vid != FORCE_VID || in.pid != FORCE_PID)) continue;
        found++;
        printf("%04X:%04X  %s\n", in.vid, in.pid,
               in.product[0] ? in.product : "(no product string)");
        if (in.manufacturer[0])
            printf("          manufacturer  %s\n", in.manufacturer);
        printf("          interface     %d (alt %d)\n", in.ifnum, in.altsetting);
        printf("          class         %02X, subclass %d, protocol %d\n",
               in.if_class, in.if_subclass, in.if_protocol);
        printf("          EP_IN         %#04x bulk, wMaxPacketSize %d\n",
               in.ep_in, in.ep_in_maxpkt);
        if (in.ep_out)
            printf("          EP_OUT        %#04x bulk, wMaxPacketSize %d\n",
                   in.ep_out, in.ep_out_maxpkt);
        else
            printf("          EP_OUT        none (input-only device)\n");
        printf("\n");
    }
    libusb_free_device_list(list, 1);

    if (!found) {
        printf("no vendor-class MIDI interface (0xFF/3) with a bulk IN endpoint found.\n");
        return 2;
    }
    printf("%d matching device(s).\n", found);
    return 0;
}

int main(int argc, char **argv) {
    int secs = 0, want_probe = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) g_selftest = 1;
        else if (!strcmp(argv[i], "--probe")) want_probe = 1;
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) {
            snprintf(g_name, sizeof(g_name), "%s", argv[++i]);
            g_name_forced = 1;
        }
        else if (argv[i][0] != '-') secs = atoi(argv[i]);
    }

    setvbuf(stdout, NULL, _IOLBF, 0);

    if (g_selftest) return selftest_run();

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    if (want_probe) {
        int prc = probe_mode(ctx);
        libusb_exit(ctx);
        return prc;
    }

    signal(SIGINT, onsig);
    signal(SIGTERM, onsig);

    pthread_t wr;
    int writer_up = 0, client_up = 0;

    unsigned long nmsg = 0, nreal = 0, shown = 0, nread = 0;
    unsigned char ls0 = 0, ls1 = 0, ls2 = 0;
    time_t t0 = time(NULL), lastBeat = time(NULL), lastLine = 0;
    unsigned char *buf = NULL;

    while (!stop && (secs == 0 || (time(NULL) - t0) < secs)) {
        libusb_device **list = NULL;
        ssize_t n = libusb_get_device_list(ctx, &list);
        iface_info_t info;
        memset(&info, 0, sizeof(info));
        int rc = -1;
        libusb_device_handle *h = (n > 0) ? attach_device(list, n, &info, &rc) : NULL;

        if (!h) {
            if (rc != -1) printf("claim failed (%s), retrying in %ds\n", libusb_error_name(rc), RETRY_S);
            else          printf("no matching device, waiting...\n");
            if (list) libusb_free_device_list(list, 1);
            for (int s = 0; s < RETRY_S && !stop; s++) sleep(1);
            continue;
        }

        /* Ports are published once a device is actually present, so the names
         * come from its string descriptors instead of a compile-time guess. */
        if (!client_up) {
            if (!g_name_forced) derive_name(&info, g_name, sizeof(g_name));
            if (MIDIClientCreate(CFSTR("YamahaMX61Bridge"), NULL, NULL, &g_client) != noErr) {
                fprintf(stderr, "MIDIClientCreate failed\n");
                return 4;
            }
            if (!ensure_chan(0)) {
                fprintf(stderr, "could not publish cable 0\n");
                return 5;
            }
            if (pthread_create(&wr, NULL, out_writer, NULL) != 0) {
                fprintf(stderr, "writer thread failed\n");
                return 6;
            }
            writer_up = 1;
            client_up = 1;
        }

        if (!buf) {
            buf = malloc((size_t)g_read_size);
            if (!buf) { fprintf(stderr, "out of memory\n"); return 7; }
        }

        pthread_mutex_lock(&hlock);
        g_h = h;
        pthread_mutex_unlock(&hlock);

        while (!stop && (secs == 0 || (time(NULL) - t0) < secs)) {
            int got = 0;
            rc = libusb_bulk_transfer(h, g_ep_in, buf, g_read_size, &got, READ_TIMEOUT);

            if (got > 0) {
                nread++;
                if (got % 4) printf("warning: short packet (%d bytes, not 4-aligned)\n", got);
                for (int i = 0; i + 3 < got; i += 4) {
                    if (!buf[i]) continue;
                    if ((buf[i] & 0x0F) < 0x4 || (buf[i] & 0x0F) > 0x7) {
                        if (buf[i+1] != 0xFE && buf[i+1] != 0xF8) {
                            ls0 = buf[i+1]; ls1 = buf[i+2]; ls2 = buf[i+3];
                            nreal++;
                        }
                    }
                    nmsg++;
                }
                decode_chunk(buf, got);
                lastBeat = time(NULL);
                /* Log only on new musical events, once a second at most.
                 * ActiveSense arrives ~3x/s; printing per read would grow the
                 * file without bound, since launchd does not rotate it. */
                if (nreal != shown && time(NULL) != lastLine) {
                    printf("in: events=%lu real=%lu reads=%lu out=%lu dropped=%lu last=%02x %02x %02x\n",
                           nmsg, nreal, nread, g_out_pkts, g_dropped, ls0, ls1, ls2);
                    lastLine = time(NULL);
                    shown = nreal;
                }
            } else if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) {
                if (time(NULL) - lastBeat >= HEART_S) {
                    printf("idle: events=%lu real=%lu reads=%lu\n", nmsg, nreal, nread);
                    lastBeat = time(NULL);
                }
            } else if (rc == LIBUSB_ERROR_NO_DEVICE) {
                printf("device detached (events=%lu)\n", nmsg);
                break;
            } else {
                printf("bulk error: %s (events=%lu)\n", libusb_error_name(rc), nmsg);
                libusb_clear_halt(h, g_ep_in);
                break;
            }
        }

        /* The device is gone or wedged; release anything it left held. */
        panic_all_sources();

        pthread_mutex_lock(&hlock);
        g_h = NULL;
        libusb_release_interface(h, info.ifnum);
        libusb_close(h);
        pthread_mutex_unlock(&hlock);
        ring_reset();
        if (list) libusb_free_device_list(list, 1);
    }

    stop = 1;
    pthread_cond_broadcast(&rcv);
    if (writer_up) pthread_join(wr, NULL);

    printf("exit: events=%lu real=%lu reads=%lu out=%lu dropped=%lu panics=%lu\n",
           nmsg, nreal, nread, g_out_pkts, g_dropped, g_panics);
    for (int i = 0; i < MAX_CABLES; i++) {
        if (g_ch[i].ready) {
            if (g_ch[i].src) MIDIEndpointDispose(g_ch[i].src);
            if (g_ch[i].dst) MIDIEndpointDispose(g_ch[i].dst);
        }
    }
    if (client_up) MIDIClientDispose(g_client);
    free(buf);
    libusb_exit(ctx);
    return 0;
}

/* ---------------- selftest: codec both directions, no hardware ---------------- */

struct enc_case { const char *name; unsigned char in[12]; int n; const char *want; };

static const struct enc_case ENC_CASES[] = {
    { "note on",       {0x90,0x3C,0x64},            3, "09 90 3C 64\n" },
    { "note off",      {0x80,0x3C,0x40},            3, "08 80 3C 40\n" },
    { "program change",{0xC0,0x05},                 2, "0C C0 05 00\n" },
    { "channel aftertouch", {0xD0,0x33},            2, "0D D0 33 00\n" },
    { "pitch bend",    {0xE0,0x00,0x40},            3, "0E E0 00 40\n" },
    { "cc",            {0xB0,0x07,0x7F},            3, "0B B0 07 7F\n" },
    { "clock",         {0xF8},                      1, "0F F8 00 00\n" },
    { "start",         {0xFA},                      1, "0F FA 00 00\n" },
    { "tune request",  {0xF6},                      1, "05 F6 00 00\n" },
    { "song select",   {0xF3,0x02},                 2, "02 F3 02 00\n" },
    { "song position", {0xF2,0x10,0x20},            3, "03 F2 10 20\n" },
    { "sysex 6B",      {0xF0,0x7E,0x7F,0x06,0x01,0xF7}, 6,
      "04 F0 7E 7F\n07 06 01 F7\n" },
    { "sysex 4B",      {0xF0,0x43,0x10,0xF7},       4, "04 F0 43 10\n05 F7 00 00\n" },
    { "sysex 8B",      {0xF0,0x41,0x10,0x42,0x12,0x40,0x00,0xF7}, 8,
      "04 F0 41 10\n04 42 12 40\n06 00 F7 00\n" },
    { "realtime inside sysex", {0xF0,0x7E,0xF8,0x7F,0xF7}, 5,
      "04 F0 7E 00\n0F F8 00 00\n06 7F F7 00\n" },
    { "running status data",   {0x90,0x3C,0x64,0x3E,0x50}, 5,
      "09 90 3C 64\n09 90 3E 50\n" },
};

static int selftest_run(void) {
    int pass = 0, fail = 0;

    printf("== encoder (CoreMIDI bytes -> USB-MIDI packets) ==\n");
    for (size_t t = 0; t < sizeof(ENC_CASES) / sizeof(ENC_CASES[0]); t++) {
        const struct enc_case *tc = &ENC_CASES[t];
        memset(g_enc, 0, sizeof(g_enc));
        cap_reset();
        for (int i = 0; i < tc->n; i++) enc_feed(0, tc->in[i]);
        if (!strcmp(cap, tc->want)) { printf("PASS  %s\n", tc->name); pass++; }
        else {
            printf("FAIL  %s\n  want: %s  got:  %s", tc->name, tc->want, cap);
            fail++;
        }
    }

    printf("\n== decoder (USB-MIDI packets -> CoreMIDI bytes) ==\n");
    {
        unsigned char chunk_a[] = {
            0x09,0x90,0x3C,0x64,   /* cable0 note on */
            0x19,0x90,0x40,0x7F,   /* cable1 note on */
            0x1B,0xB0,0x07,0x7F,   /* cable1 cc */
            0x00,0x00,0x00,0x00,   /* padding */
        };
        memset(g_ch, 0, sizeof(g_ch));
        cap_reset();
        decode_chunk(chunk_a, (int)sizeof(chunk_a));
        const char *want_a = "c0 len=3 90 3C 64\nc1 len=3 90 40 7F\nc1 len=3 B0 07 7F\n";
        if (!strcmp(cap, want_a)) { printf("PASS  channel messages, two cables\n"); pass++; }
        else { printf("FAIL  channel messages, two cables\n  want:\n%s  got:\n%s", want_a, cap); fail++; }

        unsigned char chunk_b[] = {
            0x04,0xF0,0x7E,0x7F,   /* sysex start/continue */
            0x07,0x06,0x01,0xF7,   /* sysex end, 3 bytes */
        };
        memset(g_ch, 0, sizeof(g_ch));
        cap_reset();
        decode_chunk(chunk_b, (int)sizeof(chunk_b));
        const char *want_b = "c0 sysex len=6 F0 7E 7F 06 01 F7\n";
        if (!strcmp(cap, want_b)) { printf("PASS  sysex reassembly across packets\n"); pass++; }
        else { printf("FAIL  sysex reassembly across packets\n  want:\n%s  got:\n%s", want_b, cap); fail++; }

        /* SysEx interrupted by a channel message must be dropped, not merged. */
        memset(g_ch, 0, sizeof(g_ch));
        cap_reset();
        unsigned char chunk_c[] = { 0x04,0xF0,0x41,0x10, 0x09,0x90,0x3C,0x64 };
        decode_chunk(chunk_c, (int)sizeof(chunk_c));
        const char *want_c = "c0 len=3 90 3C 64\n";
        if (strstr(cap, want_c)) { printf("PASS  unterminated sysex discarded\n"); pass++; }
        else { printf("FAIL  unterminated sysex discarded\n  got:\n%s", cap); fail++; }
    }

    printf("\n== all-notes-off on detach ==\n");
    {
        memset(g_ch, 0, sizeof(g_ch));
        g_ch[0].ready = 1;              /* cable 0 published */
        g_ch[2].ready = 0;              /* cable 2 never published */
        cap_reset();
        panic_all_sources();

        int lines = 0;
        for (const char *q = cap; *q; q++) if (*q == '\n') lines++;
        int bad = 0;
        if (lines != 32) bad++;
        if (!strstr(cap, "panic c0 B0 78 00")) bad++;   /* ch0  CC120 */
        if (!strstr(cap, "panic c0 BF 78 00")) bad++;   /* ch15 CC120 */
        if (!strstr(cap, "panic c0 B0 7B 00")) bad++;   /* ch0  CC123 */
        if (!strstr(cap, "panic c0 BF 7B 00")) bad++;   /* ch15 CC123 */
        if (strstr(cap, "panic c2")) bad++;             /* unpublished cable */

        if (!bad) { printf("PASS  32 events to published cable only\n"); pass++; }
        else { printf("FAIL  all-notes-off (lines=%d)\n  got:\n%s", lines, cap); fail++; }
    }

    printf("\n%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
