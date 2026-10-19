# Skywave Link Implementation Strategy

This file describes the current implementation shape of the project. It is no longer a phased delivery plan.

## Build Targets

The top-level CMake file currently defines four build products:

- `skywave_link_core`
  - Static library containing protocol, modem, radio/PTT, message store, APRS, mail (SMTP/POP3/Winlink), transfer, mesh relay, PSK Reporter, keychain and image-decoding code
- `skywave_link`
  - SDL2/OpenGL/ImGui GUI executable
  - Built only when SDL2, OpenGL, and vendored ImGui sources are available
- `skywave_link_emulator`
  - TCP command/data bridge for two-app testing without RF hardware: speaks
    the modem's control dialect (BCAST/`EVT:BROADCAST`, BEACON/CQ `MON:`
    frames, session-only data, `NO_ANSWER`), passes payloads unchanged
- `skywave_link_probe`
  - runs every modem query the app relies on against a live modem

Optional:

- `tests/*`
  - Enabled only with `-DSKYWAVE_LINK_BUILD_TESTS=ON`

## Runtime Architecture

```text
                         +----------------------+
                         |      Skywave Link GUI    |
                         |      src/gui/*       |
                         +----------+-----------+
                                    |
                                    v
                         +----------------------+
                         |    App / SharedState |
                         +----------+-----------+
                                    |
            +-----------------------+-----------------------+
            |                       |                       |
            v                       v                       v
   +----------------+     +-------------------+   +-------------------+
   |  ModemClient   |     |   MessageStore    |   | FrequencyManager  |
   | cmd/data/voice |     | SQLite-backed     |   | bands, slots, QSY |
   +--------+-------+     +---------+---------+   +---------+---------+
            |                       |                       |
            v                       v                       v
      Skywave modem TCP           message/beacon/ping      recommendation,
      ports 9101/9100/9102    and weather persistence  sniffer, slot math

            +-----------------------+-----------------------+
            |                       |                       |
            v                       v                       v
      APRSClient              mail/relay clients         utility modules
      NMEA parser             SMTP / POP3 / Winlink      B2F / LZH / mesh /
      station map             email gateway job          PSK Reporter / image
```

## GUI Composition

`App` owns all long-lived state and the concrete subsystem instances, then exposes that state through `SharedAppState` to modular window classes.

Current window/component classes include:

- `FileWindow`
- `MessageWindow`
- `MailWindow`
- `WeatherWindow`
- `PositionWindow`
- `BBSWindow`
- `FrequencyWindow`
- `SettingsWindow`
- `StationMapWindow`
- `MainContentRenderer` (chat, control bar, heard beacons/CQ calls, ping)

Shared, ImGui-free helpers for the windows and `App` live in
`include/skywave_link/gui_util.hpp` (callsign/setting normalisation,
position packets, formatting, file-type labels). Every tool window has a
status line showing the result of the last action started there.

The visible runtime workflow is built around:

- main control/chat surface
- secondary task windows switched by `AppTab`
- settings tabs for subsystem configuration

The old monolithic render methods have been partially retired in favor of these dedicated window classes.

## Source Layout

```text
include/skywave_link/
  public headers for config, modem, storage, GUI windows, protocols, and utilities

src/core/
  config, secret_store (keychain)
  freq_manager (bands, slots, slot scanner)
  message_store
  modem_client
  radio_controller (Hamlib, PTT server)
  file_transfer
  image_preview (image decoding for the map)
  mesh_relay
  psk_reporter
  notification_sound

src/gui/
  app
  shared_state
  main content and modular windows

src/email/
  smtp_client
  pop3_client
  winlink_telnet_client
  email_gateway_job

src/position/
  aprs_client
  nmea_parser

src/protocol/
  b2f
  lzh
  skywave_protocol

tests/
  currently 22 test executables when enabled
```

## Subsystem Notes

### Modem connectivity

- `ModemClient` is the transport boundary to the modem
- Handles command, data, and voice sockets independently
- Parses modem state, link state, noise floor, and voice status
- Exposes callbacks used by the GUI event loop

### Persistence

- `MessageStore` is SQLite-backed; SQLite is required at configure time
- Periodic maintenance (`runMaintenance`) expires undelivered mail, caps
  pending mail, trims history and compacts the database
- Besides messages, it stores beacon history, ping history, weather cache, and mail read-state

### Frequency/QSY

- `FrequencyManager` owns band definitions, slot calculations, and QSY helpers
- Core band definitions include `60m`, but current GUI selectors expose eight bands only
- Recommendation scoring uses recent stored beacon and ping records plus propagation estimation from grid squares

### Mail and relay

- GUI mail composition and viewing live in `MailWindow`
- Internet-side delivery/fetch helpers live in `src/email/`
- Mesh relay safety policy is configured through `EmailConfig`
- Relay diagnostics are surfaced back to the Settings UI

### Position and APRS

- `APRSClient` handles APRS-IS login, direct position reports, and position parsing
- `NMEAParser` supports GPS serial integration
- Position reporting exists in three forms:
  - direct APRS-IS
  - position relay via connected station
  - radio position beacon
- `StationMapWindow` renders station markers on a Natural Earth basemap copied into the build output

### Voice

- Live voice transport is handled through the modem voice socket and voice-related commands in `ModemClient`
- Uplink PCM is queued in whole records and flushed from `poll()`; the
  voice socket is reopened when it closes

## Configuration Model

The persisted `AppConfig` currently contains:

- `station`
- `modem`
- `voice`
- `radio`
- `aprs`
- `email`
- `store`
- `files`
- `bbs`
- `beacon`
- `away`
- `psk_reporter`
- `ui`

ALE configuration has been removed.

The application also normalizes legacy configs that still use the old swapped modem command/data port pair.

## Testing Strategy

Tests are build-time optional and currently cover:

- `test_app_helpers` (GUI-independent app logic)
- `test_aprs`
- `test_b2f`
- `test_config`
- `test_email_gateway_job`
- `test_emulator` (two clients through the real emulator binary)
- `test_file_transfer`
- `test_freq_manager`
- `test_gui_util`
- `test_image_preview`
- `test_lzh`
- `test_mesh_relay`
- `test_message_store`
- `test_modem_client`
- `test_nmea`
- `test_notification_sound`
- `test_pop3`
- `test_psk_reporter` (IPFIX checked by an independent decoder)
- `test_radio_controller`
- `test_skywave_protocol`
- `test_smtp`
- `test_winlink_telnet` (fake CMS: B2F handshake, secure login, LZHUF)

No test contacts a real server (Winlink, PSK Reporter, APRS-IS, mail,
weather); network tests use loopback fakes or unroutable addresses.

Two-station end-to-end runs (two modems over a virtual audio cable, two
app instances driven by `SKYLINK_TEST_SCRIPT`) cover the GUI flows; see
`docs/TESTING_HOOK.md`.

This gives good coverage of the core library, but GUI behavior is still primarily exercised through manual runs and the link emulator.

## Current Boundaries

- GUI builds are conditional on SDL2/OpenGL availability
- SQLite is effectively required for message persistence
- The link emulator covers command/data workflows, not live voice media
- ALE has been removed and should not be reintroduced into docs or config examples unless the codebase adds it back
