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
| `connect_modem`, `disconnect_modem` | `connectToModem()`, `disconnectFromModem()` |
| `connect <CALL>`, `disconnect` | `connectToStation()`, modem `DISCONNECT` |
| `chat <text>` | `sendChatMessage()` (long text is fragmented) |
| `file <path>` | `sendFile()` |
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
| `quit` | normal shutdown (graceful link teardown, config save) |
