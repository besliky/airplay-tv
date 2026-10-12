# Third-party components

AirPlay TV is licensed under the GNU General Public License, version 3 or
later (see `LICENSE`). It contains or is derived from the following
open-source components. Their license texts are shipped inside the app
(Settings → Advanced → Open Source Licenses) and in this repository.

## UxPlay / RPiPlay / shairplay (protocol logic)

- Source: https://github.com/FDH2/UxPlay (and its predecessors
  https://github.com/FD-/RPiPlay and https://github.com/juhovh/shairplay)
- Copyright: Juho Vähä-Herttua (2011–2018), dsafa22 (2019),
  Florian Draschbacher (2019), Jaslo Ziska (2020), F. Duncanh and the UxPlay
  contributors (2021–2026)
- License: GPL-3.0-or-later for the project as a whole; individual files
  LGPL-2.1-or-later
- Used in: `core/src/airplay.c`, `pairing.c`, `mirror.c`, `audio_rtp.c`,
  `fairplay.c`, `ntp.c`. The AirPlay request handling, `/info` contents, key
  derivation, stream packet formats and FairPlay reply tables follow UxPlay's
  `lib/` implementation. The code was restructured for Android (event-driven
  sockets, no GStreamer, no libplist, no OpenSSL) and hardened against
  malformed input.

## geekylaneorgcf/airplay-tv (music, photos, wake)

- Source: https://github.com/geekylaneorgcf/airplay-tv, a fork of this project
- Author: Ashish Jullia (2026)
- License: GPL-3.0-or-later, like this project
- Used in: the Now Playing screen (layout, transitions, cover colours, OLED care, lyrics
  view, sleep after music), the player icons, the photo viewer, the pause and stale
  progress handling, the DACP remote control and the remote volume keys. The features
  were reworked and reorganised for this project; the icons are used as they are.

## playfair

- Source: https://github.com/EstebanKubata/playfair (as distributed with UxPlay)
- Author: Esteban Kubata
- License: GPL-3.0 (`core/third_party/playfair/LICENSE.md`)
- Purpose: the FairPlay SAP handshake that AirPlay senders use to hand over the
  stream key. It is part of session setup and does not decrypt protected media.

## csrp (SRP-6a)

- Source: https://github.com/cocagne/csrp
- Copyright: 2013 Tom Cocagne
- License: MIT
- Used in: `core/src/srp.c` (protocol logic, re-implemented on mbedTLS big numbers)

## Mbed TLS 3.6.7 (subset)

- Source: https://github.com/Mbed-TLS/mbedtls
- Copyright: The Mbed TLS Contributors
- License: Apache-2.0 (dual-licensed Apache-2.0 OR GPL-2.0-or-later; used
  under Apache-2.0), `core/third_party/mbedtls/LICENSE`
- Used for: AES (CTR, CBC, GCM), SHA-1, SHA-512, MD5, big-number arithmetic.
  Only the required files are vendored; they are unmodified.

## Monocypher 4.0.3

- Source: https://github.com/LoupVaillant/Monocypher
- Copyright: 2017–2023 Loup Vaillant, Michael Savage, Fabio Scotoni
- License: BSD-2-Clause OR CC0-1.0, `core/third_party/monocypher/LICENCE.md`
- Used for: X25519 and Ed25519. Unmodified.

## ALAC decoder

- Source: https://github.com/juhovh/shairplay (`src/lib/alac`), originally
  http://crazney.net/programs/itunes/alac.html
- Copyright: 2005 David Hammerton
- License: MIT (see the header of `core/third_party/alac/alac.c`)
- Modified: bounded bit reader, validated frame headers and sample counts,
  overflow-safe arithmetic, 16-bit stereo output only.

## Trademarks

AirPlay, Apple TV and iPhone are trademarks of Apple Inc. This project is an
independent implementation and is not affiliated with or endorsed by Apple.
