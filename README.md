# Perch for Windows

Perch for Windows uses a native Indirect Display Driver (IddCx), not desktop screenshot capture. Windows composes the virtual display into the driver's swap chain; the driver publishes changed frames to a local shared-memory feed. The companion eye executable turns those changes into Perch vectors and tape.

## Build and tests

See [`windows/README.md`](windows/README.md) for Visual Studio and WDK prerequisites, driver/app build commands, signing and installation cautions, and the local benchmark details.

Run the watchdog policy and HTTP-handler tests from the repository root:

```powershell
python -m unittest discover -s windows\testapp -p "test_*.py" -v
```

The demo server is intended for local/private testing. Its listener and firewall/NAT constraints are described in the Windows README. It does not use a cloud model or transmit request contents to an external service.

## Contact and sponsorship

- Website: [ethernest.co.uk](https://ethernest.co.uk)
- Ko-fi: [ko-fi.com/liveuk](https://ko-fi.com/liveuk)
- Email: [mik.hebblethwaite@gmail.com](mailto:mik.hebblethwaite@gmail.com)

## Copyright

Copyright (c) 2026 Mike J Hebblethwaite. All rights reserved. See [`COPYRIGHT`](COPYRIGHT).
