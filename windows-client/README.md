# FlowCast for Windows

Mirror this Windows PC's screen **and system sound** to a FlowCast receiver
(the Android/TV app in the parent directory) over the local network.

No iPhone, no driver, no virtual display, no install: one `flowcast.exe`.

```
flowcast.exe                 open the window
flowcast.exe --host 192.168.0.201     console mode, no window
flowcast.exe --receiver sony --max-width 1280 --fps 30
```

One binary, two front ends: no arguments opens the window, anything on the
command line runs the console client. In console mode **Ctrl+C** stops; in the
window the big button toggles. Either way the receiver goes back to its idle
screen when you stop.

---

## The window

A 480×574 card: target receiver on top, three settings, one start button, one
status card. Everything inside the window is painted by hand with GDI+, so there
is no framework and no theme engine to fight. The only shipped asset is the icon
(`res/flowcast.ico`), because Explorer reads it from the exe, not from code.

The icon is the Android app's icon: white card, two overlapping panels, play
triangle. It is not redrawn here — `ui::icon_app_logo` replays the same numbers
as `res/drawable/ic_launcher_foreground.xml` (182 viewport, 104×72 mark, panels
84×56 r12, back panel offset (20,−16)), and `--make-icon` renders that mark into
a nine-size .ico which `rc.exe` compiles into the exe. One drawing, three
surfaces: file icon, taskbar, window header.

```bat
build\bin\flowcast.exe --make-icon=res\flowcast.ico   ; after changing the mark
```

The accent is the icon's blue: `#185FA5` on light, `#2E7CC4` on dark. Not
`#85B7EB` on dark — a large pale-blue slab becomes the brightest thing on screen
and pulls the eye off the status card, so the mark keeps the pale blue and the
button keeps the deep one.

- **Target** — click the card to pick a discovered receiver, re-scan, or type an
  address. Typing happens in place: click the card, type, `Enter` commits,
  `Esc` backs out, clicking anywhere else also commits. `Ctrl+V` pastes. The
  card's outline turns accent-coloured while it is a text field.
- **Quality** — 720p30 / 1080p30 / 1080p60 / 1440p30 / native. Never upscales:
  a cap above the captured screen's own width simply sends it as-is, so the
  top three entries only differ on a screen that is actually larger than
  1920 wide. The screen is the *encoded* size; nothing here can add detail it
  does not have.
- **Capture screen** — which display is mirrored, shown as the resolution
  Windows reports for it plus `主屏` for the primary one. Only appears when a
  second screen is attached; the row is hidden on a single-monitor PC rather
  than offering a choice that cannot change anything. Picking a 4K TV set up as
  an extended display is the only way this app ever sends a native 4K stream.
- **Sound** — mirror system audio or send video only.
- **Language** — 跟随系统 / 简体中文 / 繁體中文 / English. The default is the
  system language, resolved with `GetUserDefaultUILanguage()` (the user's
  display language, not the machine default). Traditional Chinese gets
  `Microsoft JhengHei UI`, Simplified `Microsoft YaHei UI`, English `Segoe UI`;
  the family decides the glyph style for characters the two scripts share, and
  only the right one makes Traditional text look native.
