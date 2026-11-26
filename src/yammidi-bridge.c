/* yammidi-bridge.c — Yamaha MX61 (0499:1508) USB-MIDI -> CoreMIDI virtual source.
 *
 * Release build (v0.6). Input only: reads the bulk IN endpoint and republishes
 * as a CoreMIDI virtual source. No OUT path, no SysEx reassembly.
 *
 * Behaviour:
 *   - clear_halt on both MIDI endpoints at attach (fixes wedged endpoint
 *     after an unclean kill of a previous reader)
 *   - retry loop when the device is absent, instead of exiting (so a USB
 *     replug is picked up automatically without restarting the service)
 *   - re-open on transfer error rather than dying
 *   - logs only when the counter CHANGES (no stale repeat spam)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>
#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#define VID      0x0499
#define PID      0x1508
#define MIDI_IF  3
#define EP_IN    0x84
#define EP_OUT   0x03
#define RETRY_S  3
#define HEART_S  300   /* quiet summary interval; ActiveSense is not logged */

/* The bulk IN endpoint reports wMaxPacketSize = 32. Requesting a larger
 * buffer makes libusb wait for the full length, which the device never
 * sends — every read then blocks until the timeout. Measured: 1024-byte
 * reads had 0/19 clean completions at 319ms each; 32-byte reads had 31/31
 * clean at 196ms. Always read exactly one max-packet. */
#define EP_READ_SIZE 32
#define READ_TIMEOUT 200

/* CoreMIDI packet list needs real backing storage; the stack struct only
 * has room for one small packet and silently rejects the rest. */
#define PL_BUF_SIZE  4096

static volatile sig_atomic_t stop = 0;
static void onsig(int s) { (void)s; stop = 1; }

static int msglen(unsigned char st) {
    if (st < 0x80) return 0;
    switch (st & 0xF0) {
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 3;
        case 0xC0: case 0xD0: return 2;
        case 0xF0:
            if (st == 0xF2) return 3;
            if (st == 0xF1 || st == 0xF3) return 2;
            return 1;
    }
    return 0;
}

static const char *sname(unsigned char st) {
    switch (st & 0xF0) {
        case 0x80: return "NoteOff";
        case 0x90: return "NoteOn";
        case 0xA0: return "PolyAT";
        case 0xB0: return "CC";
        case 0xC0: return "ProgChg";
        case 0xD0: return "ChanAT";
        case 0xE0: return "PitchBend";
    }
    switch (st) {
        case 0xF0: return "SysEx";
        case 0xFE: return "ActiveSense";
        case 0xFA: return "Start";
        case 0xFB: return "Continue";
        case 0xFC: return "Stop";
        case 0xF8: return "Clock";
    }
    return "System";
}

static libusb_device_handle *find_and_claim(libusb_context *ctx, libusb_device **list,
                                            ssize_t n, int *out_rc) {
    (void)ctx;
    libusb_device_handle *h = NULL;
    for (ssize_t i = 0; i < n; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) < 0) continue;
        if (dd.idVendor == VID && dd.idProduct == PID) {
            if (libusb_open(list[i], &h) < 0) h = NULL;
            break;
        }
    }
    if (!h) { *out_rc = -1; return NULL; }

    int rc = libusb_claim_interface(h, MIDI_IF);
    if (rc < 0) { libusb_close(h); *out_rc = rc; return NULL; }

    int r1 = libusb_clear_halt(h, EP_IN);
    int r2 = libusb_clear_halt(h, EP_OUT);
    printf("attached: claimed IF%d, clear_halt IN=%s OUT=%s\n",
           MIDI_IF, r1 == 0 ? "ok" : libusb_error_name(r1),
           r2 == 0 ? "ok" : libusb_error_name(r2));
    *out_rc = 0;
    return h;
}

