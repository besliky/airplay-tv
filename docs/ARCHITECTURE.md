# Architecture

AirPlay TV has two layers: a protocol and media core written in C
(`libairplaytv.so`, about 300 KB), and a thin Kotlin layer for the Android
lifecycle, service discovery, settings and the screens. Media data never
crosses JNI except decoded PCM on its way to `AudioTrack`, and covers and photos
as the encoded pictures the sender sent.

```
                        ┌──────────────────────── Android (Kotlin) ────────────────────────┐
 BOOT_COMPLETED ──────▶ │ BootReceiver ─▶ ReceiverService (foreground, connectedDevice)      │
 MY_PACKAGE_REPLACED    │                  ├─ NetworkMonitor (ConnectivityManager callback) │
 SCREEN_ON              │                  ├─ Advertiser (NsdManager: _airplay, _raop)      │
                        │                  ├─ AudioOutput (AudioTrack writer thread)        │
                        │                  ├─ NowPlayingTracker, MediaKeys, DacpClient      │
                        │                  └─ ReceiverState ─▶ MainActivity, MirrorActivity,│
                        │                                      NowPlayingActivity, Photos   │
                        └──────────────┬──────────────────────────────────▲─────────────────┘
                                 JNI   │ start/stop, surface, TXT records │ events (session,
                                       ▼                                  │ video, PIN, track)
                        ┌──────────────────────────── native core (C) ─────────────────────┐
 iPhone ──RTSP/TCP────▶ │ airplay.c  RTSP server: /info, pairing, fp-setup, SETUP, …       │
                        │   ├─ pairing.c / srp.c   Ed25519 + X25519 pair-verify, SRP PIN    │
                        │   ├─ fairplay.c + playfair   stream key hand-over                  │
                        │   ├─ ntp.c     timing requests to the sender                      │
                        │   ├─ dmap.c    track info from music apps                         │
 ──mirror TCP────────▶ │   ├─ mirror.c  AES-CTR, AVCC→Annex-B, parameter sets ─┐           │
 ──RTP/UDP───────────▶ │   └─ audio_rtp.c  AES-CBC, jitter buffer, resend     ─┼─┐         │
                        │ android/video_decoder.c  AMediaCodec ──▶ Surface ◀────┘ │         │
                        │ android/audio_pipeline.c AAC (MediaCodec) / ALAC ◀──────┘         │
                        └──────────────────────────────────────────────────────────────────┘
```

## Protocol core

The request handling follows UxPlay's `lib/` (itself based on RPiPlay and
shairplay), which is the most mature open-source implementation of AirPlay
mirroring. It was not wrapped but rewritten around these goals:

- **No heavy dependencies.** OpenSSL is replaced by a vendored subset of Mbed TLS
  (AES, SHA, big numbers) and Monocypher (X25519, Ed25519); libplist by a small
  binary-plist reader/writer (`bplist.c`); llhttp by a strict RTSP parser
  (`rtsp.c`); GStreamer by the Android NDK media APIs.
- **Event-driven.** Every receiver thread blocks in `poll()` on its sockets and a
  wake-up pipe. An idle receiver has one native thread and wakes up for nothing.
  The server loop only uses a timer while connections exist.
- **Hostile input.** Every length, offset and index from the network is checked
  before use. Limits: 16 KiB request headers, 2 MiB bodies (16 MiB for a photo
  upload), 32 headers, 8 connections, 32 MiB mirror packets, bounded plist depth and
  node count, bounded DMAP nesting. The
  upstream code this is based on had several unchecked lengths (FairPlay mode
  index, pairing payload sizes, SPS/PPS parsing, ALAC sample counts); these are
  all validated here and covered by tests and fuzzers.

### Session setup

