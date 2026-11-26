# yammidi

A userspace CoreMIDI bridge for **deprecated Yamaha instruments** — obsolete,
end-of-life, discontinued, no-longer-supported gear that modern macOS refuses to
bind, because Yamaha's own driver stopped working and nobody at Yamaha is going
to fix it.

If you own a **deprecated Yamaha** keyboard, synth, or audio interface with MIDI
over USB, and it vanished from your DAW after a macOS upgrade, this is probably
the cheapest way back. No kernel extension, no system extension, no disabling
SIP, no security downgrade, no vendor installer.

**Status:** v0.7 — bidirectional, SysEx, automatic device discovery. Verified
against a real Yamaha MX61 on macOS 27 (arm64). See
[what was verified](#what-was-actually-verified).

---

## The problem, stated plainly

Yamaha's USB-MIDI driver stack is **deprecated in practice**: abandoned for a
long list of instruments that Yamaha still lists, and which people still buy
secondhand, but no longer supports. On current macOS:

- The **3.x kext** (`YamahaSteinbergUSBAudio.kext`) declares device
  personalities that no longer attach on recent kernels.
- The **4.x DriverKit extension** ships, on at least one current package, as a
  **malformed bundle** — no `Contents/` directory, `Info.plist` and the binary
  sitting at bundle root — so it fails to register and throws `errno 80` at
  every boot while changing nothing.
- Yamaha's download pages list these instruments as discontinued, with no driver
  build for current macOS.

So the instrument becomes **invisible, not broken**. It enumerates on the USB
bus, it is powered, it sends MIDI — but macOS binds no driver, because the MIDI
interface declares itself as **vendor-specific** (`class 0xFF`, subclass 3)
instead of USB-MIDI (`class 0x01`, subclass 3). macOS looks at that class byte,
sees a vendor device it has no driver for, and moves on.

Underneath that vendor-class interface, the device speaks **standard USB-MIDI
1.0**: 4-byte event packets on a bulk endpoint. It always did. The class byte is
the only thing standing between you and a working instrument.

This project finds that interface by walking the USB descriptors, claims it with
`libusb`, and republishes the stream as CoreMIDI ports in both directions, so
every DAW sees an ordinary MIDI input and output.

---

## Devices this applies to

The vendor-class habit is not unique to one model. It is a **pattern across
Yamaha's product line**, and it also covers Steinberg (same parent company),
whose MI2/MI4 interfaces are explicitly documented in the Linux kernel as
"compatible with the USB class audio spec, but the MIDI part ... reported as a
vendor specific interface".

Families that follow the pattern, from Yamaha's own product history and the
quirks the Linux kernel has accumulated for them:

| Family | Notes |
|---|---|
| **Yamaha MX** (MX49 / MX61 / MX88) | MX61 verified here. One USB PID covers MX49 and MX61 — the device reports the product string `MX49/MX61`. |
| **Yamaha MOTIF / MOX / MODX** | Long-lived workstation line, repeatedly affected by driver churn. |
| **Yamaha P-series, S90, CP stage pianos** | Same USB-MIDI-behind-vendor-class arrangement. |
| **Yamaha Clavinova / Tyros / Genos** | Home and arranger lines with USB-to-host MIDI. |
| **Yamaha MU / UX USB-MIDI interfaces** | Older outboard MIDI interfaces, driver-dependent. |
| **Steinberg UR / MI series** | MI2/MI4 carry a dedicated Linux quirk entry. |

### Honest status of that table

**Only the MX61 is verified.** Everything else in the table is where the
*pattern* applies and where reports are wanted — not a list of tested devices.

Because discovery is descriptor-based rather than hardcoded, a device probably
works if it presents the same shape: an interface with class `0xFF`, subclass 3,
and a bulk IN endpoint. "Probably" is doing real work in that sentence. The
cheap way to check without trusting this program is:

```sh
./yammidi-bridge --probe
```

It describes any candidate device — vendor ID, product string, interface number,
class triple, endpoint addresses and packet sizes — **without claiming
anything**, so it is safe to run while another instance is attached.

If you have one of these instruments, **please open an issue with your `--probe`
output and a log excerpt.** Measured facts get a device added to the table with a
"verified" note. Guesses do not.

---

## What was measured on the MX61

Recorded so the next person does not have to rediscover them. Every value below
came from the device's own descriptors:

| Property | Value |
|---|---|
| USB ID | `0499:1508` |
| Product string | `MX49/MX61` (one PID covers both models) |
| Manufacturer string | `Yamaha Corporation` |
| MIDI interface | interface **3**, alternate setting 0 |
| Interface class triple | `0xFF` / `3` / `0` (vendor-specific) |
| Bulk IN endpoint | `0x84`, `wMaxPacketSize` **32** |
| Bulk OUT endpoint | `0x03`, `wMaxPacketSize` **32** |
| Embedded jacks | 5 (USB-MIDI cable numbers 0–4) |
| Event format | standard 4-byte USB-MIDI 1.0 packets |

### The one measurement worth repeating

**Read exactly one max-packet, not a big buffer.** Requesting more bytes than
the endpoint ever sends makes the transfer wait for a full buffer that never
arrives, so every read blocks until the timeout expires. Measured on the MX61:

```
size=1024   reads=19   clean= 0   timeout+data=19   avg_read=319.5 ms
size= 256   reads=19   clean= 0   timeout+data=19   avg_read=318.8 ms
size=  64   reads=20   clean=10   timeout+data=10   avg_read=298.0 ms
size=  32   reads=31   clean=31   timeout+data= 0   avg_read=196.0 ms
```

The top three rows are a keyboard that feels laggy and drops notes under dense
playing. The bottom row is an instrument. v0.7 reads the size from
`wMaxPacketSize` at runtime, so this cannot silently regress on a device with
different packet sizes — but do not override it upward.

### The second trap

On macOS, `libusb_bulk_transfer` can return `LIBUSB_ERROR_TIMEOUT` **with a
non-zero byte count** — the bytes did arrive, the transfer just did not complete
inside the window. Discarding those byte counts silently drops exactly the dense
passages you most want. The code processes them.

A physical cable pull surfaced as `LIBUSB_ERROR_IO`, not
`LIBUSB_ERROR_NO_DEVICE`. Both paths are handled: panic, teardown, retry.

---

## What was actually verified

Not "should work" — observed, on hardware, with the numbers recorded:

| Path | Evidence |
|---|---|
| **Input** | 89 NoteOn events played on the instrument arrived at an independent CoreMIDI client |
| **Output** | Universal SysEx identity request written to `EP 0x03` produced a reply from the device: `F0 7E 7F 06 02 43 00 41 47 06 00 00 00 7F F7` |
| **SysEx, both directions** | The 15-byte reply arrived split across packets and was reassembled into one message |
| **Program change** | Bank select + PC sent; the instrument's display followed (`out` counter advanced by 9 packets for 3 changes) |
| **All-notes-off on detach** | Cable pulled mid-session: `panic: all-notes-off sent to 1 source(s)` |
| **Reattach** | Detach, three retry intervals, clean re-attach; MIDI ports not duplicated |
| **Discovery** | `--probe` identified interface 3, 32-byte packets, both endpoints, with no hardcoded values |
| **Static link** | `otool -L` shows system frameworks only; no Homebrew dependency to break on upgrade |
| **Selftest** | 20 assertions over both codecs plus the panic, no hardware required |

The identity reply is the strongest single piece: it required the instrument to
read what we wrote and answer, which proves the output path, the encoding, and
SysEx reassembly in one message.

**Not verified:** audible output. The MX61 has no built-in speakers and testing
here had no 1/4" connection, so the tone generator was never heard. Program
change on the display is the closest substitute. If you can hear yours, an issue
confirming it would be welcome.

---

## Install

Requirements: macOS, Xcode command line tools (`xcode-select --install`), and
libusb via Homebrew.

```sh
brew install libusb
git clone https://github.com/RPFiend/yammidi.git
cd yammidi
./install.sh
```

`install.sh` builds, runs the selftest, installs to
`~/Library/Application Support/YamahaMX61Bridge/`, writes a per-user
LaunchAgent at `~/Library/LaunchAgents/com.rpfiend.yamahamx61bridge.plist`, and
loads it. No `sudo` anywhere. It refuses to install if the selftest fails.

The bridge starts at login and survives replugs. It also survives the instrument
being absent at login: the log reads `no matching device, waiting...` and it
attaches when you plug in.

### Verify

```sh
tail -f ~/Library/Logs/yamahamx61bridge.log
```

A healthy attach looks like this:

```
attached: 0499:1508 Yamaha MX49/MX61
  interface 3 alt 0, class FF/3/0, read 32 B
  clear_halt IN 0x84=ok  OUT 0x03=ok
published cable 0: "Yamaha MX49/MX61" + destination
```

Then play. You should see `in: events=… real=… last=90 3C 64` lines, at most one
per second. The log only prints when a *new* musical event arrives, so silence
while idle is correct — the instrument sends an ActiveSense heartbeat three times
a second and logging that would grow the file without bound.

In your DAW, look for **`<product>`** as a MIDI input and **`<product> Out`** as a
MIDI output. On an MX61 that reads `Yamaha MX49/MX61`.

Override the name if your DAW has routing saved against something else:

```sh
./yammidi-bridge --name "Yamaha MX61"
```

### A note about Audio MIDI Setup

**The bridge will not appear in Audio MIDI Setup's MIDI Studio window.** That is
correct behaviour, not a bug. MIDI Studio enumerates *devices*
(`MIDIDeviceRef` objects with entity graphs); this publishes a *virtual source*,
which belongs to a client, not a device. macOS has nothing to draw.

The control case is on your own machine: GarageBand's "Virtual Out" is a virtual
source created with the same API, by Apple, and it does not appear in MIDI
Studio either. Look in your DAW's MIDI port list instead — that is where it
matters.

### Uninstall

```sh
launchctl bootout gui/$(id -u)/com.rpfiend.yamahamx61bridge
rm -rf "$HOME/Library/Application Support/YamahaMX61Bridge" \
       "$HOME/Library/LaunchAgents/com.rpfiend.yamahamx61bridge.plist"
```

---

## Adapting to another device

By default there is nothing to adapt: the interface number, endpoint addresses
and read size all come from the descriptors. Run `--probe`; if it finds your
device, try it.

Discovery deliberately matches only `class 0xFF` / subclass 3. Class-compliant
devices (`0x01`/3) are **not** claimed, and that is on purpose — macOS already
binds those, and hijacking a working device would be hostile.

If discovery picks the wrong interface on a device with several vendor-class
ones, pin it at build time:

```c
#define FORCE_VID 0x0499   /* 0 = discover */
#define FORCE_PID 0x1508
```

The instrument broadcasts a shared product string, so vendor and product ID may
identify a model family rather than one model. That is Yamaha's choice, not ours.

---

## Limitations

Stated plainly, because silent gaps are worse than documented ones:

- **No MIDI 2.0 / UMP.** MIDI 1.0 only. The instrument is MIDI 1.0 anyway.
- **SysEx is buffered to 32 KB.** Large bulk dumps (full voice banks) may exceed
  it; the buffer is cleared and a line is logged rather than corrupting state.
  Voice-editor-sized messages are comfortably inside the limit.
- **One process per device.** Only one process can claim the interface. Run the
  LaunchAgent or a foreground instance, never both.
- **No velocity curve or channel remap.** Events pass through unmodified. Your
  DAW does that better than we would.
- **Not signed or notarised.** Built locally from source on your machine, which
  is why this ships as source rather than a binary.
- **MIDI only.** This does not claim to replace anything Yamaha ships beyond
  MIDI. Firmware updates, the Voice Editor's fast bulk transport, and the USB
  audio Control Panel are outside its scope.

---

## Prior art and thanks

This is not a novel technique, and the fact that it works is owed to people who
documented the pattern first:

- The **Linux kernel** `sound/usb/quirks-table.h` — the Steinberg MI2/MI4 entry
  and the Yamaha catch-all both describe exactly this situation, and
  `sound/usb/midi.c` is the reference USB-MIDI stream implementation.
- The cluster of independent userspace CoreMIDI drivers written for other
  abandoned hardware in 2025–2026 — Roland UM-1X, Akai MPC Studio, Numark V7,
  Hercules DJ Control, Roland SC-D70. Same architecture, same conclusion, no
  coordination. That convergence is the best evidence the approach is right.

Every one of those hardcodes a single device. The obvious next step was to stop
doing that, which is what v0.7 does.

---

## Disclaimer

Independent project. Not affiliated with, endorsed by, or supported by Yamaha
Corporation or Steinberg Media Technologies. "Yamaha" and "Steinberg" are
trademarks of their respective owners and appear here only to describe hardware
compatibility.

The protocol details used here were derived from the device's own public USB
descriptors and from published kernel source — not from vendor binaries,
disassembly, or leaked documentation. No vendor code is redistributed.

This software is provided under the MIT licence, without warranty. It talks
directly to a USB device. If that sounds risky to you, it should: read the
source, it is one file.

---

## Licence

MIT. See [LICENSE](LICENSE).

The v0.6 input-only implementation remains available at tag `v0.6.0`.