- **Status card** — the only place a failure is ever explained. Raw client
  errors are mapped to a sentence the user can act on ("连不上这个地址，检查 IP
  是否正确"), not echoed as English protocol text.

Dark and light follow the desktop (`AppsUseLightTheme`), live: switching the
system theme while the window is open repaints it. The window is per-monitor DPI
aware v2 and rebuilds its fonts on `WM_DPICHANGED`.

### Text is rendered grayscale, on purpose

Three settings decide whether the type looks sharp, and GDI+'s defaults are wrong
for all three:

- **Integer pixel sizes.** Fonts are created at `round(size × scale)`, not at
  `13.5 × 1.0`. A fractional em box makes every stem land between pixels, and
  hinted CJK then reads as mud.
- **`PixelOffsetModeNone` for text.** The half-pixel offset that makes a 1 px
  border sit on the grid also shifts hinted glyphs by half a pixel, forcing a
  resample — every stem turns grey.
- **`TextContrast(6)`.** GDI+'s default gamma of 4 renders CJK at 12–14 px
  visibly lighter than the same text in a native control.

ClearType is *not* used. GDI+'s subpixel filter is a cheap one: white text on a
dark card gets an orange top edge and a blue bottom edge on every CJK stem, which
reads as blur even though the glyphs are grid-fitted. Clean grayscale edges plus
a heavier gamma beat sharper-but-dirty. (DirectWrite would do better on both
counts, but it is a second text stack for one window.)

Settings live in `%APPDATA%\FlowCast\client.ini`:

```ini
[client]
lang=system          ; system | zh-Hans | zh-Hant | en
quality=1            ; 0=720p30 1=1080p30 2=1080p60 3=1440p30 4=native
presetRev=2          ; written on save; lets an old quality=3 ("native") migrate to 4
display=\\.\DISPLAY2 ; screen to capture; empty or missing = the primary screen
systemAudio=1
lastAddress=192.168.0.201
lastPort=7000
```

Delete the file to reset everything. `lang=system` is the default, so a fresh
install already speaks the desktop's language.

---

## Build

Requirements: Visual Studio 2022 (MSVC x64) + Windows 10 SDK. No CMake, no
MinGW, no external packages — everything under `third_party/` is vendored.

```bat
build.bat client     :: build\bin\flowcast.exe
build.bat test       :: build + run the self-tests
build.bat all        :: both
build.bat clean
```

Output goes to `build\bin\`. The binary is self-contained.

---

## Console options

Passing any of these opens the console client instead of the window; nothing
here affects the window, which remembers its own settings in `client.ini`.

| Option | Meaning |
|---|---|
| `--host <name\|ip>` | Receiver to use. Default: mDNS discovery. |
| `--receiver <text>` | When discovery finds several receivers, only accept one whose service name contains `text` (case-insensitive). |
| `--port <n>` | AirPlay port (default 7000). |
| `--name <text>` | Name the receiver shows (default `Windows PC`). |
| `--fps <n>` | Target frame rate, 1–120 (default 30). |
| `--bitrate <bps>` | Video bitrate. Default: ~0.16 bit per pixel per frame, clamped to 3–40 Mbit/s (about 10 Mbit/s at 1080p30, 18 at 1440p30). |
| `--max-width <n>` | Downscale the captured screen to this width (default 1920). `0` = native. |
| `--display <n>` | Capture screen `n` from `--list-displays` (default 1). |
| `--list-displays` | Print the capturable screens and exit — the quickest way to see whether the TV is really there at 3840×2160. |
| `--probe-capture` | Open the capture path *and* the encoder, report which backend answered, the frame size, whether it is non-black, which encoder was picked and whether it actually emits NAL units, then exit. The first thing to run on a machine that mirrors nothing. |
| `--test-pattern` | Send synthetic colour bars instead of the desktop: separates "nothing was ever sent" from "the desktop could not be read". |
| `--audio-bitrate <bps>` | AAC bitrate (default 128000). |
| `--no-video` / `--no-audio` | Send only one of the two. |
| `--discover-ms <n>` | Discovery window (default 3000). |
| `--duration <sec>` | Stop automatically, useful for testing. |
| `-v` | Debug logging. |

`FLOWCAST_CAPTURE=dxgi|wgc|gdi` (environment) forces one capture backend, for
testing a fallback on a machine where the default one works.


Discovery logs **every** receiver it sees, so a wrong pick is easy to
diagnose — rerun with `--receiver`.

---

## What it actually speaks

AirPlay 1 (RAOP) mirroring, implemented from scratch against UxPlay's
receiver behaviour. Nothing is delegated to iTunes or Bonjour.

| Stage | Detail |
|---|---|
| Discovery | mDNS PTR/SRV/TXT/A for `_airplay._tcp`, queried once per IPv4 interface (a VPN adapter otherwise steals the multicast send). |
| Handshake | `OPTIONS` → `fp-setup` ×2 → `SETUP` ×1 (timing) → `SETUP` ×2 (streams) → `RECORD`. |
| Pairing | Skipped. The receiver only hashes the FairPlay AES key when a pairing secret exists, and ours never creates one. |
| FairPlay | The `ekey` is *constructed*, not decrypted: `chunk1` is chosen so that the receiver's forward `cycle()` lands on the AES key we already know. No inverse permutation needed. |
| Bodies | `application/x-apple-binary-plist`. The receiver parses request bodies with `plist_from_bin()`, which hard-requires the `bplist00` magic and does **not** fall back to XML — an XML body silently yields an empty dict and the whole SETUP looks like a success with no ports. |
| Video | DXGI output duplicated → BGRA → NV12 (D3D11 video processor, scaling included) → Media Foundation H.264 → 128-byte-header mirror packets → AES-128-CTR, keystream continuous across packets. |
| Audio | WASAPI loopback → linear resample to 44.1 kHz → Media Foundation AAC-LC (1024 samples/frame) → RTP, AES-128-CBC with the IV reset per packet and only the 16-byte-aligned prefix encrypted. |
| Clock | NTP timing replies plus periodic RTP sync packets; the receiver's audio playout clock is the master and video is aligned to it. |

### Things that are easy to get wrong

- Video key derivation uses **SHA-512**, not SHA-256:
  `key = sha512("AirPlayStreamKey<connID>" ‖ aeskey)[0:16]`. UxPlay's
  `sha_init()` is `EVP_sha512()`, and its `aeskey_video[64]` buffer size is the
  tell.
- `streamConnectionID` lives inside the `streams[]` dictionary, not at the top
  level of the SETUP body.
- The mirror header repeats the frame size in three places; the receiver sizes
  its surface from them, so they must be the *encoded* size.
- AAC is `ct=4` / `spf=1024`; the receiver rebuilds the AudioSpecificConfig
  (`0x12 0x10`) itself from those two values.
- Media Foundation encoders reject an input type set before any output type
  (`MF_E_INVALIDMEDIATYPE`), and the AAC MFT additionally needs both
  `MFT_MESSAGE_NOTIFY_BEGIN_STREAMING` and `_START_OF_STREAM` before it emits
  anything at all.
- **"The first encoder that activates" is not good enough.** Vendor MFTs
  activate happily and then refuse the media types — AMD's `AMDh264Encoder`
  answers `SetInputType` with `MF_E_TRANSFORM_ASYNC_LOCKED` (`0xc00d6d77`) on a
  perfectly healthy Windows 10 desktop. Enumeration order is not the same on
  every machine, so stopping at the first activation made one build work here
  and fail on another with "the encoder would not start". Every candidate now
  has to survive type negotiation; if none does, the in-box Microsoft software
  encoder is created by CLSID as a guaranteed last resort.

---

## Behaviour worth knowing

**Silent desktops still send audio.** WASAPI loopback produces no packets at
all while nothing is playing — not even silence packets. The receiver's audio
clock is the master clock for A/V sync, so the client pads the gap with real
silence frames to keep the stream continuous. Without it a quiet machine would
never start the clock.

**1 ms timer resolution.** `timeBeginPeriod(1)` for the life of the process.
At the Windows default of ~15.6 ms both the frame pacing and the audio poll
quantise badly, and the receiver — which pins a near-zero cushion for mirror
audio — reports continuous underruns.

**The BGRA → NV12 conversion runs on the GPU.** It used to be a scalar loop over
every pixel, and that loop — not the encoder — was what made a 4K stream
impossible: 6.5 ms per frame at 1080p, 24.5 ms at 4K, on top of reading 33 MB of
BGRA back from the GPU. `ID3D11VideoProcessor` does the same conversion (and the
downscale, so `--max-width` no longer costs CPU either) in ~0.1 ms, and only the
NV12 result — 1.5 bytes per pixel instead of 4 — comes back. Measured on an AMD
780M:

| | before | after |
|---|---|---|
| 1080p blit + readback | 5.4 ms + 33 MB | 0.7 ms + 3 MB |
| 4K blit + readback | 19 ms + 33 MB | 1.8 ms + 12 MB |
| 1080p encode | 6.5 ms | 1.9 ms |
| 4K encode | 24.5 ms | 5.9 ms |

The colour path matters as much as the speed: the processor emits **limited-range
BT.709**, which is what an HD H.264 decoder assumes. The old scalar loop wrote
full-range BT.601 values, so the receiver was expanding a range that had never
been compressed. `test/nv12conv_test.cpp` converts known colour blocks and
converts them back, which is the only way this fails loudly instead of just
looking slightly wrong.

**Frame delivery is change-driven.** With Desktop Duplication a static screen
produces no packets at all; the receiver simply keeps showing the last frame.
That is the point, not a stall.

**One screen per stream, chosen by name.** Desktop Duplication captures one
output, and output 0 is not necessarily the primary one — with a TV on HDMI it is
whichever screen the driver brought up first, so the client matches
`DXGI_OUTPUT_DESC.DeviceName` (`\\.\DISPLAY2`) against the saved choice and
creates the D3D device on the adapter that owns it. An enumeration index would
shift the moment a screen is unplugged, silently retargeting the mirror; a device
name follows the port. Unplug the chosen screen and capture falls back to the
primary one instead of failing.

**Capture has three paths, tried in this order.** `DuplicateOutput` returns
`E_ACCESSDENIED` while the desktop is unavailable (a locked session, the UAC
secure desktop, another duplicator still winding down); it retries for ~2 s,
then hands over to Windows Graphics Capture, and only then to `BitBlt`.

- *Desktop Duplication* — a GPU texture with no copy, the normal case.
- *Windows Graphics Capture* (`wgc.cpp`) — reads the desktop from DWM instead of
  from the display output, which is what makes a hybrid-GPU laptop, an indirect
  display (USB-C dock, DisplayLink), another app already holding the output, and
  an HDR desktop (FP16 frames, aimed at the DWM path instead of the BGRA one)
  capturable at all. Windows 10 1809+, and it draws a capture border on Windows
  10; Windows 11 lets it be switched off, which the code asks for.
- *BitBlt* — of the same screen, not of the whole virtual desktop (two screens
  at different sizes in one frame gives a seam and the wrong aspect).

`FLOWCAST_CAPTURE=dxgi|wgc|gdi` pins one of them. A fallback nobody ever runs is
a fallback that does not work, and this is how it gets exercised on a machine
whose default path is healthy.

**Every failure is written down.** The exe is GUI-subsystem: double-clicked from
Explorer it has no console at all, so `%LOCALAPPDATA%\FlowCast\flowcast.log`
takes every line (rotated at 1 MB, one `.old` generation). When a session fails,
the status card grows an *Open log* link, and `--probe-capture` reports which
backend answered, with what code, and whether the frame is actually non-black.

---

## Tests

```bat
build.bat test
```

- `playfair_test` — SHA-256/512, AES-128 ECB/CTR, the mirror key and IV
  derivations against independent values, keystream continuity across packets,
  200 rounds of FairPlay ekey construction.
- `plist_test` — XML plist round-trip, escapes, real RAOP SETUP bodies,
  malformed input rejection.
- `bplist_test` — binary plist reader against vectors generated by Python's
  `plistlib` (regenerate with `tools/gen_bplist_vectors.py`), plus writer
  round-trips and a byte-exact known answer.
- `aacenc_test` — drives the AAC MFT in isolation: 1 s of tone must yield
  ~40 access units, and the AudioSpecificConfig must be `12 10`.
- `nv12conv_test` — the GPU BGRA→NV12 converter: colour round-trip through
  BT.709 limited range (≤1 level of error), plus the per-frame cost at 1080p,
  1440p and 4K.
- `displays_test` — screen enumeration and name resolution: ordered 1..n, one
  primary, and a stale or lower-case device name falls back to the primary
  screen instead of failing.

---

## Layout

```
src/client.cpp   the orchestrator: handshake, then a video and an audio thread
src/capture.cpp  DXGI Desktop Duplication of one chosen screen (+ GDI fallback)
src/wgc.cpp      Windows Graphics Capture: the same screen, read from DWM
src/nv12conv.cpp BGRA -> NV12 and any downscale, on the GPU
src/h264enc.cpp  Media Foundation H.264 encoder, NV12 in
src/gui.cpp      the window: layout, painting, hit testing, inline address editor
src/ui.cpp       theme + drawing primitives; every icon is hand-drawn geometry
src/i18n.cpp     three string tables and the system-language resolution
src/settings.cpp %APPDATA%\FlowCast\client.ini
src/main.cpp     console front end, reusing the same client
test/            self-contained test executables, one main() each
tools/           gen_bplist_vectors.py - regenerates the reader's known answers
third_party/playfair/   FairPlay permutation tables (vendored)
build.bat        compiles src/*.cpp + links; no CMake needed
```

## Window hooks

Only useful when changing the window, since it cannot be screenshotted from a
terminal otherwise:

```
flowcast.exe --theme=dark --shot=out.png --shot-state=edit --shot-lang=zh-hant
```

`--shot-state` accepts `idle-none`, `connecting`, `mirroring`, `failed`,
`devices`, `lang`, `screens`, `edit`, `edit-long`. `--shot-screens=2` makes the
renderer pretend two screens are attached, which is the only way to see the
"capture screen" row and its dropdown on a single-monitor PC. `--theme` is not
persisted, so the next plain launch follows the desktop again.