1. `GET /info` — capabilities, display size, public key.
2. `POST /pair-setup`, `/pair-verify` — optional legacy pairing. With *Require PIN*,
   `/pair-pin-start` shows a PIN and `/pair-setup-pin` runs SRP-6a (2048-bit,
   SHA-1, Apple's 40-byte session key) followed by an AES-GCM exchange of Ed25519
   keys. Paired senders are remembered by their public key, and in PIN mode
   `SETUP` is refused unless the connection proved such a key.
3. `POST /fp-setup` (two rounds) — FairPlay SAP handshake; afterwards the `ekey`
   in `SETUP` yields the session key (hashed with the pair-verify secret when
   pairing took place).
4. `SETUP` with streams: type 110 opens the mirroring TCP port, type 96 the audio
   UDP ports. `TEARDOWN` ends a stream or the session; `POST /feedback` is the
   sender's heartbeat.

### Track info, remote control and photos

- `SET_PARAMETER` carries what a music app plays: DMAP track info (`minm`, `asar`,
  `asal`, parsed by `dmap.c`), the cover as an image (an empty `image/none` means the
  track has none) and `progress: start/current/end` in RTP timestamps. They are reported
  only for the session on screen.
- Requests carry `DACP-ID` and `Active-Remote`. The first names the sender's
  remote-control service (`_dacp._tcp`, `iTunes_Ctrl_<id>`), the second is the token its
  commands need. Only a 64-bit hex id and a 32-bit decimal token are accepted, because
  both end up in a service name and an HTTP header. `DacpClient` finds the service with
  NSD and sends `GET /ctrl-int/1/playpause|nextitem|previtem` over a plain socket (the
  app-wide cleartext policy stays strict).
- Photos use the HTTP/1.1 side of AirPlay on the same port: `GET /server-info`,
  `POST /reverse` (the event channel, kept open while a photo is shown; closing it ends
  the photo session), `PUT /photo` with `X-Apple-AssetKey` and `cacheOnly` /
  `displayCached` backed by a cache of 8 photos and 48 MB, and `POST /stop`. In PIN mode
  photos need a paired sender. AirPlay video and slideshows are refused and logged.

One session is active at a time. A new sender's `SETUP` replaces the current
session, so a phone that disappeared without saying goodbye never blocks the TV.
Dead peers are detected by TCP keepalive on the control and mirror connections,
with a 90-second inactivity watchdog as a backstop.

## Video pipeline

```
mirror socket ─▶ payload buffer ─▶ AES-CTR (in place) ─▶ AVCC→Annex-B (in place)
             ─▶ one copy into the MediaCodec input buffer ─▶ decoder ─▶ Surface
```

- The decoder is chosen by `DecoderSelector`: hardware decoders only (software is
  a last resort, e.g. on emulators), preferring those with
  `FEATURE_LowLatency`. It is configured with `low-latency`, realtime priority and
  the vendor low-latency keys of Qualcomm, HiSilicon, Exynos and Amlogic decoders;
  if a decoder rejects them, the options are dropped one step at a time.
- Output buffers are released for display immediately
  (`releaseOutputBufferAtTime(now)`). If several pictures are ready at once, only
  the newest is shown. Nothing waits for a presentation clock.
- Senders send key frames only when a stream starts or changes format, so the
  decoder must not lose its reference pictures during a session. When the
  playback activity stops (Home button), the decoder is moved to an invisible
  `AImageReader` surface with `setOutputSurface` and keeps decoding; it moves back
  without a restart. When a decoder has to be recreated (new resolution, surface
  switch refused, decoder error), the frames since the last key frame are kept in
  a bounded cache (24 MB / 900 frames) and replayed into the new decoder.
- Rotation and resolution changes arrive as new parameter sets; a changed size
  gets a fresh decoder, otherwise the parameter sets are sent in-band. The
  activity sizes the `SurfaceView` to the decoded aspect ratio.

## Audio pipeline

- RTP packets are decrypted (AES-CBC, whole blocks) and reordered in a 256-slot
  jitter buffer that removes the sender's redundant copies. Missing packets are
  requested once; a gap is skipped after 30 ms for mirroring audio and 250 ms for
  music.
- AAC-ELD and AAC-LC are decoded by the platform's software AAC decoder through
  `AMediaCodec` (vendor decoders often lack ELD); ALAC by the bundled decoder.
- PCM goes into a ring buffer that the `AudioTrack` writer thread drains with
  blocking writes. Playback starts after a 40 ms cushion (mirroring) or 250 ms
  (music). If the sender's clock runs faster than the TV's audio clock, the
  oldest samples are dropped so latency cannot creep up during long sessions.
- Mirroring audio uses `PERFORMANCE_MODE_LOW_LATENCY` and a buffer of at most
  80 ms. Volume changes from the sender are applied to the `AudioTrack`.
- Senders do not announce a pause; they stop sending audio. The writer reports a pause
  after 300 ms without audio and the moment it comes back. The progress update a sender
  sends at a pause can repeat an old position, so an update that came right around the
  pause is replaced by the position from before it.

## Android lifecycle

- **Foreground service** of type `connectedDevice`. Its notification uses a
  minimum-importance channel, which Android TV does not display.
- **Autostart**: `BOOT_COMPLETED`, `MY_PACKAGE_REPLACED` and vendor quick-boot
  broadcasts start the service when *Start Automatically* is on. `START_STICKY`
  brings it back after the process is killed.
- **Network changes**: a `NetworkCallback` for Wi-Fi and Ethernet (with or
  without internet access) triggers a debounced re-registration of the mDNS
  services when addresses change, and withdraws them without a network. The RTSP
  server listens on the wildcard address and needs no restart. `SCREEN_ON`
  re-announces the services after the TV wakes.
- **Discovery** uses `NsdManager`, i.e. the platform's mDNS responder, so there
  is no second responder competing for port 5353. Registrations are serialized so
  the same name is never published twice. If another device already uses the
  name, the responder publishes a substitute ("Name (2)"), which the UI shows; the
  configured name is tried once more after ten seconds, since the conflict is
  often with this TV's own records still cached on the network. A multicast lock
  is held while the receiver runs.
- **Sessions** acquire a partial wake lock and a low-latency Wi-Fi lock, released
  as soon as the session ends. Decoders, audio output and their threads exist only
  during a session.
- **Opening the picture**: the service starts `MirrorActivity` when mirroring
  begins or a PIN must be shown, `NowPlayingActivity` when music (non-mirroring audio)
  starts and `PhotoActivity` for a photo. Android 10+ allows this from the background
  only with the *Display over other apps* permission; for mirroring, a notification with
  a full-screen intent is posted otherwise.
- **Turning the TV on**: when a sender connects while the screen is off, a short
  `ACQUIRE_CAUSES_WAKEUP` wake lock turns it on (a stick's TV follows over HDMI-CEC), and
  the playback screens use `setTurnScreenOn`. If AirPlay turned the TV on and nobody
  used the remote, *Sleep After Music* locks the screen again through an accessibility
  service, the only way an app may put a TV device to sleep.
- **Now Playing** keeps no state of its own: `NowPlayingTracker` turns the native events
  into `ReceiverState.nowPlaying` (track changes and their direction, cover colours, a
  position that runs between updates, a cover dropped when a new track brings none),
  and a `MediaSession` routes the remote's media keys to the sender while another app is
  in front. Covers and photos are decoded on a background thread, shrunk to the screen
  and turned upright by their EXIF orientation.
- **Updates**: `Updater` reads the GitHub releases only when the user selects Software
  Update, picks the package of the running ABI, checks it against the release's
  `SHA256SUMS` and hands it to `PackageInstaller`. Android installs it only over an app
  signed with the same key. Once the app installed itself, Android 12+ lets later
  updates through without asking again; after an update the service restarts through
  `MY_PACKAGE_REPLACED` and the app reopens.

## Threads

| State | Threads |
|---|---|
| Idle | main thread, one native server thread (blocked in `poll`) |
| Mirroring with audio | + mirror receiver, decoder output, audio receiver, timing, AudioTrack writer |
| Music | + audio receiver, timing, AudioTrack writer; image decoding and remote commands on demand |

## Security

- Local network only: connections whose source is not in one of the TV's
  subnets (or IPv6 link-local) are closed immediately. The internet is used only for
  Software Update (when selected) and Lyrics (when turned on), both over HTTPS.
- Keys: the receiver identity is an Ed25519 key generated on the device and
  stored encrypted by an Android Keystore AES-GCM key. Key material is wiped from
  memory after use. No secrets, PINs or sender names are logged.
- The FairPlay component is only the session-setup handshake of AirPlay. It
  cannot decrypt DRM-protected media, and protected apps refuse to mirror.
