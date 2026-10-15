# Porting Notes: Ancestor App → Skywave Link

This application began life as the companion GUI for a different HF modem.
This document records what had to change to talk to Skywave, so that a
future reader can tell a deliberate adaptation from an accident.

Everything below is written against the modem's own documentation in
`../../skywave/docs/`.

## 1. Naming

| Before | After |
|---|---|
| `RIA Link` / `RIACodex` | `Skywave Link` |
| `ria_link`, `ria_link_core`, `ria_link_emulator` | `skywave_link`, `skywave_link_core`, `skywave_link_emulator` |
| namespace `riacodex` | namespace `skylink` |
| `include/riacodex/` | `include/skywave_link/` |
| `RIACODEX_*` CMake options / defines | `SKYWAVE_LINK_*` |
| `~/.config/ria-link/config.toml` | `~/.config/skywave-link/config.toml` |
| PSK Reporter mode string `RIA` | `SKYWAVE` |

## 2. Transport

| | Before | After |
|---|---|---|
| data port | 8300 | **9100** |
| control port | 8301 | **9101** |
| voice port | 8302 | **9102** |

Configs carrying the old ports (in either command/data order) are migrated on
load by `normalizeLegacyModemPorts()`.

The voice socket is now **optional**. A Skywave build without `ENABLE_VOICE`
never binds the port, and the link emulator has no voice path at all;
previously a missing voice port failed the entire connection. `ModemClient::
hasVoiceMedia()` reports whether media is available.

## 3. Control-protocol dialect

The ancestor modem answered commands with `OK` and pushed notifications
prefixed `NOTIFY:`. Skywave does neither.

| Concern | Before | After |
|---|---|---|
| success | `OK` | the response **echoes the verb** (`LISTEN ON`, `CONNECT W1AW`) |
| failure | (varied) | `ERROR:UNKNOWN` (typo), `ERROR:UNSUPPORTED` (feature compiled out), `ERROR:STATE` (wrong FSM state), `ERROR:BADARG` |
| notifications | `NOTIFY:CONNECTED`, `NOTIFY:STATE`, … | `EVT:CONNECTED <call> <family> <Mx>`, `EVT:STATE`, `EVT:DISCONNECTED <reason>`, `EVT:MODE`, `EVT:BUSY`, `EVT:TXCOMPLETE`, `EVT:FILE_*`, `EVT:VOICE` |
| monitor | `MONITOR:<src>:<dst>:<type>:<level>:<hex>` | `MON:<src>:<dst\|*>:<frame_type>:<family>:<Mx>:<hex>` |

`sendCommandExpectOk()` became `sendCommandOk()`, which treats any non-`ERROR:`
response as success.

### Command mapping

| Before | After |
|---|---|
| `WAVEFORM`, `MODULATION`, `CODERATE`, `AUTOMODE` | `MODE <M4..M14\|AUTO>`, `MAXMODE`, `CPMODE`, `HARQ` |
| `LADDER STANDARD\|NB1000` | `FAMILY WIDE\|MID\|NARROW\|ULTRA` |
| `VOICEMODE` | `VOICE OFF\|CONNECTED\|BROADCAST` |
| `VOICEFAMILY AUTO\|STANDARD\|NB1000` | *(removed — Skywave has no voice family)* |
| `BEACONDATA <hex>` | `BCAST <hex>` |
| `COMPRESSION ON\|OFF` | *(removed — zstd is negotiated per session; `STATUS` reports `COMPRESS`)* |
| — | `MONITOR ON\|OFF` is now required to hear third-party traffic |
| — | `STATUS` / `STATUS JSON`, `AUDIOLEVEL`, `FREQINFO` are new |

### Status and SNR

Skywave has no periodic SNR notification. Link quality reaches the UI only
through polling, so the app's 500 ms `STATE` poll became a `STATUS` poll,
whose single line carries state, remote call, family, mode, net rate, SNR,
remote SNR, TX alignment, retry ratio, busy, listen, compression, crypto
epoch, masked carriers and frame counters.

The modem also emits `EVT:SNR` on a ≥1 dB change while connected (see §6),
which `ModemClient` consumes; the poll remains as a floor so a client that
attaches mid-session still gets a full snapshot immediately.

