# Testing

## Automated tests

| Suite | What it covers | Where it runs |
|---|---|---|
| `core/tests` unit tests | binary plist, RTSP parser, DMAP track info, crypto known-answer vectors (NIST AES, RFC 7748 X25519, RFC 8032 Ed25519, SHA), pair-verify, SRP, FairPlay framing, mirror parsing (avcC/hvcC, AVCC→Annex-B), audio jitter buffer, ALAC | CI (clang, ASan + UBSan), any Android device via adb |
| `core/tests` loopback tests | complete sessions against the real server over sockets: legacy pairing, no pairing, PIN pairing (right and wrong PIN, remembered sender), sender takeover, receiver-side disconnect, sender vanishing, PIN hidden when the sender leaves, hostile and half-open connections, encrypted mirror and audio streams compared byte for byte, track info, cover, progress and remote-control identity, photos (cache, event channel, PIN mode) | same |
| fuzzers | `bplist`, `rtsp`, `mirror`, `alac`, `dmap` and the complete request handler (`server`) with libFuzzer | CI, 60 s per target per run |
| JVM unit tests | name sanitising, settings keys, key validation, hex helpers, LRC lyrics, version ordering, release notes | CI |
| instrumentation tests | the real foreground service answering RTSP over loopback, TXT records, malformed traffic, a device name already taken on the network (substitute name, name taken back once free) | Android TV emulator |
| lint | Android lint on the release variant, no issues allowed to accumulate | CI |

Run the native tests on a device or emulator without a Linux machine:

```
cmake -S core -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-26 -DANDROID_STL=none -DAIRPLAYTV_TESTS=ON
cmake --build build-android
adb push build-android/tests/airplaytv_tests /data/local/tmp/
adb shell chmod 755 /data/local/tmp/airplaytv_tests
adb shell /data/local/tmp/airplaytv_tests
```

With `-DAIRPLAYTV_SANITIZE=ON` the same build runs under AddressSanitizer and
UndefinedBehaviorSanitizer; push the NDK's `libclang_rt.asan-x86_64-android.so` next to
the binary and start it with `LD_LIBRARY_PATH=/data/local/tmp`. Leak detection is only
available in the Linux build.

## Test sender

`airplaytv_sender` (built with the tests) speaks the sender side of the protocol:
pairing, FairPlay framing, `SETUP`, the encrypted mirror stream and, with `--audio`, an
ALAC tone. The picture is a real H.264 stream generated without an encoder (I_PCM key
frames and skipped P frames), so any decoder can show it.

```
adb push build-android/tests/airplaytv_sender /data/local/tmp/
adb shell chmod 755 /data/local/tmp/airplaytv_sender
adb shell /data/local/tmp/airplaytv_sender 127.0.0.1 7000 20 --pair --audio
adb shell /data/local/tmp/airplaytv_sender 127.0.0.1 7000 10 --pin-screen   # with Require PIN on
```

`--music` plays like a music app instead: no picture, but track info, a cover and the
position, a pause from 40 to 55 % of the time and a second track without a cover after it.
The Now Playing screen opens by itself.

```
adb shell /data/local/tmp/airplaytv_sender 127.0.0.1 7000 30 --pair --music
```

The FairPlay key exchange cannot be reproduced without Apple's implementation; the
sender derives the stream key with the receiver's own playfair code, so both ends
agree on the key. Everything else is exercised exactly as with a real sender.

![Test stream on the emulator](images/test-stream.png)

## Looking at the screens without a phone

Debug builds include a preview that fills the Now Playing and photo screens with made-up
content, for parts that otherwise take minutes or a remote to see:

```
adb shell am start -n io.github.besliky.airplaytv/.PreviewActivity --es show now
```

`show` is one of `now`, `pause`, `lyrics`, `dim`, `ambient`, `blank`, `goodnight`,
`volume`, `next`, `previous`, `photo` and `photo2`. Back restores the real state. Capture
the screen with `adb exec-out screencap -p > shot.png`.

## Verified on the Android TV emulator (API 36, x86_64)

- install, first launch, settings navigation with the D-pad
- the release build (R8, release signing) with the test stream and audio
- service start, `_airplay._tcp` and `_raop._tcp` published once, IP shown
- a sender session with pairing: decoder created in the background, moved to the
  display surface when the activity appeared, picture shown with the right aspect
  ratio, ALAC audio to `AudioTrack`, clean teardown
- PIN screen shown on request and hidden when the sender leaves
- autostart after reboot (`BOOT_COMPLETED` → foreground service → published)
- restart after the process was killed
- receiver restart and re-publication after settings changes
- music with the test sender: Now Playing opens, track info, cover and colours, progress,
  the pause (icon, smaller cover), a track change that drops the old cover, closing at the
  end of the session
- Software Update from a release build: the GitHub release found, the arm64/x86_64
  package downloaded and verified, the system installer (and Google TV's Play Protect
  notice) and the new version installed with the app as its installer

## Needs a real TV and iPhone

- discovery and mirroring from iOS (FairPlay key exchange with a real sender)
- track info, pauses, the remote control (DACP) and photos from a real iPhone; turning a
  TV on and off over HDMI-CEC
- hardware decoders of TV SoCs (Amlogic, MediaTek, Realtek, Qualcomm), vendor
  low-latency modes, `setOutputSurface` support
- AAC-ELD mirroring audio and lip sync on HDMI outputs
- Wi-Fi reconnects and address changes (the emulator's Ethernet cannot be dropped
  without root), TV standby and wake
- 30+ minute sessions, rotation and lock/unlock on the phone, H.265 at 4K

A useful manual matrix: connect, disconnect with Back, reconnect; rotate the phone;
lock and unlock; switch apps on the phone; play a video (lip sync); switch the TV's
network; put the TV to sleep and wake it; reboot the TV; stream music from the Music
app; enable *Require PIN*, pair, reconnect without a PIN, reset pairing.
