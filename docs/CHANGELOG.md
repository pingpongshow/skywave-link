# Skywave Link Changelog

All notable changes to Skywave Link should be recorded here.

The format follows Keep a Changelog.

## [Unreleased]

### Fixed (voice, mail and settings usability, 2026-10-19)

- **PTT never put the microphone on the air.** The local "PTT held" flag
  was cleared by any voice status with `PTT=OFF`, including the answer to
  a `VOICESTATUS` poll sent just before `VOICEPTT ON`; the modem then
  talked with no audio. PTT is now level-triggered: the held buttons and
  hotkey are the intent, and `VOICEPTT ON/OFF` follows them once the
  session can take it
- **Silence chopped speech apart.** A frame with no new capture data sent
  40 ms of silence, so ~16 ms frames against 20 ms capture blocks inserted
  silence between real speech. Filler silence now only covers a stalled
  or absent microphone, paced to real time
- **Voice errors were swallowed.** `VOICESTART`/`VOICEPTT`/`VOICE` answers
  (`ERROR:STATE`, `ERROR:UNSUPPORTED`) only reached stderr; a refused
  `VOICESTART` was retried every 750 ms forever. Refusals are explained
  (no link, link mode below the voice floor, peer without voice, N0CALL,
  modem without voice) on the status line and in the log, once
- **VOICE button was an indicator only.** It now opens or closes a voice
  session in the configured mode and explains when it cannot. PTT is
  enabled whenever the modem is connected and voice is not OFF, opens a
  session if needed, and says why when it cannot key (main panel,
  Settings -> Voice, hotkey; hotkey messages rate-limited)
- **Hotkey typed into text fields.** The PTT key's character is no longer
  typed into a focused text field while it keys; the hotkey works only
  while the app window has focus (documented in Settings -> Voice)
- **auto_start** reopens voice at each new opportunity (link up, link
  speeding up to a voice mode, going idle for broadcast) and no longer
  reopens a session the operator stopped
- **Received voice with nobody playing it** accumulated without bound in
  the client and was replayed later; it is capped at 10 s and discarded
  while no speaker is open. EVT:VOICE TALKING/STOPPED now set the PTT flag
- **Internet mail for this station never reached the Inbox.** A Winlink
  message addressed to the station's own callsign was queued for radio
  relay to itself; it now goes to Mail -> Inbox

### Changed (voice, mail and settings usability, 2026-10-19)

- Mail window: Inbox (default, unread count) | Sent | Compose tabs, with
  Reply; the main panel button is now MAIL (with the unread count) and
  opens the Inbox
- Settings: Winlink has its own tab (moved out of Email) with the last CMS
  exchange; the Settings window is wide enough for every tab label (the
  tab bar still scrolls in a narrower main window)
- Main panel: PTT and VOICE share one row
- Settings -> Voice shows audio device state, frames/seconds sent,
  received and played, and the last voice problem
- Test hook: `voice_start`, `voice_stop`, `voice_ptt on|off`,
  `show settings:<Tab>`, and a `voice` object in `dump`

### Security (code review, 2026-10-19)

- **Email relay allowlist fails closed and is persisted.** `[email]
  allowed_relays` was never saved or loaded, so the list was always empty
  and every station could send internet email through the operator's
  account. It is now saved, editable in Settings -> Email, and an empty
  list admits nobody
- **Passwords in the system keychain.** SMTP, POP3 and Winlink passwords
  are kept in the macOS keychain where available (the config file only
  notes that); otherwise the config file is written owner-only. Saves are
  atomic
- **Winlink never sends the account password.** The CMS client answers the
  B2F secure-login challenge (`;PQ` -> `;PR`) and logs in with the fixed
  `CMSTelnet` token; TLS (`cms.winlink.org:8773`) is the default

### Fixed (code review, 2026-10-19)

- **Chat framing.** Chat lines carry their own type byte (`0xC0 | UTF-8
  text`, `0xFD` fragments): text starting with certain UTF-8 lead bytes
  used to be dispatched as mail/ping/weather/control frames. Malformed
  binary frames are dropped instead of shown as text. Chat that could not
  be sent keeps the input and says why
- **Private mail goes only to its recipient.** Mail is sent over the link
  only when the linked station is the addressee; otherwise it waits in the
  outbox (or goes by mesh relay). "Mail sent" is no longer reported for
  mail the peer refused
- **PSK Reporter uploads real IPFIX** (RFC 7011, pskreporter.info spec):
  receiver and sender templates, enterprise number 30351, correct sequence
  numbers and export time, uploads at most every 5 minutes, per-station
  dedup, datagrams kept under the MTU, a bounded queue. The old home-made
  format was dropped by the server while the UI said "Active"; Settings now
  shows queued/sent spots, the last upload and the last error
