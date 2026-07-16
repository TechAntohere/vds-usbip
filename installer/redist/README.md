# Bundled third-party installers

Drop the **signed** driver installers here to bundle them into the vDS setup.
If they are absent, the installer skips them and assumes they are already
installed on the machine.

Expected filenames (rename your downloads to match exactly):

- `usbip-win2.msi` — from https://github.com/vadimgrn/usbip-win2/releases
  (the WHLK-signed client package)
- `HidHide.msi` — from https://github.com/nefarius/HidHide/releases

Both are installed silently by the setup:
- `msiexec /i usbip-win2.msi /qn /norestart`
- `msiexec /i HidHide.msi /qn /norestart`

If a release ships as an `.exe` rather than `.msi`, update the `[Run]` silent
flags in `vds.iss` accordingly (HidHide's exe uses Inno `/VERYSILENT`; verify
usbip-win2's installer type and its silent switch).

Check each project's license permits redistribution before shipping a bundle.
