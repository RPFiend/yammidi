# yammidi

A userspace CoreMIDI bridge for **deprecated Yamaha instruments** — obsolete,
end-of-life, discontinued, no-longer-supported gear that modern macOS refuses to
bind, because Yamaha's own driver stopped working and nobody at Yamaha is going
to fix it.

If you own a **deprecated Yamaha** keyboard, synth, or audio interface with MIDI
over USB, and it vanished from your DAW after a macOS upgrade, this is probably
the cheapest way back. No kernel extension, no system extension, no disabling
SIP, no security downgrade, no vendor installer.

**Status:** v0.6 — input only, verified on the Yamaha MX61 on macOS 27 (arm64).
A bidirectional version with automatic device discovery is in development and is
not part of this release. See [Roadmap](#roadmap).

---

## The problem, stated plainly

Yamaha's USB-MIDI driver stack is **deprecated in practice**: abandoned for a
long list of instruments that Yamaha still lists, and still sells secondhand,
but no longer supports. On current macOS:

- The **3.x kext** (`YamahaSteinbergUSBAudio.kext`) declares device
  personalities that no longer attach on recent kernels.
- The **4.x DriverKit extension** is, on at least one current package, a
  **malformed bundle** — no `Contents/` directory, `Info.plist` and the binary
  sitting at bundle root — so it fails to register and throws `errno 80` at
  every boot while changing nothing.
- Yamaha's download pages list these instruments as discontinued, with no
  driver build for current macOS.

So the instrument becomes **invisible, not broken**. It enumerates on the USB
bus, it is powered, it sends MIDI — but macOS binds no driver, because the MIDI
interface declares itself as **vendor-specific** (`class 0xFF`, subclass 3)
instead of USB-MIDI (`class 0x01`, subclass 3). macOS looks at that class byte,
sees a vendor device it has no driver for, and moves on.

Underneath that vendor-class interface, the device speaks **standard USB-MIDI
1.0**: 4-byte event packets on a bulk endpoint. It always did. The class byte is
the only thing standing between you and a working instrument.

This project claims the interface with `libusb` and republishes the stream as a
CoreMIDI virtual source, so every DAW sees it as an ordinary MIDI input port.

---

## Devices this applies to

The vendor-class habit is not unique to one model. It is a **pattern across
Yamaha's product line**, and it also covers Steinberg (same parent company),
whose MI2/MI4 interfaces are explicitly documented in the Linux kernel as
"compatible with the USB class audio spec, but the MIDI part ... reported as a
vendor specific interface".

Families that follow the pattern, based on Yamaha's own product history and the
quirks the Linux kernel has accumulated for them:

| Family | Notes |
|---|---|
| **Yamaha MX** (MX49 / MX61 / MX88) | MX61 is the model this project was built and verified on. |
| **Yamaha MOTIF / MOX / MODX** | Long-lived workstation line, repeatedly affected by driver churn. |
| **Yamaha P-series, S90, CP stage pianos** | Same USB-MIDI-behind-vendor-class arrangement. |
| **Yamaha Clavinova / Tyros / Genos** | Home and arranger lines with USB-to-host MIDI. |
| **Yamaha MU / UX USB-MIDI interfaces** | Older outboard MIDI interfaces, driver-dependent. |
| **Steinberg UR / MI series** | MI2/MI4 carry a dedicated Linux quirk entry. |

### Honest status of that table

**v0.6 is hardcoded for the MX61.** It is not a universal driver. The table
above is where the *pattern* applies and where reports are wanted — not a list
of tested devices.

Making it work for another model in this release means editing five constants
(see [Adapting to another device](#adapting-to-another-device)). The values come
from the device's own descriptors, which you can read without any vendor
software. The in-development v7 replaces the constants with a descriptor walk
and adds a `--probe` mode so you can see what it finds before trusting it.

If you have one of these instruments, **please open an issue with your
`system_profiler SPUSBDataType` output and a log from the bridge.** Measured
facts get a device added to the table with a "verified" note. Guesses do not.

---

## What was measured on the MX61

These are the descriptor facts this release is built around, recorded so the
next person does not have to rediscover them:

| Property | Value |
|---|---|
| USB ID | `0499:1508` |
| MIDI interface | interface **3**, alternate setting 0 |
| Interface class triple | `0xFF` / `3` / `0` (vendor-specific) |
| Bulk IN endpoint | `0x84`, `wMaxPacketSize` **32** |
| Bulk OUT endpoint | `0x03` (present; unused by v0.6) |
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
playing. The bottom row is an instrument. If you adapt this to another device,
do not "optimise" the read size upward.

There is a second, subtler trap documented in the source: on macOS,
`libusb_bulk_transfer` can return `LIBUSB_ERROR_TIMEOUT` **with a non-zero byte
count** — the bytes did arrive, the transfer just did not complete inside the
window. Discarding those byte counts silently drops exactly the dense passages
you most want. The code processes them.

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

`install.sh` builds, installs to
`~/Library/Application Support/YamahaMX61Bridge/`, writes a per-user
LaunchAgent at `~/Library/LaunchAgents/com.rpfiend.yamahamx61bridge.plist`, and
loads it. No `sudo` anywhere.

The bridge starts at login and survives replugs. It also survives the keyboard
being absent at login: the log will read `device not present, waiting...` and it
attaches when you plug in.

### Verify

```sh
tail -f ~/Library/Logs/yamahamx61bridge.log
```

Play something. You should see events forwarded, at most one line per second.

In your DAW, look for a MIDI **input** called **Yamaha MX61**. That is the
instrument.

### A note about Audio MIDI Setup

**The bridge will not appear in Audio MIDI Setup's MIDI Studio window.** That is
correct behaviour, not a bug. MIDI Studio enumerates *devices*
(`MIDIDeviceRef` objects with entity graphs); this is a *virtual source*, which
belongs to a client, not a device. macOS has nothing to draw.

The control case is on your own machine: GarageBand's "Virtual Out" is a virtual
source created with the same API, by Apple, and it does not appear in MIDI
Studio either. Look in your DAW's MIDI input list instead — that is where it
matters.

### Uninstall

```sh
launchctl bootout gui/$(id -u)/com.rpfiend.yamahamx61bridge
rm -rf "$HOME/Library/Application Support/YamahaMX61Bridge" \
       "$HOME/Library/LaunchAgents/com.rpfiend.yamahamx61bridge.plist"
```

---

## Adapting to another device

Everything device-specific is five constants at the top of
`src/yammidi-bridge.c`:

```c
#define VID          0x0499   /* idVendor      */
#define PID          0x1508   /* idProduct     */
#define MIDI_IF      3        /* bInterfaceNumber of the vendor-class MIDI interface */
#define EP_IN        0x84     /* bulk IN endpoint address, high bit = IN */
#define EP_READ_SIZE 32       /* that endpoint's wMaxPacketSize */
```

To find yours, plug the device in and run:

```sh
system_profiler SPUSBDataType
```

That gives the vendor and product IDs. The interface number, endpoint addresses
and packet size come from the configuration descriptor; `lsusb -v` on Linux, or
`ioreg -p IOUSB -l -w0` on macOS, will show them. Look for the interface with
`bInterfaceClass 0xFF` and `bInterfaceSubClass 3`.

Set those five, rebuild, and watch the log. If the interface number or endpoint
address is wrong you will get a claim failure or a stalled read rather than
corruption — the failure mode is loud, which is the point.

**Do not change `EP_READ_SIZE` upward.** It must equal the endpoint's
`wMaxPacketSize`, for the reason measured above.

Then open an issue with the numbers so the table can be updated and the next
person does not repeat the work.

---

## Limitations of this release

Stated plainly, because silent gaps are worse than documented ones:

- **Input only.** No MIDI OUT. You cannot send program changes, CC automation,
  or clock from the DAW to the instrument, so its arpeggiator free-runs instead
  of following your tempo.
- **SysEx is not reassembled.** A `0xF0` start byte is forwarded; the
  continuation bytes behind it are not. Voice dumps, patch editors and backup
  utilities will not work.
- **Cable number is ignored.** All five embedded jacks are merged into one
  source. In practice the MX61 appears to transmit on one cable, so this has not
  mattered — but it is a real simplification.
- **One process per device.** Only one process can claim the interface. Run the
  LaunchAgent or a foreground instance, never both.
- **No all-notes-off on detach.** Pull the cable mid-chord and the DAW may hold
  those notes. Release them manually, or use your DAW's panic function.
- **No MIDI 2.0 / UMP.** MIDI 1.0 only.
- **Not signed or notarised.** It is built locally from source on your machine,
  which is why this ships as source rather than a binary.

None of these affect the core use case: playing an obsolete Yamaha keyboard into
software instruments.

---

## Roadmap

`v7` exists in the development tree and is **not** part of this release. It adds:

- bidirectional MIDI — a `Yamaha MX61 Out` destination, including clock
- SysEx reassembly in both directions
- per-cable separation instead of merging
- **descriptor-based discovery**: find any interface with class `0xFF` /
  subclass `3` and a bulk IN endpoint, and read the endpoints and packet size
  from the descriptor rather than hardcoding them
- `--probe`, which describes matching devices without claiming them
- `--selftest`, 19 codec assertions that run with no hardware attached

It will be published once it has been verified against real hardware. The USB
OUT path in particular is unproven: whether these instruments accept writes on
their bulk OUT endpoint without a vendor-specific handshake is unknown, and
guessing is not publishing.

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

If you are the maintainer of one of those, the descriptor-walk design in v7 owes
you a debt; every one of them hardcodes a single device, and the obvious next
step was to stop doing that.

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