- **Winlink CMS speaks B2F** (FC EM proposals with `F>` checksum, FS
  answers, LZHUF-compressed SOH/STX/EOT blocks, FF/FQ) with the Winlink
  message format (Mid/Date/Type/From/To/Subject/Mbo/Body/File) and
  Winlink LZHUF (CRC16 + length header). Inbound mail is acknowledged only
  after it is stored. Outgoing gateway mail gets a stable Winlink message
  id per outbox item, so a retry cannot deliver it twice; the B2F envelope
  is type Private
- Beacon/CQ bodies match the modem (BEACON: call + SNR-floor byte, `0xFF`
  unknown; CQ: call only) and frame types 30/31 are known; a beacon's "SNR"
  is shown only when the modem measured it (unknown SNR shows as `-`
  everywhere, and is stored as NULL rather than 0 dB)
- **NMEA**: one parser with mandatory checksum that accepts standard
  GGA/RMC sentences (trailing empty fields); a stale fix times out
- **Mesh relay** uses fresh learned routes for directed relaying (flood
  otherwise), learns routes with measured SNR only, prunes stale routes and
  retries failed relay sends. The route table is shown in Settings ->
  Email (Mesh routes)
- **Store maintenance**: undelivered mail expires after
  `store.message_expiry_days`, the pending cap and `auto_cleanup` apply,
  old history is trimmed and the database is compacted; the weather cache
  is persisted
- PTT: the PTT server listens on 4532 (the modem's default) and the app
  points the modem at it (`PTTMETHOD TCP`, `PTTPORT`); KEY/UNKEY drive the
  TX badge. Hamlib dry run refuses control instead of pretending
- Voice uplink audio is queued in whole records and never sleeps on the
  GUI thread; refused frames are counted and reported (rate-limited); the
  voice socket is reopened when it closes
- Gateway mail no longer tears down the operator's QSO and gateway calls
  are capped with backoff; queued internet email is sent right away when
  the gateway runs; the Messages window reports success only when the
  email was really queued
- QSY, sniffer, ping, weather and position actions report failures
  instead of claiming success; the channel sniffer returns the rig to its
  frequency; weather requests that are refused get an error answer
- Files: `files.max_file_size` is honoured on both ends; transfers can be
  cancelled from the Files window and show as cancelled, not failed
- Settings are saved on Apply / Save (failures reported), not only on a
  clean exit; APRS settings apply without a restart
- Heard tables show the band, bandwidth and SNR actually recorded (or `-`)

### Added (code review, 2026-10-19)

- **Weather alerts** (type `0x05`, US active alerts from api.weather.gov):
  Weather -> Forecast -> Alerts, for a state code, grid square, "lat,lon"
  or place name
- **APRS-IS**: server-side filter (`[aprs] filter`, default a range around
  the station plus its own call), inbound APRS messages shown and acked,
  auto-beacon setting saved
- **Away mode**: Settings buttons work; the linked station can send
  internet email with `EMAIL:<address>:<subject>:<body>` when the away
  options allow it, and always gets a reply
- **BBS uploads**: a linked station may upload into this station's BBS
  folder when `bbs.allow_uploads` is on and it is in
  `bbs.allowed_uploaders` (within `max_upload_size`); the BBS window has
  "Upload to remote BBS"; completed uploads are announced and the list
  refreshed. The BBS welcome message is sent at session start
- Settings -> Modem: MODE, MAXMODE and CWID controls; voice codec
  (AUTO/700C/1300/2400/3200) is sent to the modem
- Incoming chat/mail notification sound (system alert sound, never blocking;
  `ui.sound_notifications`); chat timestamps toggle (`ui.show_timestamps`);
  operator name used as the email From display name
- Status line in every tool window showing the result of the last action
  started there
- Main panel CQ repeat selector (x1-x10)
- Test hook actions `wx_alerts`, `wx_cancel`, `away_on`, `away_off`,
  `save_config`, `email`, `psk_upload`, `tune`, `qsy`, `bbs_send`

### Removed (code review, 2026-10-19)

- Unused modules `ChannelScanner`, `WebServer`, `MessageRouter`,
  `voice_message` and `ImagePreviewCache` (image decoding for the map
  remains), and other unreferenced APIs (`ModemClient` compression, HARQ,
  auto-mode, `receiveData`)
- Never-rendered `ChatWindow`, `BeaconPanel` and `PingPanel`
- Typing indicators (airtime on HF for little value) and the app-level SNR
  exchange: the modem's own report (RSNR) is used. Test hook `snr` removed
- `away.enabled` (away mode is the runtime toggle), `psk_reporter.upload_interval_sec`
  (the spec fixes the cadence), `ui.notification_sound`
- Builds without SQLite: SQLite is now required (CMake stops without it)
  instead of silently dropping all data

### Fixed (2026-10-19)

- **Start-up hang.** The voice microphone and speaker were opened during
  start-up on the GUI thread; that open waits inside CoreAudio (the
  microphone permission check) and the window never finished starting, so
  the app also never connected to the modem. Voice devices now open on a
  helper thread and only while voice can be used (a voice mode is set,
  the modem is connected, and a session is up, voice is active, or the
  mode is BROADCAST); they close again afterwards, so the microphone is no
  longer live all the time.
- **Modem auto-reconnect.** `modem.reconnect_interval_sec` was read but
  never used: a modem that was not listening at the single start-up
  attempt (both apps launched together) left the companion disconnected
  until CONNECT MODEM was pressed, and a restarted modem was never
  rejoined. With `auto_connect` on, the app now retries every interval
  until connected; a manual disconnect pauses it, and only the first
  failure is logged.

### Changed (2026-10-19)

- **File transfer streams.** The file is sent in 2 KB chunks back to back
  over the modem's reliable, ordered ARQ session instead of 200-byte chunks
  each waiting for an application ACK (10 s timeout, spurious
  retransmits). Flow control follows the modem's TX backlog (`BUFFER`):
  new data is written only while about 8 KB or less is queued. The
  receiver checks size and CRC end to end and answers once (FileAck with
  ok/failed and a reason); one 180 s inactivity timeout replaces
  per-chunk retries. New wire format (FileOffer: payload size + CRC;
  FileChunk: offset + data; FileComplete: size + payload CRC; FileNack and
  FileResume removed); both ends must run this version. Live: 20 KB
  between two modems in ~85 s (was 600-800 B in 100 s)
- Bandwidth family: choosing a family in Settings applies it to the modem
  at once. ULTRA sends `ULTRA ON` then `FAMILY ULTRA`; leaving ULTRA is a
  plain `FAMILY` change. A refusal (e.g. mid-session) reverts the choice
  to the modem's actual family and logs why. On modem connect the app
  reads the modem's saved `FAMILY` instead of overwriting it
- Status bar shows the modem's FSM state (`LINK: CALLING <call>`,
  `CONNECTING`, `DISCONNECTING`, `LISTENING`) and the family/mode the
  modem is actually running
