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
- Optional unit/integration-style test suite: 16 tests when `SKYWAVE_LINK_BUILD_TESTS=ON`

ALE support has been removed from the codebase, UI, and configuration.

## Feature Summary

- Live station-to-station chat with typing indicator, fragmentation/reassembly, CQ, broadcast, ping, and QSO timing
- File transfer up to 50 KiB with CRC validation, resume/cancel support, and file-type detection. Offers are accepted automatically only with `auto_accept`, from a trusted callsign, or as a requested BBS download; any other offer opens an **Incoming File** accept/decline prompt (declined automatically after 120 s, at most 8 waiting)
- Radio mail with inbox/sent views, broadcast mail, message priority, and compose templates for free text, ICS-213, and radiogram traffic
- Email relay support through SMTP, POP3 polling, and Winlink CMS Telnet
- HF store-and-forward mesh relay controls with deduplication, TTL, hop limits, and relay diagnostics. Relayed chat and mail are limited to one hop, can trigger no session-only action, and are shown as "[relayed via X, sender unverified]"
- APRS-IS direct reporting, APRS relay via connected stations, GPS serial input, and station map display
- BBS file hosting, remote listing sync, and remote file fetch workflow
- Frequency/channel management with slot grid, channel sniffer, QSY suggestion/accept flow, and recommendation scoring from beacon/ping history
- Beaconing, away mode, PSK Reporter uploads, and live voice control/PTT over the modem voice socket

## Build Requirements

Required:

- CMake 3.16+
- C++20 compiler
- Threads

Recommended:

- SDL2 and OpenGL for the GUI executable
- SQLite3 for persistent message storage
- OpenSSL for SMTP TLS, Winlink helpers, and OpenSSL-gated core features

Bundled in-tree:

- Dear ImGui under `thirdparty/imgui`
- `stb_image` under `thirdparty/stb`
- `nlohmann/json` headers under `thirdparty/json/include`

If SDL2 or OpenGL is missing, CMake still builds `skywave_link_core` and `skywave_link_emulator`, but it skips the GUI executable.

If SQLite is missing, the build continues, but `MessageStore` cannot open a database. The old README claim about a file-storage fallback is no longer true.

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

Configs carrying the ancestor application's ports (`8300`/`8301`/`8302`, in
either command/data order) are migrated to the Skywave defaults on load.

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

It speaks the same control dialect as the modem (verb-echo responses,
`EVT:STATE` / `EVT:CONNECTED` / `EVT:DISCONNECTED` notifications, a `STATUS`
line with the modem's field set), so the app needs no special mode.

It does not emulate the live voice media socket, the passive monitor, or any
waveform behaviour: voice verbs answer `ERROR:UNSUPPORTED`, and mode/family
commands are accepted and ignored.

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
codec = "AUTO"
mode = "CONNECTED"
family = "AUTO"

[aprs]
enabled = false
server = "rotate.aprs.net"
port = 14580
beacon_interval_sec = 900
gps_enabled = false
gps_baud = 9600

[email]
gateway_enabled = false
winlink_telnet_enabled = false
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

[beacon]
enabled = false
interval_sec = 600
beacon_text = "Skywave Link"

[away]
enabled = false
auto_reply = true

[psk_reporter]
enabled = false
antenna = "Dipole"
upload_interval_sec = 300

[ui]
dark_mode = true
font_size = 14
show_timestamps = true
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
src/core/           config, modem, file transfer, mesh relay, web server, utilities
src/gui/            app and modular ImGui windows
src/email/          SMTP, POP3, Winlink Telnet clients
src/messaging/      message router
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
Dear ImGui (MIT), nlohmann/json (MIT), stb_image (MIT / public domain);
linked SDL2 (zlib), SQLite (public domain) and OpenSSL 3 (Apache-2.0).