## 4. Monitor and broadcast traffic

This is the largest behavioural difference.

The ancestor modem delivered decoded application payloads on its monitor
surface. Skywave's monitor surfaces carry the **raw link frame** —
12-byte envelope, body, CRC-32 — in both forms:

* `MON:` control lines, carrying the whole frame as hex (an earlier 64-byte
  cap is gone — see §6 — but the truncated-frame path is kept so the client
  still works against a modem that predates the fix);
* `0xFB` binary records on the data port, which are complete but are emitted
  **only while no session is CONNECTED**, and whose "payload" field is the
  whole link frame rather than a decoded body.

`src/protocol/skywave_protocol.cpp` therefore implements the small amount of
Skywave wire knowledge the app needs: the frame-type table, envelope parsing
with CRC verification (and an explicit truncated-frame path for `MON:`
lines), the `BEACON` / `CQ` bodies, and the `BCAST_DATA` 4-bit fragment
chain. `tests/test_skywave_protocol.cpp` pins all of it.

Consequences for the app:

* `BEACON` and `CQ` now carry **only station metadata** (callsign, noise
  floor, family). Operator payloads ride `BCAST_DATA`.
* Broadcast application messages arrive through the new
  `ModemClient::onBroadcast` callback once the fragment chain completes, and
  are handled by `App::onBroadcastMessage()`. `App::onMonitorFrame()` no
  longer tries to read app payloads out of beacons.
* The data-port reader demultiplexes `0xFB` records from payload bytes,
  following the modem's own rule: while a session is connected every byte is
  payload.

The modem now also surfaces its own reassembly as `EVT:BROADCAST`; the
client prefers that and keeps `BroadcastReassembler` for older modems and
for the binary-record path.

## 5. Configuration schema

* `[modem] ladder = "STANDARD"|"NB1000"` → `[modem] family = "WIDE"|"MID"|"NARROW"|"ULTRA"`
* `[voice] family` removed — Skywave selects the Codec2 rate from the live
  ladder mode and steps it down when AMC demotes (`VOICE.md`)
* voice codec labels are now the Codec2 rates `700C` / `1300` / `2400` / `3200`
* `NoiseFloorReading` lost `bandwidth_hz` and `threshold_dbfs`; Skywave's
  `NOISEFLOOR` reports a min-tracked floor level and a busy flag only

## 6. Modem-side gaps found while porting — now closed

These were all observations about the modem, not app bugs. Each was fixed
in the modem's post-v1.0.0 hardening pass (see its ROADMAP deviations log),
and the app now depends on the fixed behaviour:

* `FREQINFO <hz>` was documented as the companion's way to push the dial
  frequency into the modem's channel-memory buckets, but the dispatcher
  discarded the argument. It works now, and `App::update()` reports the
  rig's dial frequency whenever it changes.
* `EVT:SNR` and `EVT:CFO_WARN` were documented and never emitted. Both are
  emitted now — SNR on a ≥1 dB change while connected — so link quality no
  longer depends solely on the `STATUS` poll. The poll is kept as a floor.
* The `MON:` payload was silently capped at 64 bytes, which put the frame's
  CRC-32 out of reach. Whole frames are emitted now, so
  `parseSkywaveFrame()` verifies the CRC on real traffic rather than
  falling back to its truncated path.
* The `0xFB` record's byte 12 onward is now documented as what it always
  was: the raw link frame.
* `EVT:BROADCAST <src> <hex>` is new — the modem's own reassembly of a
  `BCAST_DATA` chain, which it previously built and discarded. Unlike the
  data port's binary records it keeps flowing during a session, so the
  client prefers it and keeps `BroadcastReassembler` as the fallback for
  older modems.

## 7. What the emulator now does

`skywave_link_emulator` was taught the same dialect: verb-echo responses,
`EVT:` notifications, a Skywave-shaped `STATUS` line, `BCAST`, and
`ERROR:UNKNOWN` / `ERROR:UNSUPPORTED` for unknown and gated verbs. It still
routes application data over the data sockets rather than synthesising
monitor records, so it exercises the connected paths, not the passive-monitor
path.