- `LISTEN ON` is no longer forced after every disconnect (the modem
  persists LISTEN)
- Settings tab bar scrolls instead of truncating labels
- Builds with `-Wall -Wextra` for the project's own sources (not
  `thirdparty/`); dead code it reported was removed
- Test hook: `family <NAME>`; `dump` reports the modem's state, family,
  mode and TX backlog; `show chat` closes every floating window as
  documented

### Fixed (2026-10-19)

- File transfers kept running after the radio link ended; every transfer
  in flight now fails ("Link lost") on both sides, and nothing is sent
  unless the station link is up
- `ModemClient::sendData()` wrote unframed bytes to the data port while no
  session was up, where they sat in the modem's TX queue (seen as
  `TXBUF=666` after a disconnect); it now refuses them, and queued session
  data is dropped when the session ends. Mesh relay forwarding uses the
  broadcast path when idle
- Failed and refused calls were silent: `EVT:DISCONNECTED <reason>` is
  reported in plain words ("Call to K1ZZZ failed: No answer from the
  called station"), state changes (calling, incoming call, disconnecting)
  and modem warnings (TX watchdog, CFO, stream replay) are logged
- CONNECT, CQ, BCAST, MYCALL (Settings Apply and on modem connect),
  FREQINFO and FAMILY report modem `ERROR:` answers to the log instead of
  treating "sent" as success
- Idle position broadcasts (main view and Position window) went out as
  `BEACON` plus raw data-port bytes; they now use BCAST (session data
  while linked)
- SNR panel: "Mine" shows the peer's report (modem `RSNR` / `EVT:SNR`
  remote value), "Avg" is a real running average over the session;
  `EVT:SNR <local> <remote>` is parsed in full
- FREQINFO reported a dial frequency no rig had confirmed (e.g. 7055000
  with no radio); only the rig's confirmed frequency is reported,
  otherwise `FREQINFO 0` (unknown)
- "Connected to modem" is logged when the modem connection comes up
- Removed the dead QSY slot control from the main view
- The main view's FILE indicator counted finished transfers as active

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