int main(int argc, char **argv) {
    int secs = (argc > 1) ? atoi(argv[1]) : 0;   /* 0 = daemon */

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, onsig);
    signal(SIGTERM, onsig);

    libusb_context *ctx = NULL;
    if (libusb_init(&ctx) < 0) { fprintf(stderr, "libusb_init failed\n"); return 1; }

    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    if (MIDIClientCreate(CFSTR("YamahaMX61Bridge"), NULL, NULL, &client) != noErr) {
        fprintf(stderr, "MIDIClientCreate failed\n"); return 4;
    }
    if (MIDISourceCreate(client, CFSTR("Yamaha MX61"), &source) != noErr) {
        fprintf(stderr, "MIDISourceCreate failed\n"); return 5;
    }
    printf("bridge up: source 'Yamaha MX61' published\n");

    unsigned char buf[EP_READ_SIZE];
    unsigned long nmsg = 0, nraw = 0, nread = 0;
    unsigned char ls0 = 0, ls1 = 0, ls2 = 0;
    time_t t0 = time(NULL), lastBeat = time(NULL);

    while (!stop && (secs == 0 || (time(NULL) - t0) < secs)) {
        libusb_device **list = NULL;
        ssize_t n = libusb_get_device_list(ctx, &list);
        int rc = -1;
        libusb_device_handle *h = (n > 0) ? find_and_claim(ctx, list, n, &rc) : NULL;

        if (!h) {
            if (rc != -1) {
                printf("claim failed (%s), retrying in %ds\n", libusb_error_name(rc), RETRY_S);
            } else {
                printf("device not present, waiting...\n");
            }
            if (list) libusb_free_device_list(list, 1);
            for (int s = 0; s < RETRY_S && !stop; s++) sleep(1);
            continue;
        }

        /* attached — read until error or exit */
        while (!stop && (secs == 0 || (time(NULL) - t0) < secs)) {
            int got = 0;
            rc = libusb_bulk_transfer(h, EP_IN, buf, EP_READ_SIZE, &got, READ_TIMEOUT);

            /* IMPORTANT: on macOS a bulk read can return LIBUSB_ERROR_TIMEOUT
             * with a non-zero `got` — the bytes arrived but the transfer did
             * not complete inside the timeout window. Those bytes are real and
             * must be processed, otherwise dense bursts are silently dropped. */
            if (got > 0) {
                nread++;
                static unsigned char plbuf[PL_BUF_SIZE];
                MIDIPacketList *pl = (MIDIPacketList *)plbuf;
                MIDIPacket *p = MIDIPacketListInit(pl);
                int added = 0, interesting = 0;
                for (int i = 0; i + 3 < got; i += 4) {
                    unsigned char b0 = buf[i], b1 = buf[i+1], b2 = buf[i+2], b3 = buf[i+3];
                    nraw++;
                    if (!b0 && !b1 && !b2 && !b3) continue;
                    int len = msglen(b1);
                    if (len == 0) continue;
                    unsigned char m[3] = { b1, b2, b3 };
                    MIDIPacket *np = MIDIPacketListAdd(pl, PL_BUF_SIZE, p, 0, len, m);
                    if (!np) { p = MIDIPacketListInit(pl); break; }
                    p = np; added++; nmsg++;
                    /* ActiveSense (0xFE) and Clock (0xF8) fire constantly and
                     * are not worth a log line; they would grow the file
                     * without bound since launchd does not rotate it. */
                    if (b1 != 0xFE && b1 != 0xF8) {
                        ls0 = b1; ls1 = b2; ls2 = b3;
                        interesting = 1;
                    }
                }
                if (added > 0) {
                    MIDIReceived(source, pl);
                    if (interesting) {
                        printf("forwarded=%lu raw=%lu last=%s %02x %02x %02x\n",
                               nmsg, nraw, sname(ls0), ls0, ls1, ls2);
                        lastBeat = time(NULL);
                    }
                }
            } else if (rc == 0 || rc == LIBUSB_ERROR_TIMEOUT) {
                /* nothing this cycle — normal */
                if (time(NULL) - lastBeat >= HEART_S) {
                    printf("idle: forwarded=%lu raw=%lu (no key activity)\n", nmsg, nraw);
                    lastBeat = time(NULL);
                }
            } else if (rc == LIBUSB_ERROR_NO_DEVICE) {
                printf("device detached (forwarded=%lu)\n", nmsg);
                break;
            } else {
                printf("bulk error: %s (forwarded=%lu)\n", libusb_error_name(rc), nmsg);
                libusb_clear_halt(h, EP_IN);
                break;
            }
        }

        libusb_release_interface(h, MIDI_IF);
        libusb_close(h);
        libusb_free_device_list(list, 1);
    }

    printf("exit: forwarded=%lu raw=%lu reads=%lu\n", nmsg, nraw, nread);
    MIDIEndpointDispose(source);
    MIDIClientDispose(client);
    libusb_exit(ctx);
    return 0;
}
