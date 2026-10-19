# Skywave Link Changelog

All notable changes to Skywave Link should be recorded here.

The format follows Keep a Changelog.

## [Unreleased]

### Changed (2026-10-18)

- **Modern GUI look.** Roboto (UI) and Cousine (monospace) are compiled
  into the binary from `thirdparty/imgui/misc/fonts` (CMake generates
  `embedded_fonts.cpp`; a missing TTF falls back to ImGui's built-in font)
  and render sharp on Retina displays. New dark and light themes
  (`src/gui/modern_style.cpp`): neutral backgrounds, one blue accent,
  rounded corners, roomier padding, semantic colours for RX/TX, warnings
  and callsigns. Settings -> UI -> Font Size now applies live (default 15)
- Main window opens at ~80% of the display on first run (`[ui]
  window_width/height = 0`), remembers its size on exit and has a minimum
  size. The main view's layout scales with the font: the status bar and
  window buttons are no longer cut off, heard-station lists and chat use
  tables, panels have section headers. Tool windows open centred,
  staggered and sized to the font. Multi-line mail/message bodies no
  longer clip their labels
- Test hook: `show <window>` and `theme dark|light`

### Fixed (2026-10-18)

- GUI frame pacing: the loop slept a fixed 16 ms on top of the VSync
  wait (halving the frame rate); it now sleeps only the unused part of
  the frame budget
- The 4 Hz voice-status poll no longer blocks the GUI thread waiting for
  the reply (>= 10 ms each, up to 500 ms); the pending-message counter is
  queried once a second instead of every frame
- **No GUI-thread waits on the modem.** User actions and settings
  (connect, disconnect, abort, CQ, beacon, PTT, voice, QSY, MYCALL, ...)
  are sent without waiting for the reply: `ModemClient` gains
  `sendCommandAsync()` and `setNonBlockingCommands()` (the GUI turns it
  on; replies, including `ERROR:`, are matched in order as `poll()` reads
  them and logged as before). Blocking calls stay available for tests and
  tools
- Connecting to the modem runs in the background (`beginConnect()` /
  `pollConnect()`, 3 s connect timeout): an unreachable host no longer
  hangs the window; "CONNECTING..." shows on the button and status bar and
  a log line reports failure. The blocking `connect()` also times out
- Weather requests no longer sleep-poll the link state for up to 400 ms;
  native file/folder pickers run on a helper thread so the window keeps
  rendering while they are open
- **Email gateway network I/O off the GUI thread.** POP3 / Winlink Telnet
  fetches and Winlink Telnet / SMTP deliveries run as one job at a time on
  a helper thread (`EmailGatewayWorker`, `src/email/email_gateway_job.cpp`);
  requests are snapshotted and results (store writes, diagnostics, chat
  notices, retry queue) applied on the GUI thread. Relayed radio mail no
  longer blocks while it is delivered
- **Bounded, lossless email gateway.** POP3, SMTP and Winlink Telnet
  connects time out after 5 s and every socket read/write (and TLS
  handshake) after 15 s (`src/email/net_timeouts.hpp`). Fetched mail is
  stored before the server copy is deleted: POP3 sends QUIT (which commits
  the DELEs, RFC 1939) and Winlink kills messages only after the app has
  written them to the store; otherwise the server keeps them. POP3
  Message-ID is used for deduplication. Outgoing internet-relay mail is kept
  in a durable queue (`internet_relay_queue` table) until a confirmed send
  and reloaded at start. On quit a running job is stopped (no new fetch or
  delivery), finished and applied (up to 45 s with the window hidden);
  abandoning it after that still deletes nothing on the server
- **APRS-IS login in the background** (`APRSClient::beginConnect()` /
  `pollConnect()`, 5 s connect timeout, also for the blocking
  `connect()`); "Send to APRS-IS Now" while logged out queues the beacon
  until the login completes
- Quitting hides the window at once and bounds the radio-link teardown to
  ~0.7 s (was up to ~2 s)
- `imgui.ini` (window layout) is stored in `~/.config/skywave-link/`
  instead of the current directory. Right-panel scrollbar, clipped
  Weather buttons and the Frequency window's hard-coded slot colours fixed.
  Dead `App::renderMainContent/renderSettingsTab/renderPositionTab` removed
- File-transfer **trusted callsigns** can be set: `[files]
  trusted_callsigns` in `config.toml` and a Trusted Callsigns field in
  Settings -> Files. The list was consulted but never loaded, saved or
  editable, so it was always empty. Matching ignores case and spaces;
  changes apply immediately

### Fixed (2026-10-17)

- File offers that are not auto-accepted (no auto-accept, untrusted
  callsign) open an "Incoming File" accept/decline prompt instead of being
  refused; undecided offers time out after 120 s, at most 8 pending.
  Test hook: `file_accept`, `file_decline`, `wait_file_offer`
- The offer prompt labels its size as compressed (the sender reports the
  original size)
- Relayed mesh chat and mail are marked "[relayed via X, sender
  unverified]", and the mail view shows the relay
- `test_modem_client`: the send-buffer test no longer assumes 2.4 MB
  fills the loopback socket (macOS auto-tuning sometimes absorbed it all
  and the test failed under parallel load)

### Fixed - full bug review (2026-10-16)

- POP3 no longer spins forever when the server closes mid-message, and
  never DELEs a truncated or interrupted message
- SMTP: implicit TLS on 465, STARTTLS required whenever TLS is requested
  (fails closed - never AUTH in cleartext), socket timeouts
- PTT server: a client that keys then drops is unkeyed; stopping the
  server can no longer deadlock or hang
