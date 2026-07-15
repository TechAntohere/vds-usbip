# VDS USB/IP Port — Session Handoff (living doc, update after every confirmed fix)

## Goal
Port vds (Windows side — vds is cross-platform, we ONLY care about the Windows path here) to use **USB/IP (usbip-win2, UDE-based client)** as the virtual-device transport instead of the unsigned kernel drivers (`vds_usb.sys`, `vds_filter.sys`), so it ships without test-signing mode. Transport-layer port only — vds's PCM/Opus/report logic is reused untouched.

## What vds actually is (read this before touching anything)
vds is a **wireless bridge**: it emulates a full **USB DualSense** device that games/Windows talk to, and forwards everything to the **real DualSense over Bluetooth** (which gets hidden — HidHide / vds_filter.sys — so games see only the virtual one). The BT controller already supports haptics/audio/triggers; Windows' BT HID stack just doesn't expose them. vds's virtual USB device does.
- **4-channel USB audio OUT** (Format Type I, 4ch, 16-bit, 48kHz, isoc OUT wMaxPacketSize=392 @1ms): ch0/1 = stereo speaker/headphone (stays stereo, Opus-encoded → BT report 0x36-family), ch2/3 = left/right **haptics actuators** (resampled, summed L/R separately). Games hook onto exactly these endpoints. THIS IS THE PRIORITY PATH.
- **Mic** = independent interface 2, own isoc IN endpoint, own BT input report. Currently STRIPPED from descriptors (see below), deprioritized. Re-enabling later = un-strip descriptor, mic plumbing code all still in place.
- HID interrupt IN/OUT = controller input/output reports (buttons/rumble/lightbar via extended report 0x31 over BT).

