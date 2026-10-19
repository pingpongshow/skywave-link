# Skywave Link

Skywave Link is an ImGui-based HF digital communications client for the
[Skywave modem](../skywave/docs/README.md). It provides live chat,
store-and-forward messaging, file transfer, radio mail, BBS sharing, APRS
position reporting, weather requests, beaconing, QSY coordination, station
mapping, and live voice control over the modem's data/control/voice TCP
interfaces.

It is the "companion app" half of the split described in
[INTERFACES.md](../skywave/docs/INTERFACES.md): the modem owns the waveform,
sessions, timing, audio and PTT keying; this app owns rig CAT control,
frequency planning, the operator UI, and everything above the byte stream.

The modem lives in its own repository (`skywave`). Links of the form
`../skywave/...` in this README assume the two are checked out side by
side, as `Modem/skywave` and `Modem/skywave-link`. This app is written
against modem **v1.2.7**; both stations' modems must run the same release.

## Current Scope

- GUI application: `skywave_link`
- Core static library: `skywave_link_core`
- No-RF/no-modem test bridge: `skywave_link_emulator`
- Control-protocol probe: `skywave_link_probe`
- Optional unit/integration-style test suite: 22 tests when `SKYWAVE_LINK_BUILD_TESTS=ON`

ALE support has been removed from the codebase, UI, and configuration.

## Feature Summary

- Live station-to-station chat (each line framed `0xC0 | UTF-8 text`, long lines fragmented and reassembled), CQ with a x1-x10 repeat, broadcast, ping, and QSO timing
- File transfer up to `files.max_file_size` (default 50 KiB, enforced on both ends) with an end-to-end size/CRC check, cancel, and file-type labels. Offers are accepted automatically only with `auto_accept`, from a trusted callsign, or as a requested BBS download; any other offer opens an **Incoming File** accept/decline prompt (declined automatically after 120 s, at most 8 waiting)
- Radio mail with inbox/sent views, broadcast mail, message priority, and compose templates for free text, ICS-213, and radiogram traffic. Private mail is sent over a link only to its addressee; otherwise it waits in the outbox or goes by mesh relay
- Email relay through SMTP, POP3 polling, and Winlink CMS (B2F with secure login over TLS); only stations on the relay allowlist may use it (an empty list admits nobody)
- HF store-and-forward mesh relay with deduplication, TTL, hop limits, directed relaying over fresh learned routes (route table in Settings -> Email), and relay diagnostics. Relayed chat and mail are limited to one hop, can trigger no session-only action, and are shown as "[relayed via X, sender unverified]"
- APRS-IS reporting with a server-side filter and inbound APRS messages, APRS relay via connected stations, GPS serial input (checksummed NMEA), and station map display
- BBS file hosting, remote listing sync, remote file fetch, and uploads to a linked station's BBS when it allows them
- Frequency/channel management with slot grid, channel sniffer, QSY suggestion/accept flow, and recommendation scoring from beacon/ping history
- Weather forecast, METAR/TAF, marine and US weather-alert requests through a linked station
- Beaconing, away mode, PSK Reporter uploads (IPFIX, at most every 5 minutes), and live voice control/PTT over the modem voice socket

## Build Requirements

Required:

- CMake 3.16+
- C++20 compiler
- Threads
- SQLite3 (message store; CMake stops without it)
- Hamlib (rig control)

Recommended:

- SDL2 and OpenGL for the GUI executable
- OpenSSL for TLS (SMTP, POP3, Winlink CMS)

Bundled in-tree:

- Dear ImGui under `thirdparty/imgui`
- `stb_image` under `thirdparty/stb`
- `nlohmann/json` headers under `thirdparty/json/include`

If SDL2 or OpenGL is missing, CMake still builds `skywave_link_core` and `skywave_link_emulator`, but it skips the GUI executable.

SQLite is required: there is no file-storage fallback, and a build without
it would lose mail, so CMake refuses to configure.

## Passwords

SMTP, POP3 and Winlink account passwords are stored in the system keychain
where one is available (macOS); `config.toml` then only notes that the
password is in the keychain. Elsewhere they are written to `config.toml`,
which is saved owner-only (mode 0600). The Winlink password is never sent
to the CMS: it only answers the secure-login challenge.

## Build

```bash
cd Modem/skywave-link
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

Build tests explicitly:

```bash
cd Modem/skywave-link
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSKYWAVE_LINK_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

## Runtime Ports

