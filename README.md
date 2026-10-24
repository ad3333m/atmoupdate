# AtmoUpdate

Switch homebrew that updates **Atmosphère**, **hekate** and **firmware** without a PC.

- Pulls releases from GitHub (`Atmosphere-NX/Atmosphere`, `CTCaer/hekate`, `THZoria/NX_Firmware`).
- Atmosphère: installs to the SD card. `package3` and `stratosphere.romfs` are held open by
  Atmosphère while it runs, so they're written as `*.aio` and a reboot through a small payload
  swaps them in, then boots hekate. On Mariko units the payload goes through `/payload.bin`
  temporarily and puts your original back.
- hekate: installs the zip, keeps `hekate_ipl.ini`, and copies the payload to
  `bootloader/update.bin` and `atmosphere/reboot_payload.bin`.
- Firmware: pick any version from the list; it's saved to `/firmware/<version>/` for Daybreak.
- TLS is verified against the Mozilla CA bundle in romfs.

## Controls
D-Pad: navigate. A: update / choose. B: back. Y: refresh. +: exit (disabled mid-install).

## Build
CI only (devkitA64). Pushing to `main` publishes `AtmoUpdate.nro` to the `latest` release and
desktop renders of every screen to the `preview` release (`tools/preview.cpp` draws the real
`source/ui.cpp` with mock data).

## Swap payload licence
`romfs/swap_payload.bin` is built in CI by `tools/build_payload.sh` from
[HamletDuFromage/TegraExplorer](https://github.com/HamletDuFromage/TegraExplorer) at commit
`26f992d378697a406610004d3079a4b3463ed68f` (GPL-2.0), with only the boot banner text changed.
