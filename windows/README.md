# Perch for Windows

The Windows port uses a native Indirect Display Driver (IddCx), not desktop screenshot capture. Windows composes the virtual display
into the driver's swap chain; the driver samples a changed surface at most 10 times a second and signals a local shared-memory frame
feed. `coop-perch-win.exe` waits on that event, compares changed pixels, and writes the Perch `eye` plane and tape files. It does not
save full-screen images.

## Build

Install Visual Studio 2022 Build Tools with the C++ desktop workload and Windows Driver Kit 10.0.26100.0. From a Visual Studio
Developer PowerShell, use the 64-bit MSBuild executable for the driver build so the WDK INF verifier loads its x64 library:

```powershell
$msbuild = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"
& $msbuild windows\PerchWin.vcxproj /p:Configuration=Release /p:Platform=x64
& $msbuild windows\idd\PerchIddDriver.vcxproj /p:Configuration=Release /p:Platform=x64
```

The application-only test exercises the vector, tape, and box writer without a virtual display:

```powershell
windows\bin\x64\Release\coop-perch-win.exe --self-test
```

Run the local moving-parcel, screenshot-versus-Perch benchmark page:

```powershell
python windows\testapp\server.py
```

Open `http://127.0.0.1:8765/` and start the 20-minute test. The server binds only to loopback (`127.0.0.1`) and the Windows
VM's private/NAT interface (`10.0.2.15`), both on TCP 8765. The private endpoint requires a Windows Firewall allow rule scoped
to that local address, TCP 8765, the Private profile, Ethernet, and remote `10.0.2.0/24`; it must not be broadened to other
ports or profiles. The VM's QEMU user-mode NAT currently forwards only SSH on 2293, not 8765, so a Windows Firewall rule alone
does not make the demo reachable from the host or other LAN devices.

Autonomous screenshot and Perch-event bot paths sort the same generated moving parcels without manually entered detections. The
page measures full screenshot PNG bytes, Perch-style change-event bytes, and vision-path wall time. This browser benchmark does
not call an AI model, so it cannot report model token usage; it also does not measure Windows process CPU time, hardware watts,
or the installed driver's live physical-monitor feed. Its Perch path exercises the 16x10 changed-cell format, but is a local
benchmark implementation rather than a live driver measurement. Stop the local server with Ctrl+C. The completed 2026-11-03 run
report is available at `http://127.0.0.1:8765/results.html`.
The local page `http://127.0.0.1:8765/email-results.html` separates the saved Ubuntu email-demo measurements from the Windows
port's validation status and an independent screenshot review. It does not claim that the email task has been run through the
Windows eye.

The demo server rejects coop-style probe paths (including environment files, WordPress, traversal, injection, and backup paths).
Each probe is rejected immediately. Five probes from one non-exempt source within 60 seconds trigger a 10-minute in-process
application ban; loopback and all of `10.0.2.0/24` are exempt from bans. No operating-system or network bans are used. Redacted
watch events are rate-limited and stored at `%LOCALAPPDATA%\Perch\testapp\watchdog.jsonl`; request targets, query strings, and
bodies are not logged. Run the deterministic policy tests with:

```powershell
python -m unittest discover -s windows\testapp -p "test_*.py" -v
```

The self-test writes under `%LOCALAPPDATA%\Perch\tape\display` by default and checks a synthetic changed region through the same eye
and output path used by the frame feed. Use `--output DIRECTORY` to choose another local path.
The normal executable waits for the IddCx driver's named frame feed and emits `eye.box`, `tape.txt`, and `vectors.bin` in that
directory. `eye.box` keeps the existing 4096-byte Perch header and `u16` 16x10x9 frame layout; Windows readers can wait on the
`Local\CoopPerch.eye` event.

After a signed driver package is installed, enable the Perch display in Windows Display Settings; the executable then waits for
frames from that virtual display. This port currently covers the display eye and its vector/tape output, not the Windows text or
input-action layers.

The driver project uses `SignMode=Off`; the generated package is unsigned and build output only. Installation and enabling a virtual
monitor are intentionally not performed by the build. Installing a display driver requires administrative approval and an
appropriately signed package. The build does not enable test-signing, change boot settings, or reboot.

For local development, sign the generated catalog with an approved test certificate from the current user's certificate store, then
verify both the catalog and the packaged DLL with SignTool before installation. Trusting a self-signed development certificate in
Local Machine `Root` and `TrustedPublisher` is a machine-wide change and requires administrator approval; it is not a production
signature. Remove those trust entries by the certificate thumbprint when testing is complete. Do not enable test-signing or alter
boot configuration for this UMDF driver. If Windows reports that an operation requires a reboot, stop and obtain separate approval.