Skywave Link talks to the modem on the three localhost ports defined in
[INTERFACES.md](../skywave/docs/INTERFACES.md):

| Port | Default | Carries |
|---|---:|---|
| data | `9100` | application byte stream, plus `0xFB` binary monitor records while no session is connected |
| control | `9101` | line commands, verb-echo responses, `EVT:` / `MON:` notifications |
| voice | `9102` | framed PCM16LE media records |

The modem accepts **one client per port**. If another client (including a
second copy of this app) is already attached, the connection is refused.

The voice port is optional: a modem built without `ENABLE_VOICE` never binds
it, and the app continues without live voice media rather than failing the
whole connection.

Configs carrying the ancestor application's ports (`8300`/`8301`/`8302`) are
not migrated: set the Skywave ports (9100/9101/9102) in Settings > Modem.

## Running With A Modem

1. Start the modem stack separately.
2. Launch the GUI:

```bash
./build/skywave_link
```

3. Open `Settings` and set at least:
   - Station callsign and grid square
   - Modem host
   - Modem command/data/voice ports
4. Connect the modem from the control bar or the `Settings -> Modem` tab.

Config path:

- macOS/Linux: `~/.config/skywave-link/config.toml`
- Windows: `%APPDATA%\\Skywave Link\\config.toml`

### Verifying the link without the GUI

`skywave_link_probe` opens the same three sockets, runs every query the app
relies on, and prints what the modem answered:

```bash
./build/skywave_link_probe            # 127.0.0.1, default ports
./build/skywave_link_probe 127.0.0.1 9101 9100 9102
```

It exits non-zero if any query failed, which makes it a quick check after a
modem upgrade.

## No-Modem GUI Testing

`skywave_link_emulator` bridges two GUI instances over TCP command/data sockets so you can test chat, CQ, ping, mail, file transfer, BBS, weather requests, and link-state behavior without RF hardware.

It follows the current modem's control dialect for what the app uses:

- verb-echo responses, `EVT:STATE` / `EVT:CONNECTED` / `EVT:DISCONNECTED`
  notifications and a `STATUS` line with the modem's field set
- `BCAST <hex>` answers `BCAST <n>`; every other station gets
  `EVT:BROADCAST <src> <HEX>`, linked or not
- `BEACON` / `CQ` answer their verb; stations with `MONITOR ON` get a `MON:`
  line with the whole link frame (BEACON body: call + `0xFF`, CQ body: call)
- data-port bytes travel only inside a connected session; outside one they
  are dropped. Payloads are opaque, so the app's own framing (chat
  `0xC0|text`, station beacon `0xFA...`, mail, files) is passed unchanged
- the placeholder callsign (N0CALL or none) refuses transmissions with
  `ERROR:STATE`; `CONNECT` to an unknown station ends with
  `EVT:DISCONNECTED NO_ANSWER`

It does not emulate the voice media socket or any waveform behaviour: voice
verbs answer `ERROR:UNSUPPORTED`, and mode/family commands are accepted and
ignored. `tests/test_emulator.cpp` drives the real binary with two clients.

Build the emulator:

```bash
cd Modem/skywave-link
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target skywave_link_emulator -j4
```

Run the emulator in split-endpoint mode:

```bash
./build/skywave_link_emulator \
  --host 127.0.0.1 \
  --a-cmd-port 18450 --a-data-port 18451 \
  --b-cmd-port 18460 --b-data-port 18461 \
  --verbose
```

Run two GUI instances with isolated homes:

```bash
SKL=$PWD/build/skywave_link
mkdir -p /tmp/skywave-link-test/a /tmp/skywave-link-test/b
(cd /tmp/skywave-link-test/a && HOME=/tmp/skywave-link-test/a "$SKL")
(cd /tmp/skywave-link-test/b && HOME=/tmp/skywave-link-test/b "$SKL")
```

Configure the two GUIs:

- Instance A: host `127.0.0.1`, command `18450`, data `18451`
- Instance B: host `127.0.0.1`, command `18460`, data `18461`

The emulator does not provide a voice endpoint, so leave voice testing for a real modem session.

## GUI Surfaces

The current GUI centers around the main control/chat surface plus dedicated windows for:

- Files
- Messages
- Mail
- Weather
- Position
- Station Map
- BBS
- Frequency
- Settings

Settings tabs currently include:

- Station
- Modem
- Voice
- Files
- APRS
- Email
- Beacon
- Away Mode
- PSK Reporter
- UI

## Configuration Sections

