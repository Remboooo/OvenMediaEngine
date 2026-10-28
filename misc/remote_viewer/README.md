# Remote viewer desktop

`limits.py --remote-viewer` / `--remote-firefox` and `smoke_test.sh` (`SMOKE_VIEWER_CDP` / `SMOKE_VIEWER_BIDI`) judge a real Chrome or Firefox on another machine: a desktop with a display and a hardware decoder, like a real viewer. `Setup-RemoteViewer.ps1` prepares a Windows desktop for that.

On the desktop, with the IP of the machine that runs the harness:

```powershell
powershell -ExecutionPolicy Bypass -File .\Setup-RemoteViewer.ps1 -ControllerIP <lan-ip>
```

It:
- starts Chrome (debug port 9222) and Firefox (WebDriver BiDi, 9224) on test profiles under `C:\ometest`, with background throttling off. Your own profiles are not touched.
- exposes them as 19222 / 19224 on the LAN (netsh portproxy), with a firewall rule that lets in only `-ControllerIP`. It asks for admin rights for this step only.
- prints the GPU, display mode and refresh rate, and the harness arguments to use.
- keeps the machine and display awake until Ctrl+C, and reports when a debug port stops answering.
- while it runs, serves a small control service on LAN port 19220 (controller only), so the harness can restart a stuck test browser without anyone at the desktop:
  ```bash
  curl http://<desktop-a-ip>:19220/status                 # which test browsers answer, their pids
  curl -X POST http://<desktop-a-ip>:19220/restart/firefox # or /restart/chrome
  ```
  A restart stops only processes on the test profile and starts the browser again with the same flags. The usual reason is Firefox: it allows one remote session, and a client that was killed instead of disconnecting leaves it taken until Firefox restarts. `traffic_sim.py --desktop` calls it by itself after three quick failures in a row. It can do nothing the debug ports can't already. `-NoControl` leaves it out.

`-Browser Chrome` or `-Browser Firefox` sets up one of the two; `-NoKeepAwake` exits after setup. `-Remove` deletes the port proxies and the firewall rule (it leaves the browsers and profiles). Rerunning is safe: a browser that already answers is left alone.

Things to know:
- **Those ports give full control of the test browsers.** Keep the firewall rule to the controller, and run `-Remove` when you're done testing.
- **Never close a test browser's last tab while a harness is connected:** that ends the session. The harness only opens and closes its own windows.
- **Connect by IP address.** Chrome's debug port refuses a hostname in the Host header.
- Turn off Energy saver and Memory saver in the test Chrome (`chrome://settings/performance`), and keep the windows on a monitor that stays on.
- Results depend on the desktop (GPU, refresh rate, hardware decoder), so take a clean baseline on each new machine before comparing builds. Some hardware decoders hold only 8 frames, so a burst of frames can freeze there when it doesn't in headless Chromium (see `misc/traffic_sim/FINDINGS-2026-10.md`).
