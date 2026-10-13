# Changelog

## [0.3.0] - 2026-10-13

AirPlay Car: the receiver as a second app for Android head units, built from the same code.

- A CarPlay-like shell for touch screens: a dock on the driver's side (clock, network, Home,
  the phone's screen, Now Playing, Settings), Home with the track and the connection, a Now
  Playing page with large controls, and a setup guide
- Maps: a landscape picture fills the screen and a touch brings up Home, Music and Stop over
  it; an upright picture stands next to the dock with the clock, the track and Stop
  Mirroring beside it
- The stream is advertised in the shape of the head unit's panel (1024x600, 1920x720, ...)
- Back leaves the page, not the session; the picture opens by itself when mirroring starts
- Dock Side setting for right-hand-drive cars, settings pages sized for head units
- Updates from the same releases (`AirPlayCar-v<version>-<abi>.apk`); installs next to
  AirPlay TV as `io.github.besliky.airplaytv.car`
- Both apps: Russian translation; AirPlay audio goes quiet while another app holds the audio
  focus for a moment
- Test sender: `--portrait` sends an upright picture

## [0.2.0] - 2026-10-12

Music, photos and updates on the TV. The music, photo and wake features are inspired by the
fork [geekylaneorgcf/airplay-tv](https://github.com/geekylaneorgcf/airplay-tv) by Ashish
Jullia, who found out on real hardware how iPhones report track info, pauses and their
remote-control service. Thank you!

- Now Playing screen for music: cover, title, artist, album, progress and volume, a
  backdrop tinted by the cover, transitions in the direction of travel
- The TV remote pauses and skips on the phone (DACP), also from other apps through the
  system's media keys; pauses are detected from the audio stopping
- Optional time-synced lyrics from lrclib.net (off by default; sends title and artist only)
- OLED care on the Now Playing and photo screens: slow drift, dimming, a wandering card and
  black during long pauses
- Photos from the Photos app (PUT /photo with the sender's photo cache)
- The TV turns on when a sender connects in standby; optional sleep after music through an
  accessibility service
- Remote Volume Keys for streaming sticks whose volume buttons change nothing
- Software Update on the TV: installs new GitHub releases, verified against their checksums
- Music settings on a page of their own
- Test sender: `--music` plays a track with info, cover, a pause and a track change

## [0.1.0] - 2026-09-30

First release.

- AirPlay screen mirroring receiver for Android TV and Google TV (Android 8.0+)
- H.264 mirroring up to 1080p60, H.265 up to 4K with a hardware HEVC decoder
- Hardware decoding with MediaCodec straight to the display surface, low-latency
  decoder configuration, newest-frame rendering, decoder kept alive in the background
- Audio: AAC-ELD while mirroring, ALAC and AAC-LC for music, sender volume control
- Bonjour discovery through Android's mDNS responder, re-announced on network
  changes and wake-up, with duplicate device names resolved automatically
- Background receiver: foreground service, start on boot, recovery after process
  death and app updates, automatic opening of the playback screen
- Optional PIN pairing with remembered devices and pairing reset
- TV interface for the D-pad: status, device name, AirPlay on/off, PIN, autostart,
  connection info, advanced settings, performance overlay, diagnostics export
- Local-network-only connections, Keystore-protected identity, no analytics
