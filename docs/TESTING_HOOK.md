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
| `chat <text>` | `sendChatMessage()` (long text is fragmented) |
| `file <path>` | `sendFile()` |
| `wait_file_offer [timeout]` | until an incoming file offer is waiting for a decision (default 60 s) |
| `file_accept`, `file_decline` | accept / decline the newest offer waiting in the Incoming File prompt (`decidePendingFileOffer()`); offers not answered within 120 s are declined automatically |
| `ping <CALL>` | `sendPing()` |
| `cq_start`, `cq_stop` | `startCQ()`, `stopCQ()` |
| `broadcast <text>` | `sendBroadcast()` |
| `beacon` | `sendBeacon()` |
| `mail <TO>\|<subject>\|<body>` | `sendMail()` |
| `broadcast_mail <subject>\|<body>` | `sendBroadcastMail()` |
| `weather <location>`, `metar <ICAO>` | `requestWeatherForecast()`, `requestMETAR()` |
| `bbs_upload <path>\|<description>` | `uploadToBBS()` |
| `bbs_announce` | `announceBBSFile()` for every local BBS file |
| `bbs_fetch <OWNER>\|<filename>` | `requestRemoteBBSDownload()` |
| `snr` | `exchangeSNR(true)` |
| `position <lat>\|<lon>` | `updatePosition()` |
| `modem <command>` | a raw modem control command |
| `screenshot <path.ppm>` | the next rendered frame, read back from GL (works with the display asleep) |
| `dump <path.json>` | report: chat, transfers, mail in/out, heard stations and beacons, ping results, weather, BBS files, modem/link state |
| `show <window>` | open a window: `settings`, `files`, `bbs`, `frequency`, `messages`, `mail`, `weather`, `position`, `map` (`chat` returns to the main view) |
| `theme dark\|light` | switch the colour theme (as Settings -> UI -> Dark Mode) |
| `quit` | normal shutdown (graceful link teardown, config save) |

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