`config.toml` currently persists:

- `[station]`
- `[modem]`
- `[voice]`
- `[radio]`
- `[aprs]`
- `[email]`
- `[store]`
- `[files]`
- `[bbs]`
- `[beacon]`
- `[away]`
- `[psk_reporter]`
- `[ui]`

Minimal example:

```toml
[station]
callsign = "N0CALL"
grid_square = "FN20"
name = "Your Name"

[modem]
host = "127.0.0.1"
command_port = 9101
data_port = 9100
voice_port = 9102
family = "WIDE"            # WIDE | MID | NARROW | ULTRA
auto_connect = true

[voice]
auto_start = false
codec = "AUTO"             # AUTO | 700C | 1300 | 2400 | 3200
mode = "CONNECTED"

[aprs]
enabled = false
server = "rotate.aprs.net"
port = 14580
beacon_interval_sec = 900
auto_beacon = true
filter = ""                # empty: r/<lat>/<lon>/<filter_range_km> plus your call
filter_range_km = 100
gps_enabled = false
gps_baud = 9600

[email]
gateway_enabled = false
allowed_relays = ""        # stations allowed to relay email; empty: nobody
winlink_telnet_enabled = false
winlink_telnet_host = "cms.winlink.org"
winlink_telnet_port = 8773
winlink_telnet_use_tls = true
weak_signal_profile_enabled = true
hf_store_forward_enabled = true

[files]
download_path = "downloads"
max_file_size = 51200
auto_accept = false        # false: offers open the Incoming File prompt
trusted_callsigns = ""     # e.g. "W1AW, K6XYZ-2": accepted without the prompt

[bbs]
enabled = false
bbs_path = "bbs"
allow_uploads = false
allowed_uploaders = ""     # stations whose uploads are accepted

[beacon]
enabled = false
interval_sec = 600
beacon_text = "Skywave Link"

[away]                     # away mode itself is a runtime toggle, not saved
auto_reply = true

[psk_reporter]
enabled = false
antenna = "Dipole"
min_snr = -20

[ui]
dark_mode = true
font_size = 14
show_timestamps = true
sound_notifications = true
```

## Known Gaps

- The emulator has no voice endpoint and no waveform behaviour (above).

## Band Coverage

User-facing band selectors in the current GUI expose:

- `80m`
- `40m`
- `30m`
- `20m`
- `17m`
- `15m`
- `12m`
- `10m`

The core `FrequencyManager` also defines `60m`, but the current GUI pickers do not surface it yet.

## Repository Layout

```text
include/skywave_link/   public headers
src/core/           config, modem, radio/PTT, file transfer, mesh relay, message
                    store, PSK Reporter, keychain, notification sound
src/gui/            app and modular ImGui windows
src/email/          SMTP, POP3, Winlink CMS clients, email gateway job
src/position/       APRS client and NMEA parser
src/protocol/       B2F and LZH codecs, Skywave link-frame decoding
tests/              optional test targets
tools/              link emulator, control-protocol probe
assets/maps/        station-map basemap assets
```

## Additional Docs

- `docs/FEATURES.md`: current feature inventory
- `docs/IMPLEMENTATION_STRATEGY.md`: current architecture and module layout
- `docs/CHANGELOG.md`: changes, newest first
- `docs/TESTING_HOOK.md`: the scripted test hook (`SKYLINK_TEST_SCRIPT`) used for two-station end-to-end runs
- `docs/SKYWAVE_PORT.md`: what changed when this app was adapted from its
  ancestor to the Skywave modem

The modem's own design documentation lives in `../skywave/docs/`; the ones
this app is written against are `INTERFACES.md` (ports and commands),
`PROTOCOL.md` (link frames), `WAVEFORM.md` (families and the mode ladder) and
`VOICE.md` (codec selection).

## License

Copyright (C) 2026 The Skywave authors.

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version. It is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [LICENSE](LICENSE)
for the full text (GPL-3.0-or-later).

Bundled components keep their own licenses, all compatible with GPL-3.0:
Dear ImGui (MIT), nlohmann/json (MIT), stb_image (MIT / public domain),
the embedded GUI fonts Roboto (Apache-2.0) and Cousine (SIL OFL 1.1, see
[thirdparty/imgui/misc/fonts/LICENSE.txt](thirdparty/imgui/misc/fonts/LICENSE.txt));
linked SDL2 (zlib), SQLite (public domain) and OpenSSL 3 (Apache-2.0).
