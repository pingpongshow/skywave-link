# Scripted test hook

`skywave_link` can run a script of actions through its own code paths, the
same methods the GUI buttons call, so end-to-end tests against real Skywave
modems exercise exactly what an operator would. The GUI still runs; the
script advances from the app's update loop.

```bash
HOME=/tmp/stationA \
SKYLINK_TEST_SCRIPT=scriptA.txt \
SKYLINK_TEST_LOG=scriptA.log \
./build/skywave_link
```

- `HOME` selects the config and data directory, so two instances (two
  stations) can run side by side with their own callsigns, modem ports and
  message stores (`$HOME/.config/skywave-link/config.toml`).
- `SKYLINK_TEST_SCRIPT` is the script: one action per line, `#` comments.
- `SKYLINK_TEST_LOG` (optional) mirrors the `TEST:` lines written to stdout.
- While a script is active, vsync is off: with the display asleep or the
  window occluded the GL swap blocks and the loop would stall. The frame
  rate is still capped by the loop's own delay.

## Actions

| Action | Calls |
|---|---|
| `wait <sec>` | pause |
| `wait_modem [timeout]` | until the modem TCP connection is up (default 60 s) |
| `wait_connected [timeout]` / `wait_disconnected [timeout]` | until the radio link is up / down |
| `connect_modem`, `disconnect_modem` | `connectToModem()` (connects in the background; follow with `wait_modem`), `disconnectFromModem()` |
| `connect <CALL>`, `disconnect` | `connectToStation()`, modem `DISCONNECT` |
| `chat <text>` | `sendChatMessage()` (`0xC0` framing; long text is fragmented); logs `chat not sent` when the line was refused (no link, modem refused) |
| `file <path>` | `sendFile()` |
| `bbs_send <path>` | `sendFile(path, BbsUpload)`: offer the file to the linked station's BBS; logs `bbs_send not started` on refusal |
| `wait_file_offer [timeout]` | until an incoming file offer is waiting for a decision (default 60 s) |
| `file_accept`, `file_decline` | accept / decline the newest offer waiting in the Incoming File prompt (`decidePendingFileOffer()`); offers not answered within 120 s are declined automatically |
| `ping <CALL>` | `sendPing()` |
| `cq_start`, `cq_stop` | `startCQ()` (sends the main panel's x1-x10 repeat count), `stopCQ()` |
| `broadcast <text>` | `sendBroadcast()` |
| `beacon` | `sendBeacon()` |
| `mail <TO>\|<subject>\|<body>` | `sendMail()` |
| `broadcast_mail <subject>\|<body>` | `sendBroadcastMail()` |
| `weather <location>`, `metar <ICAO>` | `requestWeatherForecast()`, `requestMETAR()` |
| `wx_alerts <where>` | `requestWeatherAlerts()`: US alerts for a state code, grid, `lat,lon` or place |
| `wx_cancel` | `cancelWeatherRequest()` (a late reply is then ignored) |
| `away_on`, `away_off` | `enableAwayMode()`, `disableAwayMode()` |
| `save_config` | `saveConfig(true)`; logs `save_config failed` when the file could not be written |
| `email <to>\|<subject>\|<body>` | `queueOutgoingEmail()` (Messages window "Send Email"); logs `email not queued` when it was refused |
| `psk_upload` | `uploadPSKReporterSpots()` (Settings "Upload Now"; still bound by the 5-minute rule) |
| `tune <hz>` | `setOperatingFrequencyFromUi()`; logs `tune failed` when the rig did not move |
| `qsy <band>\|<slot>` | `sendQsySuggestion()` to the linked station (slot defaults to 1) |
| `bbs_upload <path>\|<description>` | `uploadToBBS()` |
| `bbs_announce` | `announceBBSFile()` for every local BBS file |
| `bbs_fetch <OWNER>\|<filename>` | `requestRemoteBBSDownload()` |
| `position <lat>\|<lon>` | `updatePosition()` |
| `family <NAME>` | `setModemFamily()`, as choosing a Bandwidth Family in Settings (WIDE, MID, NARROW, ULTRA; ULTRA sends `ULTRA ON` first; a refusal reverts the setting and is logged) |
| `modem <command>` | a raw modem control command |
| `screenshot <path.ppm>` | the next rendered frame, read back from GL (works with the display asleep) |
| `dump <path.json>` | JSON report, see below |
| `show <window>` | open a window: `settings`, `files`, `bbs`, `frequency`, `messages`, `mail`, `weather`, `position`, `map`; `show chat` returns to the main view and closes every floating window, Settings included |
| `theme dark\|light` | switch the colour theme (as Settings -> UI -> Dark Mode) |
| `quit` | normal shutdown (graceful link teardown, config save) |

## `dump` report fields

| Field | Content |
|---|---|
| `callsign`, `modem_connected`, `link_connected` | station and connection state |
| `modem` | `state` (FSM), `family`, `mode`, `txbuf` (TX backlog bytes), `config_family` |
| `chat` | `from`, `to`, `out`, `bcast`, `text` per line (SYSTEM lines included) |
| `transfers` | `file`, `peer`, `upload`, `bytes`, `total`, `completed`, `failed`, `cancelled`, `error` |
| `mail_inbox`, `mail_outbox` | `from`, `to`, `subject`, `body`, `status` (MessageStatus number), `bcast` |
| `heard_calls` | callsigns of heard CQ calls, newest first |
| `heard_beacons` | `call`, `grid` |
| `ping_results` | `call`, `online` |
| `weather` | `type`, `loc`, `from`, `len` (report length) |
| `bbs_files` | `file`, `remote`, `uploader` |
| `psk` | PSK Reporter client: `active`, `queued`, `sent`, `error` |
| `voice_tx_dropped` | uplink voice frames the modem client refused this session |

## Two-station runs

Each app needs its own modem. Start the modems with
`SKW_CHANMEM_PATH=` (empty: no channel-memory persistence) so test
sessions never read or write the operator's `~/.skywave/chanmem.bin`;
otherwise test peers' link history is stored there and seeds real
sessions' starting modes. Cross the audio devices between the two and
give each its own ports:

```bash
SKW_CHANMEM_PATH= skywave --callsign N0AAA --audio-in "VB-Cable" \
  --audio-out "BlackHole 2ch" --data-port 9400 --ctrl-port 9401 --voice-port 9402 &
SKW_CHANMEM_PATH= skywave --callsign N0BBB --audio-in "BlackHole 2ch" \
  --audio-out "VB-Cable" --data-port 9500 --ctrl-port 9501 --voice-port 9502 &
```

Point each app's `[modem]` ports at its modem (in that station's `HOME`)
and run the two scripts. Give each station its own working directory,
too: relative paths in the config (the BBS folder, downloads) resolve
against it, and two apps sharing one directory see each other's files.