## Environment / repos (same Windows dev box via Windows-MCP PowerShell)
- Repo: `C:\Users\Antonio\Documents\vds`
- usbip-win2 source (the client we serve; BSD-2): `C:\Users\Antonio\Documents\usbip-win2`
- Go USB/IP device-side reference (round-trip-tested wire structs): `C:\Users\Antonio\Documents\usbip-virtual-device` (github.com/ntchjb/usbip-virtual-device)
- Ground-truth DualSense refs (NOT part of vds): `C:\Users\Antonio\Documents\New project\CupheadDualSenseMod` (BT speaker audio = Opus inside HID report 0x36; 0x31 = rumble/lightbar), `DualSenseBtProbe` (working live mic-in test, never opened yet)
- usbip.exe CLI: `C:\Program Files\USBip\usbip.exe` — its `list -r`/`port` output is unreliable/hangs; prefer `vdsctl` + `Get-PnpDevice` + checking TCP 3240 ESTABLISHED to PID 4 (System = vhci driver) via `Get-NetTCPConnection`.
- Debug log: `C:\ProgramData\vDS\` (NOT build\ or %TEMP%).
- Build: locate vcvars64.bat dynamically (path has drifted between BuildTools and Community across sessions — `Get-ChildItem 'C:\Program Files*\Microsoft Visual Studio' -Recurse -Filter vcvars64.bat` first), configure with `-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake` (Opus). Stop vdsd.exe before relinking (file lock).

## PROGRESS (state at last update: 2026-07-14)
| Component | % | Note |
|---|---|---|
| USB/IP handshake + control transfers + descriptors | 98 | Validated vs real usbip-win2 |
| USB speed field | 100 | 2=FULL 3=HIGH; we send HIGH |
| HID interrupt IN/OUT | 90 | **Interactive input USER-VERIFIED (2026-07-15)** via joy.cpl Test panel — buttons + both sticks respond correctly through the full USB/IP path. |
| Double-controller (HidHide gap) | confirmed | joy.cpl lists TWO "Wireless Controller" both Status OK (virtual USB + raw BT). Visual proof HidHide is needed. User: "hid hide is needed". Deferred, not a blocker. |
| BT discovery (plain-HID, no driver) | 95 | SetupAPI+hid.dll+CM_Get_Parent devtree walk |
| `online` status detection | FIXED | Was `filter_backed && ...` (always false on plain-HID path); now uses `bluetooth_connected`. Landed + built. |
| Audio Control iface (MI_00) startup | FIXED | See bug log #11-12. Speaker endpoint enumerated Status:OK for the first time. |
| Mic | stripped | Streaming alt-setting removed from DS5+DSE descriptors; iface 2 alt 0 reclassified vendor-specific 0xFF/0xFF (MI_02 shows Error = expected/harmless, no driver). |
| Speaker audio audible on controller | **CONFIRMED (user-verified)** | First time ever. 440Hz test tone heard through the controller speaker via the full USB/IP→pipe→vdsd→Opus→BT chain. |
| Audio quality (stutter/crackle) | **FIXED (user-verified "clean now")** | Bugs #17+timer fix. Volume slider effect (#18) still needs user verification. |
| Mic IN endpoint enumerates + stable | ✅ DONE (2026-07-15) | Descriptor restored from pristine .bak (full mic topology, DS5+DSE, 227B). Enumerates as "Headset Microphone" Status OK, MI_00 stays OK, connection SURVIVES the mic probe (the exact thing that crashed every prior session — bug #16 fix carried it). Committed 100bc12. |
| Mic capture quality | ✅ DONE (user-verified 2026-07-15) | Was stuttery/unintelligible. ROOT CAUSE: isoc IN completed instantly → Windows resubmitted ~10x real-time (measured recv/req ratio 0.09) → drained faster than BT mic filled → ~90% zero-pad. FIX (commit 2702539): (1) pace mic IN completions to real time through the same pacer as audio OUT, separate iso_in_next_due anchor → ratio now ~1.0, primed, stable. (2) mic jitter buffer (40ms prime cushion, re-prime on underrun, 120ms cap) absorbs 10ms-burst vs 1ms-pull. (3) digital mic gain VDS_MIC_GAIN (default 50x!) — the DualSense mic is absurdly quiet (known community issue), user-tuned 4→8→25→30→40→50 on hardware, 50x = usable conversational level. Also gated hot per-URB debug logging behind VDS_USBIP_DEBUG. NOTE: 50x is pure linear gain — loud sounds hard-clip; a soft-knee limiter is the proper refinement if distortion bothers (not requested yet). |
| Haptics felt on controller | **CONFIRMED (user-verified)** | User felt the actuators. Noted "buzz was not smooth" — partly expected (raw 440Hz sine feels rough on voice-coil actuators), but check completion pacing/underruns if real game haptics feel choppy. Polish item, not a blocker. |
| Pipe demux (read side) | landed | pipe_reader_thread + per-type queues + take_mic_bytes(count) w/ clamping+zero-pad. Built clean. |
| Pipe write mutex (vdsd side) | landed | virtual_write_mutex threaded through both loops + 5 write sites. |
| Worker relaunch on live re-attach | FIXED | Root cause was tcp_thread.join() deadlock (stop() never unblocked accept()/recv()), plus a std::terminate trap (pipe_reader_thread reassigned while joinable on re-import). Both fixed; auto re-import after a client-side detach observed working live. |
| HidHide swap / installer | 0 | Deprioritized. Real BT pad currently NOT hidden — known correctness gap. |
## Key files
- `src/platform/win32/vds_usbip.cc/.hh` — the USB/IP server (VirtualPort). All wire protocol + per-URB CMD_SUBMIT handling + the pipe demux (Impl: pipe_reader_thread, hid_in_queue, feature_reply_queue, mic ring, wait_for_* fns). RET_SUBMIT byte-order block near line ~945-965.
- `src/platform/win32/vdsd.cc` — daemon/supervisor. usbip_transport_enabled() (env VDS_TRANSPORT=usbip), run_bridge_session branches transport, handle_virtual_frame/handle_bluetooth_frame + virtual_write_mutex, worker-launch loop, `online` computation (~line 549).
- `src/platform/win32/vds_bt.cc/.hh` — HidBluetoothTransport + plain-HID discovery (list_hid_bluetooth_devices, find_connected_bluetooth_device — the 2-arg overload needed its own plain-HID fallback, fixed).
- `src/vds_protocol.cc` — PcmAudioExtractor (untouched, working): ch0/1→speaker_pcm_→Opus 200B/480-frame chunks; ch2/3→resample→left/right actuator sums.
- `include/vds/ds5_usb.h` — byte-exact captured descriptors, BOTH DS5 (PID 0x0ce6) and DSE (0x0df2) arrays. Mic-stripped as above. Backup exists (.bak from strip session).
- `src/vds_io.hh` — raw Frame read/write, zero routing by design (demux lives above it).
- usbip-win2 `drivers/wsk/wsk_receive.cpp` — patch_config(): client silently rewrites isoc/interrupt bInterval (+3, capped) because UDE treats everything as High-Speed; validates ISO IN: compacted buffers, header actual_length == SUM(per-packet actual_length), offsets echoed from request. No USB Audio Class awareness anywhere — descriptor startup failures are always usbaudio.sys-side.

## Bug log (all confirmed live unless noted)
1-10: see git history / earlier sessions — winsock include order; unique_handle include; BT path format (BTHENUM parent walk); worker-launch filter gate; empty string descriptors; USB speed backwards; missing UAC Feature Unit GET/SET (biggest early unlock); iso descriptor byte-order double-conversion; bogus pacing sleep removed; WASAPI test-tool float/int bug.
11. Mic isoc IN actual_length unclamped → wire violation → client reset TCP → whole composite died (mic "collateral kill"). Fixed via take_mic_bytes clamp + then descriptor strip.
12. MI_00 (Audio Control) ProblemCode 10 after mic strip: NOT dangling AC entities (that theory was byte-walk-DISPROVEN — no Terminal 4/FU 5/Terminal 6 to strip; do not re-chase). Real cause: iface 2 alt 0 still declared AudioStreaming class with no endpoints/terminal. Fix: reclassify alt 0 to vendor-specific 0xFF/0xFF. Confirmed: MI_00 OK, Speakers endpoint enumerated.
13. RET_SUBMIT `number_of_packets` never hton32'd (status/actual_length were). HID replies use -1 (0xFFFFFFFF, byte-order symmetric) so it hid all session; first real isoc OUT (10 packets) sent garbage in a validated field → client reset after reply #1. Fix landed (+ start_frame/error_count explicitly zeroed/converted), validated against Go reference structs (all BigEndian). Built clean. RETEST PENDING.
14. `online:false` while pad connected: computed from filter_backed (always false, plain-HID path). Fixed → bluetooth_connected. Built clean.
15. FIXED — worker relaunch on live re-attach. Two bugs: (a) stop() deadlocked on tcp_thread.join() — tcp_server_loop blocked in accept()/recv() with nothing unblocking sockets (CancelIoEx only touched the pipe). Fix: stop() shutdown()s the published client_socket and pokes accept() with a throwaway loopback connect. (b) handle_import_session reassigned pipe_reader_thread on every import — assigning over a joinable std::thread calls std::terminate(). Fix: start reader at most once per VirtualPort (pipe + reader survive client reconnects). Plus reader_exited flag + CancelIoEx retry loop in stop() to close the cancel-vs-read race.
17. FIXED — audio stutter/crackle root cause: instant ISO OUT completion made usbaudio.sys stream ~5x real-time (measured: 96s of PCM in a 20s tone; vdsd dropped 62% of chunks as stale, each drop an audible discontinuity). Fix: dedicated pacer thread in vds_usbip.cc completes each audio RET_SUBMIT at its real-time due moment (1ms/iso packet, anchored chain, 250ms idle reset) — the USB/IP equivalent of windrv's VdsCompleteUrbAfterDelay. Session thread never blocks (HID unaffected); all socket sends now serialized via socket_send_mutex. Plus timeBeginPeriod(1) on the pacer (wait_until otherwise quantizes to ~15.6ms system tick — same reason vdsd has HighResolutionSleeper). RESULT: dropped 6044→0, underflow 7→~1(startup), user-confirmed clean by ear.
18. ADDED — Feature Unit volume now actually applied: SET_CUR mute/volume stored (fu_muted/fu_volume_db256 → speaker_gain_q15), GET_CUR echoes stored values, gain applied to ch0/1 ONLY (haptics ch2/3 deliberately not scaled) before forwarding PCM to vdsd. Windows volume slider CONFIRMED working by user (2026-07-15).
16. FIXED — **THE audio killer, invisible for the whole project**: CMD_SUBMIT receive order was descriptors-then-data; real USB/IP layout is header → transfer buffer (OUT) → iso descriptors (verified in BOTH usbip-win2 libdrv/pdu.cpp get_isoc_descr() AND usbip-virtual-device command/submit.go). Same byte total (3840+160) so the stream never desynced — it just parsed the first 160 B of PCM as descriptors and forwarded byte-shifted PCM. Silence priming URBs → zero garbage descriptors → replies passed validation → "first 2 URBs always fine." First real audio → nonzero garbage → echoed reply actual_length garbage → client check() fail → recv thread exit → async_detach_and_delete(reattach=true) → socket close + AUTO RE-IMPORT (that reconnect is usbip-win2 by design, not our doing). After fix: 1637 ISO OUT URBs streamed, 0 BREAKs, connection stable.

## Working style (hard-won, follow these)
- READ SOURCES FIRST (vds README-WINDOWS.md, usbip-win2, Go reference) before forming theories. Every wasted hour this project came from guessing.
- Test directly; ask the user only simple yes/no confirmations ("hear a tone now?").
- Long-running daemon/attach → `Start-Process powershell` in its own window, never synchronous.
- ALL file edits via PowerShell. **.NET IO (WriteAllText/WriteAllLines) MUST get absolute paths** — relative paths silently write elsewhere (caused multiple "phantom edit / reverted itself" mysteries). Get-Content/Set-Content resolve via provider and are fine.
- CRLF vs LF: normalize before exact-string matching; here-string mismatches fail silently.
- Verify every edit landed: re-read the file + check LastWriteTime before rebuilding.
- ETW/WPP tracing on usbip-win2 = dead end, don't retry. fprintf app-level logging wins.
- UAC elevation headless doesn't work — avoid admin-requiring paths.

## Useful protocol knowledge (hard-won, saves re-derivation)
- usbip-win2 IGNORES the direction field in RET headers — it derives direction from the seqnum (extract_dir; seqnum parity encodes it client-side).
- Client ret_submit validation failures only fail that one URB; what KILLS the connection is recv-loop exit (socket error, invalid seqnum, or payload-recv/prepare_wsk_mdl failure like check(TransferBufferLength, actual_length)).
- On recv-loop death the client calls async_detach_and_delete(reattach=true) → closes socket → auto re-imports (fresh session, seqnum restarts ~1). A surprise "seqnum=3 enumeration" in the log = the client reset us and reconnected.
- RET_SUBMIT ISO OUT payload = iso descriptors only (no data). ISO IN = data (compacted, no gaps) + descriptors. Header actual_length: OUT = sum of per-packet actuals; IN = sum of transmitted bytes.
- patch_config() client-side rewrites isoc/interrupt bInterval (+3, capped) because UDE treats all devices High-Speed.
- fill_isoc_data skips ALL per-packet validation for OUT (only sets Status) — garbage descriptors in OUT replies fail at check() on actual_length, not per-packet.

## Standard test sequence
1. `Stop-Process -Name vdsd -Force` if running
2. Start fresh via `Start-Process powershell -ArgumentList '-ExecutionPolicy','Bypass','-File','C:\Users\Antonio\Documents\vds\run_vdsd.ps1' -WindowStyle Minimized`
3. `vdsctl list` (address may already be registered) — after a reboot the vhci import is gone: run `& 'C:\Program Files\USBip\usbip.exe' attach -r 127.0.0.1 -b 1-1` (this DOES work non-elevated and does NOT hang, unlike `usbip list -r`)
4. Wait ~5-8s for enumeration
5. `Get-PnpDevice` for composite (VID_054C&PID_0CE6): HID + MI_00 should be OK; MI_02 Error is expected (vendor-specific stub)
6. `Get-PnpDevice -Class AudioEndpoint` → "Speakers (DualSense Wireless Controller)" Status OK
7. Tail `C:\ProgramData\vDS\usbip_cmd_debug.log` for BREAK/send-fail signatures. Read it with .NET FileStream + FileShare::ReadWrite and byte-seek (Get-Content -Tail hangs on the live-locked file). Note: cmd entries are separated by LITERAL "\n" text, not newlines — split on that.
8. Real audio test: `C:\Users\Antonio\Documents\vds\wasapi_test_tone.exe` (auto-targets the DualSense render endpoint, writes 440Hz to all 4 channels — so it exercises speaker ch0/1 AND haptics ch2/3 at once). Success = tool completes, zero new BREAKs, TCP 3240 still Established, audio OUT URB count in the hundreds+.

## Recently fixed (2026-07-15 EOD)
- **Mic mute LED** ✅ (commit 2308b58): build_bt_mic_state_report hard-coded the mute-LED byte (offset 8) to 0; now `muted ? kMuteLedOn(1) : 0`. Shared vds_protocol.cc so fixes Linux too. User-verified.
- **System-wide 1fps video stutter on mic toggle** ✅ (commit 2916d37): speaker OUT + mic IN shared one FIFO pacer queue; mic-start burst of IN URBs (dues up to ~1s ahead) head-of-line-blocked speaker replies → speaker audio clock stalled → browser video slaved to that endpoint dropped to ~1fps. Fixed by ordering the pacer queue by due time (std::multimap) so speaker-due-now always precedes mic-due-later. User-verified smooth.

## Integration done (2026-07-15)
- **HidHide** ✅ (commit 2b9d802): `hidhide_setup.ps1` whitelists vdsd + hides the BT DualSense (dynamic discovery via `HidHideCLI --dev-gaming`, matches product=DualSense & BTHENUM base container); `hidhide_teardown.ps1` reverses. joy.cpl now shows ONE controller (the virtual one); vds still works (whitelisted). NOTE: HidHide hides by device instance path — stable per paired device, but the installer should re-run setup (dynamic find) rather than hard-code paths. HidHideCLI at `C:\Program Files\Nefarius Software Solutions\HidHide\x64\`.
- **Auto-attach** ✅ (commit 2b9d802): `usbip port --stash` marks the device persistent (vhci driver auto-reattaches at boot, retrying until vdsd is up — persistent list: 127.0.0.1:3240/1-1). `vds-autostart.ps1` (registered as a Startup-folder shortcut "vDS AutoStart.lnk") starts vdsd + attaches at logon, idempotent, logs to C:\ProgramData\vDS\autostart.log. Verified via simulated cold boot (detach+kill → run script → device returns on its own).

## Emulated DualSense fidelity (2026-07-15) — ~95%
Complete for anything real software does: byte-exact descriptors; FULL input report (buttons/sticks/triggers/gyro/accel/touchpad/timestamp); output (rumble/lightbar/trigger FX/player LED/mute LED); audio out+in+haptics+volume; feature reports incl. firmware/factory (0x20). Command-channel diagnostics partly work after the cache fix (commit 020cc63): 0x80/0x81 (and 0x84/0x85) are dynamic command req/resp and must NOT be cached (was caching 0x81 → stale → daidr factory/diagnostic fields blank). Now command-channel (>=0x80) always reads fresh from BT. Result: "some, not all" daidr factory fields populate — remaining blanks (some barcodes, precise battery voltage) are values the physical controller likely doesn't expose over Bluetooth; read-only, zero gameplay impact. Not worth deeper command-protocol bridging.

## WebHID / feature-report fix (2026-07-15, commit b984606)
- HID GET_REPORT (feature) reply was forwarded from BT verbatim without clamping to wLength. A WebHID app (daidr DualSense tester) reading firmware report 0x20 got a reply larger than requested → control-IN actual_length > transfer_buffer_length → usbip-win2 reset the whole connection (all interfaces incl. audio dropped, persistent-reattach restored = "endpoints go down then back up"). Fixed: clamp feature-GET reply to wLength + defensive clamp on all control-IN. daidr now stays connected and renders properly (user-verified).
- NOTE the general rule: ANY control-IN reply must be <= wLength or usbip-win2 resets. Descriptors/string/FU already clamp; keep any new control-IN path clamped.

## Known cosmetic: browser shows two gamepads
- After enabling HidHide mid-session, the browser Gamepad API / Windows.Gaming.Input still lists the (now-hidden) BT controller from its system cache until a controller re-plug or reboot. DirectInput (joy.cpl, most native games) already shows one. On a NORMAL boot (HidHide + autostart active before the controller connects) only the virtual one enumerates, so this self-resolves on next reboot. Re-plugging the DualSense clears it immediately. Not a code bug.

## UI plan (next major piece — tasks #12, #13)
- Stack chosen: **C# / .NET WPF** (.NET 10 SDK present). User has a Figma mockup (dark "Dualsense / Connected" card with battery), uses static gradient assets, wants fade animations.
- User decisions captured: Player LED set ONCE on connect (1st controller=1 light, 2nd=2-player pattern), then left to games. Lightbar OFF on connect (only player LED). Settings window: mic gain + mute, speaker volume, haptic volume. Plus a small fade popup on headphone-jack plug/unplug and mic state change. Connection popup (bottom-right) on connect: battery, status, live polling rate.
- BLOCKER for widgets: vds does not yet expose battery / polling-rate / jack / mic-state. Task #12 must land first: parse these in vdsd (battery from DS input report; rate from the hid-in counter vdsd already keeps; jack + mic-mute from state) and expose via a `vdsctl status` JSON command; add setters (mic gain live, mic mute, speaker/haptic volume, player-LED-on-connect, lightbar-off-on-connect). Then the WPF app (#13) polls `vdsctl status` and drives settings via vdsctl.
- Mic gain is currently launch-time only (VDS_MIC_GAIN). For a live slider, #12 needs a runtime setter (e.g. vdsctl command → vdsd → shared atomic, or the UI writes gain and vdsd re-reads). 

## Still-open / deferred
- **Global timer usage in pacer**: iso_pacer_loop still calls timeBeginPeriod(1) (global) for cv.wait_until precision, unlike the rest of the codebase which uses per-object CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (HighResolutionSleeper). Not observed to cause problems after the due-order fix, but it's an architectural outlier; consider switching to a waitable-timer wait to drop the global change. Low priority.
- **DualSense Edge (DSE)**: user does NOT own one — DSE descriptors updated in parallel but will not be tested. Deprioritized permanently.
- **Mic gain**: 50x default is pure linear (hard-clips loud sounds). VDS_MIC_GAIN env overrides. User is building a tray/connection widget to expose mic + other settings, so gain will be driven from there (may later want a runtime/IPC way to change gain without restart — currently read once at launch).

## Operational rules learned 2026-07-15
- **NEVER start the daemon inside an MCP command that could time out** — a timed-out call kills its child process tree, silently taking vdsd with it (looked like a mystery crash; Windows Event Log had no fault = external kill). Start vdsd in its own short command, test in separate short commands.
- Test tone tool now takes args: `wasapi_test_tone.exe <seconds> <amplitude>`. **Use amplitude 0.05 at night** — user request, controller speaker is loud.
- **Launchers:** `run_vdsd.ps1` (normal, mic gain default 50x), `run_vdsd_debug.ps1` (sets VDS_USBIP_DEBUG=1 → per-URB + mic-stats logs in C:\ProgramData\vDS\usbip_*_debug.log), `run_vdsd_gain.ps1 -Gain N` (override mic gain live for tuning). Mic debug log fields: recv/req ratio (~1.0 healthy, <<1 = consumer over-pulling), primed, buffered, underruns.
- **NEVER bundle daemon-start + attach + long sleeps in one MCP PowerShell call** — a timeout kills the spawned process tree incl. vdsd. Start vdsd (own Start-Process call), then attach separately, then Wait. Bit me twice; the attach also sometimes reports "timed out" but actually succeeds — verify with Get-NetTCPConnection 3240.
- Audio-quality ground truth loop: mark `vdsd.log` length → play tone → read new `audio summary` lines. Healthy = usb_delta≈100/s, dropped_delta=0, underflow_delta=0, pending stable ~22-25. The mid-stream cadence check beats guessing.
- CupheadDualSenseMod confirms the 10.667ms packet cadence (10ms × 512/480) is correct ground truth; its Opus params live inside its native DLL (not readable). vds_protocol.cc is shared with the working Linux build — treat encoder/resample as innocent until Windows-specific causes are exhausted (this was right: both crackle causes were Windows-transport-side).

## Session log 2026-07-14 (this session)
Fixed #15 (stop() deadlock + terminate trap) and #16 (CMD_SUBMIT wire order — the audio killer). First-ever stable ISO OUT streaming: 1637 URBs, 0 BREAKs. Direct folder access (vds, usbip-win2, usbip-virtual-device mounted) massively sped up source reading/edits — prefer it over PowerShell for file work; keep PowerShell for build/run/PnP/log checks. USER CONFIRMED both speaker audio (heard the tone) and haptics (felt the buzz) on the physical controller — the priority path works end-to-end for the first time. Next session: (1) interactive button/stick test (joy.cpl or a game) — input data flows but was never interactively verified; (2) real-game audio+haptics test (buzz smoothness — watch for underruns/pacing; windrv's VdsCompleteUrbAfterDelay pacing pattern is the reference if completions-too-fast becomes a real problem); (3) HidHide so games don't see the raw BT pad double; (4) mic re-enable (un-strip descriptor — all plumbing incl. demux/take_mic_bytes still in place); (5) installer.