- Sender trust: link traffic is attributed to the linked peer (the
  payload-supplied 0xFA sender unwrap is gone); session-only messages
  (file transfer, BBS requests, QSY, away mode, mail ACKs) are accepted
  only from the linked peer; broadcast envelopes obey the broadcast
  whitelist; mesh relay payloads are limited to chat/mail, one level deep
- APRS-IS: remote text is stripped of CR/LF/control characters and
  positions are validated before relaying (no line injection); server
  close is detected
- Gateway relay allowlist checks the radio sender; inbound internet mail
  for non-radio addresses is kept locally instead of dropped
- Files are accepted only with auto-accept, from a trusted callsign, or as
  a requested BBS download (a refused offer is reported in chat);
  downloads never overwrite an existing file
- BBS serves files only when enabled; BBS owner is the broadcast sender;
  filesystem errors are reported instead of crashing the app
- Modem client: session buffer cleared when a radio session ends; command
  responses matched to their command; frames never sent truncated;
  re-entrant reads safe; every resolved address tried
- Chat fragment reassembly restarts/times out per sender; weather requests
  URL-encoded, queued and attributed correctly; QSY only from the linked
  peer and within band edges; mail ACKs only from the recipient; settings
  edits persist until Apply; memory growth from remote peers capped

### Fixed - found by the two-station end-to-end test

- Session data is framed (`A5 5A | len | message`): the modem's connected
  stream merges and splits writes, which lost long (fragmented) chat
  messages, garbled mail ACKs and swallowed BBS requests sent during a file
  transfer
- Ping works without a session: an addressed broadcast (`0xE0`) answered by
  an addressed broadcast (`0xE1`), 120 s timeout
- Mail composed while the channel is busy is sent once (the store-backed
  pending sender skipped nothing the reliable-mail loop already owned, so
  the peer got it twice); the queue notice says "channel busy" rather than
  "link unavailable" while connected
- Broadcasts are shown once: `EVT:BROADCAST` is the only broadcast path
- A beacon from a sender the modem has not named yet (`?`) takes its
  callsign from the beacon text
- Test hook (`SKYLINK_TEST_SCRIPT`, see [TESTING_HOOK.md](TESTING_HOOK.md)):
  scripted GUI runs, GL screenshots and a JSON report

### Changed - ported to the Skywave modem

Full detail in [SKYWAVE_PORT.md](SKYWAVE_PORT.md).

- Rebranded from the ancestor application: binaries `skywave_link` /
  `skywave_link_core` / `skywave_link_emulator`, namespace `skylink`, headers
  under `include/skywave_link/`, config at `~/.config/skywave-link/config.toml`
- Default modem ports moved to 9100 (data) / 9101 (control) / 9102 (voice);
  configs on the old 8300-8302 ports are migrated on load
- `ModemClient` rewritten for Skywave's control dialect: verb-echo responses
  instead of `OK`, `EVT:` notifications instead of `NOTIFY:`, `MON:` monitor
  lines, `ERROR:UNSUPPORTED` distinguished from `ERROR:UNKNOWN`
- Waveform control replaced by the Skywave surfaces: `FAMILY`
  (WIDE/MID/NARROW/ULTRA), `MODE`, `MAXMODE`, `CPMODE`, `HARQ`; the
  WAVEFORM/MODULATION/CODERATE/LADDER commands are gone
- `[modem] ladder` replaced by `[modem] family`; `[voice] family` removed
  (Skywave picks the Codec2 rate from the live ladder mode)
- Link quality is now polled from `STATUS`; the modem emits no periodic SNR
  notification
- The voice socket is optional - a modem built without `ENABLE_VOICE` no
  longer prevents the app from connecting

### Added

- `src/protocol/skywave_protocol.cpp`: Skywave link-frame decoding (envelope
  + CRC-32, frame-type table, BEACON/CQ bodies, BCAST_DATA fragment chains)
  with `tests/test_skywave_protocol.cpp` covering it
- `ModemClient::onBroadcast` and `App::onBroadcastMessage()`: broadcast
  application traffic now arrives reassembled from `BCAST_DATA` chains rather
  than being read out of beacon payloads
- `skywave_link_probe`: connects to a live modem and reports every query the
  app depends on

- Modular GUI windows for mail, weather, BBS, frequency management, beacon/ping panels, main content rendering, and station map display
- Live voice control surface with codec/mode/family selection, PTT hotkey capture, audio-device selection, and modem voice-socket status
- Radio mail templates for ICS-213 and radiogram message composition
- Winlink CMS Telnet delivery and fetch helpers
- Mesh relay support with TTL, hop-limit, deduplication, routing, and relay diagnostics
- Weather request workflows for forecast, METAR, TAF, and marine reports
- Station map support using bundled Natural Earth basemap assets
- PSK Reporter upload controls
- Optional link emulator target for two-GUI command/data testing
- Optional tests for POP3, image preview, mesh relay, and Winlink Telnet, alongside the core codec/config/store/frequency coverage

### Changed

- Product branding has been renamed from `Skywave` to `Skywave Link`
- GUI and emulator build products are now `skywave_link` and `skywave_link_emulator`
- Default config storage now uses the `Skywave Link` app name, with automatic fallback to the legacy `Skywave` config path when needed
- Documentation was rewritten to match the current codebase, build targets, ports, config sections, and GUI surfaces
- Modem defaults are now documented correctly as command `8301`, data `8300`, voice `8302`
- Legacy configs using the old swapped command/data pair are normalized on load
- Architecture guidance now reflects the current modular-window design instead of the original phased roadmap

### Removed

- ALE runtime functionality
- ALE settings persistence
- ALE GUI surfaces
- Several dead legacy `App` render methods that no longer had call sites after the modular-window migration

### Fixed

- Config tests now tolerate older config files that still contain an `[ale]` section by ignoring it

## [0.2.0]

Current project version as defined in CMake.
