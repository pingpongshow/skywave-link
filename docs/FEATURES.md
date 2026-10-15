# Skywave Link Features

This document reflects the current codebase, not the original roadmap.

## User-Facing Features

### Core modem integration

- TCP command/data/voice connectivity through `ModemClient`
- Current default ports: control `9101`, data `9100`, voice `9102` (Skywave defaults, INTERFACES.md)
- Modem state, link state, SNR, busy/PTT, waveform/modulation/code-rate, and voice status tracking
- local Hamlib-backed radio control and frequency/mode/PTT management through `RadioController`
- local TCP PTT server for modem-side `PTTMETHOD TCP` interoperability

### Chat and QSO workflow

- Live chat with local/remote/system message display
- Typing indicator signaling
- Automatic fragmentation and reassembly for longer chat payloads
- Connect, disconnect, abort, CQ, broadcast, and ping flows from the main control surface
- QSO timer plus local/remote SNR display
- Heard-stations and heard-beacons panels on the main screen

### Frequency and QSY management

- Frequency database with calling frequencies and slot spacing per band
- GUI band controls for `80m`, `40m`, `30m`, `20m`, `17m`, `15m`, `12m`, and `10m`
- Slot grid with busy/available/my-slot status
- Channel sniffer for slot activity scanning
- Best-slot and best-band recommendation scoring from stored beacon/ping history
- QSY suggestion, accept, decline, rollback, and reconnect workflow

### File transfer

- File offer/accept/reject/cancel protocol
- 50 KiB transfer cap
- Chunked transfer, ACK/NACK, retry, and resume handling
- CRC32 integrity verification
- Download-path and auto-accept settings
- File-type labeling for images, PDFs, text, audio, archives, and `.c2v` voice-message files

### BBS

- Local BBS file hosting
- Optional remote uploads
- Remote BBS file announcements and listing display
- Remote fetch/download flow through the existing file-transfer path
- Welcome-message and storage-directory settings

### Mail and messaging

- Store-and-forward message persistence via `MessageStore`
- Radio mail inbox/sent views
- Message priorities: routine, priority, emergency
- Broadcast mail support
- Compose templates for:
  - Free text
  - ICS-213
  - Radiogram
- Mail read-state persistence and delete actions

### Email relay and internet-assisted mail

- SMTP outbound relay settings
- POP3 polling settings for inbound fetch
- Winlink CMS Telnet delivery/fetch settings
- Winlink-compatible B2F envelope option
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
- Relay diagnostics surfaced in the Settings UI

### Position reporting, APRS, and mapping

- Manual latitude/longitude entry
- GPS serial input with device enumeration and live fix display
- Position relay through a connected station
- Position beacon over radio
- APRS-IS direct reporting when local internet is available
- APRS passcode auto-generation from callsign
- Station map with local position plus received station markers
- Source tagging for map entries:
  - Beacon
  - Position Beacon
  - Direct Position
  - APRS

### Weather

- Forecast requests by location or grid
- Aviation weather:
  - METAR
  - TAF
- Marine weather by zone code
- Request timeout, cancel, and retry handling
- Cached weather-report display with type/source/timestamp metadata

### Voice controls

- Live voice codec selection
- Voice mode selection:
  - off
  - connected
  - broadcast
- Voice family selection:
  - auto
  - wideband
  - narrowband
- Capture/playback device selection
- Capture/playback gain controls
- Push-to-talk button and focus-scoped hotkey capture
- Voice transport status and queue counters in the UI

### Beaconing, away mode, and external reporting

- Periodic beaconing with pending-message notification
- Away-mode policy controls for message query, email gateway access, and BBS access
- Auto-reply away message
- PSK Reporter uploads with antenna, SNR threshold, and upload interval settings

## Persistence and Data

- SQLite-backed message store when built with SQLite
- Message records with status, retry count, attachments, and dedup IDs
- Beacon history and ping history persistence
- Weather cache persistence
- Mail read-state persistence
- TOML config save/load for station, modem, voice, radio, APRS, email, files, BBS, beacon, away, PSK Reporter, and UI settings

## Core Modules Present In Tree

These modules are implemented in `skywave_link_core`, but they are not all exposed as first-class GUI features today:

- `MeshRelay`: multi-hop relay queue, route learning, TTL/hop-limit enforcement, dedup
- `MessageRouter`: handler registry plus prioritized outbound queue
- `ImagePreviewCache`: image decode/cache utility used by the map/basemap/image helpers
- `VoiceMessage`: `.c2v` header helpers and codec metadata utilities
- `WebServer`: lightweight HTTP/WebSocket server with optional token auth

The current GUI app does not launch the web server and does not expose a dedicated web UI.

## Explicit Non-Features

- ALE is no longer present in runtime code, GUI, or configuration
- The old documentation references to an email tab separate from Mail are stale
- The old documentation references to file-storage fallback without SQLite are stale
- The old documentation references to image preview as a GUI file-transfer feature are stale
