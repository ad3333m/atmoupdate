# AtmoUpdate

Switch homebrew .nro that updates **Atmosphère** and **Hekate** without a PC.

- Fetches the latest release from GitHub (`Atmosphere-NX/Atmosphere`, `CTCaer/hekate`).
- Downloads the zip to `sdmc:/switch/AtmoUpdate/` and extracts it to the SD root.
- Mirrors the Hekate payload to `/atmosphere/reboot_payload.bin` after install.
- Remembers installed versions in `sdmc:/switch/AtmoUpdate/versions.ini`.
- TLS verified against the Mozilla CA bundle in romfs.

## Controls
A — Update focused card. Y — Refresh. D-Pad — Switch focus. + — Exit.

## Build
Can't build on Windows; CI builds it. Push to `main` → `AtmoUpdate.nro` goes to the `latest` release.

## Install
Copy `AtmoUpdate.nro` to `sdmc:/switch/AtmoUpdate/AtmoUpdate.nro`, launch from hbmenu.
