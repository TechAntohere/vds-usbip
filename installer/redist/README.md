# Bundled third-party installers

Drop the **signed** driver installers here to bundle them into the vDS setup.
If they are absent, the installer skips them and assumes they are already
installed on the machine.

Expected filenames (rename your downloads to match exactly):

- `usbip-win2.exe` — from https://github.com/vadimgrn/usbip-win2/releases
  (the WHLK-signed x64 client, e.g. `USBip-0.9.7.8-x64.exe`). Inno Setup based.
- `HidHide.exe` — from https://github.com/nefarius/HidHide/releases
  (x64, e.g. `HidHide_1.5.230_x64.exe`). Advanced Installer based.

Both are installed silently by the setup, in this order:
- `usbip-win2.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`
- `HidHide.exe /exenoui /qn`

These binaries are intentionally git-ignored — they are fetched per build, not
committed. Check each project's license permits redistribution before shipping.
