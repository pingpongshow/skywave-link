# Skywave Link Features

This document reflects the current codebase, not the original roadmap.

## User-Facing Features

### Core modem integration

- TCP command/data/voice connectivity through `ModemClient`
- Current default ports: control `9101`, data `9100`, voice `9102` (Skywave defaults, INTERFACES.md)
- Modem state, link state, SNR (local and the peer's report, `RSNR`),
  busy/PTT, family/mode and voice status tracking
- Settings -> Modem: bandwidth family (WIDE/MID/NARROW/ULTRA), `MODE`,
  `MAXMODE` and `CWID`
- Local Hamlib-backed radio control (frequency/mode/PTT) through
  `RadioController`; a Hamlib dry run refuses control and says so
- Local TCP PTT server (port 4532, the modem's default); on modem connect
  the app sends `PTTMETHOD TCP` and `PTTPORT` so the modem keys the rig
  through it, and KEY/UNKEY drive the TX badge

### Chat and QSO workflow

- Live chat with local/remote/system message display and an optional
  timestamp column (`ui.show_timestamps`)
- Chat framing: every line is `0xC0 | UTF-8 text` (`0xC0` is never a valid
  UTF-8 byte, so text can never be mistaken for a control frame); longer
  lines go as `0xFD` fragments and are reassembled. A line that could not
  be sent stays in the input with the reason in the log
- Connect, disconnect, abort, CQ (x1-x10 repeat), broadcast and ping from
  the main control surface
- QSO timer plus local/remote SNR display (the modem's measurements; there
  is no app-level SNR exchange)
- Last-heard beacons and CQ calls on the main screen with the band,
  bandwidth and SNR actually recorded (`-` when unknown)
- Incoming chat/mail plays the system alert sound
  (`ui.sound_notifications`), at most once per burst

### Frequency and QSY management

- Frequency database with calling frequencies and slot spacing per band
- GUI band controls for `80m`, `40m`, `30m`, `20m`, `17m`, `15m`, `12m`, and `10m`
- Slot grid with busy/available/my-slot status
- Channel sniffer (needs CAT control): tunes each slot of the band,
  records busy/clear and returns the rig to its frequency
- Best-slot choice (known clear, then least recently busy, nearest to the
  calling frequency) and band recommendations from stored beacon/ping history
- QSY suggestion, accept, decline, rollback, and reconnect workflow; a QSY
  the radio did not perform is reported, not claimed

### File transfer

- Offer/accept/decline/cancel protocol over the session; the file streams
  in 2 KB chunks with flow control from the modem's TX backlog and an
  end-to-end size and CRC-32 check
- `files.max_file_size` (default 50 KiB) enforced by sender and receiver
- Download-path, auto-accept and trusted-callsign settings; offers that are not
  auto-accepted open an **Incoming File** accept/decline prompt (120 s
  timeout, at most 8 waiting)
- Transfers in flight can be cancelled from the Files window
- File-type labels for images, PDFs, text and log files (ADIF, Cabrillo,
  QSL), audio and archives

### BBS

- Local BBS file hosting with a welcome message sent at session start
- Remote BBS file announcements and listing display
- Remote fetch/download through the file-transfer path
- Uploads to the linked station's BBS ("Upload to remote BBS"); incoming
  uploads are accepted only with `bbs.allow_uploads`, from a station in
  `bbs.allowed_uploaders` and within `max_upload_size`, are stored in the
  BBS folder (never overwriting) and announced

### Mail and messaging

- Store-and-forward message persistence via `MessageStore` (SQLite)
- Mail window with Inbox (default view, unread count on the tab), Sent and
  Compose tabs; Reply fills Compose. The main panel MAIL button (with the
  unread count) opens the Inbox
- Internet mail for this station (POP3, or Winlink mail to its callsign)
  lands in the Mail Inbox; mail for other radio stations is relayed
- Private mail is sent over the link only when the linked station is the
  addressee; otherwise it waits in the outbox (or goes by mesh relay)
- Message priorities: routine, priority, emergency
- Broadcast mail support
- Compose templates for:
  - Free text
  - ICS-213
  - Radiogram
- Mail read-state persistence and delete actions
- Undelivered mail expires after `store.message_expiry_days`

### Email relay and internet-assisted mail

- SMTP outbound relay (TLS), POP3 inbound polling (TLS); the operator name
  is the From display name
- Winlink CMS: B2F over TLS by default (`cms.winlink.org:8773`) with
  secure login (`;PQ` challenge -> `;PR` answer; the account password is
  never sent), FC EM proposals with checksum, LZHUF-compressed blocks.
  The CMS offers at most one proposal block (five messages) per session;
  the rest are fetched on the next poll. Inbound mail is acknowledged
  (removed from the CMS) only after it is stored. Each outbox item gets a
  stable Winlink message id, so a retried send cannot deliver it twice
- Relay allowlist (`email.allowed_relays`): only listed stations may send
  internet email through this station; an empty list admits nobody
- Gateway mail never tears down the operator's QSO; gateway calls are
  capped with backoff
- Winlink-compatible B2F envelope option for radio mail
- Settings -> Winlink tab: CMS enable, host, port, TLS, callsign, password
  (secure-login note), fetch interval, max messages, compatibility mode,
  and the last CMS exchange (time, server, messages or error)
- Weak-signal profile knobs:
  - SNR threshold
  - ACK timeout
  - max retries
  - compression toggle
- HF mesh forwarding controls:
  - enable/disable
  - TTL
  - hop limit
  - dedup window
- Mesh relay: directed relaying through a fresh learned route (routes
  learned from received packets with measured SNR, pruned when stale),
  flooding otherwise; the route table is shown in Settings -> Email
- Relay diagnostics surfaced in the Settings UI
- Relayed chat and mail: one hop, no session-only actions, shown as
  "[relayed via X, sender unverified]" (the mail view shows the relay)

### Position reporting, APRS, and mapping

- Manual latitude/longitude entry
- GPS serial input with device enumeration and live fix display; NMEA
  GGA/RMC with mandatory checksum, a stale fix times out
- Position relay through a connected station
- Position beacon over radio (refused without a fix)
- APRS-IS direct reporting when local internet is available, with a
  server-side filter (default: a range around the station plus its own
  call) and inbound APRS messages shown and acknowledged
- APRS passcode auto-generation from callsign
- Station map with local position plus received station markers
- Source tagging for map entries:
  - Beacon
  - Position Beacon
  - Direct Position
  - APRS

### Weather

Requests go to the linked station, which fetches the data from the internet:

- Forecast requests by location or grid
- Aviation weather:
  - METAR
  - TAF
- Marine weather by zone code
- US weather alerts (api.weather.gov active alerts) by state code, grid
  square, "lat,lon" or place name: Weather -> Forecast -> Alerts
- One request timeout, with cancel (a late reply is ignored) and retry;
  refused or invalid requests get an error answer
- Weather cache persisted in the message store (latest per type and location)

### Voice controls

- Live voice codec selection (`VOICECODEC`: AUTO, 700C, 1300, 2400, 3200)
- Voice mode selection:
  - off
  - connected
  - broadcast
- Capture/playback device selection
- Capture/playback gain controls
- VOICE button: opens a voice session in the configured mode (or says why
  it cannot: no modem, voice off, CONNECTED without a link, BROADCAST while
  linked, link mode below the family's voice floor, voice port closed,
  modem without voice, N0CALL) and closes an open one; label VOICE /
  VOICE... / VOICE ON / VOICE TX / VOICE RX with a tooltip
- Push-to-talk (main panel, Settings -> Voice, configured hotkey): opens a
  session when none is open and keys once it is; every refusal is shown on
  the window's status line and in the log (hotkey messages rate-limited).
  The hotkey works only while the app window has focus, also while a text
  field has focus (the key's character is not typed); focus loss releases
  PTT
- auto_start opens voice whenever the configured mode becomes possible
  (link up / station idle) and stays closed after the operator stops it
- Audio devices open on demand (link up, broadcast mode, start request or
  PTT held); a microphone that delivers only silence (macOS privacy
  settings) is reported
- Voice media state, audio device state, frames/seconds sent, received
  and played in Settings -> Voice; the voice socket is
  reopened when it closes. Uplink audio is queued in whole records; frames
  the modem cannot take are dropped whole and reported (rate-limited)

### Beaconing, away mode, and external reporting

- Periodic beaconing with pending-message notification
- Away mode (runtime toggle in Settings -> Away Mode, never saved) with
  policy controls for message query, email gateway access, and BBS access
- Auto-reply away message; every away request gets an answer
- Away email: the linked station sends `EMAIL:<address>:<subject>:<body>`
  and it goes out through the internet relay when
  `away.allow_email_gateway` is on, an account is configured and the
  station is on the relay allowlist
- PSK Reporter (pskreporter.info) reception reports as IPFIX (RFC 7011):
  - spots need a rig-reported dial frequency and a callsign; SNR only
    when measured; `min_snr` threshold
  - uploads about every 5 minutes (the shortest interval the service
    allows), timed from start-up plus a random offset; "Upload Now" also
    respects the 5-minute minimum
  - a station is reported at most once per 5 minutes, and only once an
    hour while its band and mode are unchanged
  - templates sent in the first three datagrams, then hourly; datagrams
    kept under the MTU; bounded queue
  - Settings shows queued and sent spots, the last upload and the last error

## Persistence and Data

- SQLite-backed message store (SQLite is required)
- Message records with status, retry count, expiry, attachments, and dedup IDs
- Beacon history and ping history persistence (unmeasured SNR stored as NULL)
- Weather cache persistence
- Mail read-state persistence
- Periodic maintenance: expiry, pending-mail cap, history clean-up and
  compaction (`store.auto_cleanup`)
- TOML config save/load for station, modem, voice, radio, APRS, email,
  files, BBS, beacon, away, PSK Reporter, and UI settings; saved on Apply /
  Save in Settings (atomically), account passwords in the system keychain
  where available

## Core Modules Present In Tree

- `MeshRelay`: multi-hop relay queue, route learning, directed relay,
  TTL/hop-limit enforcement, dedup
- `PskReporterClient`: IPFIX encoder and uploader
- `WinlinkTelnetClient`, B2F and LZHUF codecs
- `NotificationSound`: non-blocking system alert sound
- Image decoding helpers used by the station map basemap

## Explicit Non-Features

- ALE is no longer present in runtime code, GUI, or configuration
- No typing indicators (they cost airtime on HF)
- No web server or web UI
- No file-storage fallback without SQLite
- No image preview for file transfers
