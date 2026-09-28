# StreamingQATool

## Build in VS Code (Windows)

Install Visual Studio 2022 with **Desktop development with C++** and a Windows 10/11 SDK, plus CMake 3.30.x on your PATH.

1. Open this repository folder in VS Code and install the recommended **CMake Tools** and **C/C++** extensions when prompted.
2. If CMake Tools asks for a configure preset, select **windows-x64**.
3. Press **Ctrl+Shift+B** to configure and build StreamingQATool. The first configure downloads OBS and its dependencies. After a successful build, a **Yes/No** dialog asks whether to deploy. Choose **Yes** to force-stop OBS, copy `StreamingQATool.dll` into OBS's `obs-plugins/64bit` folder, and restart OBS. Accept the Windows administrator prompt for deployment into Program Files. Choose **No** to finish without deploying.
4. Run **Tasks: Run Task → StreamingQATool: Stage release** to build and collect the DLL and resources under `release/StreamingQATool/`.

The staged DLL is `release/StreamingQATool/bin/64bit/StreamingQATool.dll`. Builds use **RelWithDebInfo**. The preset selects an installed Windows SDK automatically.

Equivalent PowerShell commands:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel
cmake --install build_x64 --config RelWithDebInfo --prefix release
```

If an old CMake cache reports a generator/platform mismatch, run `cmake --fresh --preset windows-x64` once, then build again.

## StreamingQATool panel

The StreamingQATool panel starts docked in OBS and supports docking on any side. OBS can restore your previously saved layout. Drag its title bar to move or float it; if dragging is disabled, uncheck **Docks → Lock Docks**. Reopen a closed panel through **Docks → StreamingQATool**.

Click **Settings** to open the non-modal settings window. **Save** persists the Service Manager host and API key, Bitfocus Companion host/port, OpenLP host/port, and **End Streaming Delay (seconds)**. The delay defaults to **30**; use **0** for no countdown (range 0-3600). **Cancel** discards edits. Repeated Settings clicks focus the existing window.

Settings are stored in OBS's plugin configuration directory as `StreamingQATool/settings.json` (normally `%APPDATA%/obs-studio/plugin_config/StreamingQATool/settings.json` on Windows). The API key is stored in this local file and masked in the UI and startup log. Other values are printed in the OBS startup log; blank values are shown as `(not set)`.

## Live-control workflow

Set the Service Manager **domain** (for example `servicemanager.kcac.ca`) and API key. `https://` is added automatically; a trailing slash is optional and removed. Full HTTPS URLs also work. Do not include `/api/...`. The plugin uses the Worker's `/api/admin/v1` endpoints with `X-API-Key` authentication. Local HTTP hosts are supported for development.

On Windows, requests use native WinHTTP/Schannel on background threads, so HTTPS does not depend on OBS shipping Qt TLS plugins. Certificate validation remains enabled and credentials are not forwarded through redirects.

1. **Selected Service** lists linked, unfinished services for the next 30 days, using Service Manager's Toronto timezone. Select one to retrieve its RTMP address and stream key and enable the two YouTube browser buttons.
2. **Prestart** updates the current OBS profile's streaming destination and starts its encoder. It does not start the public YouTube broadcast. YouTube auto-start must be disabled for this two-step workflow. If YouTube has not started within **20 minutes**, Prestart automatically cancels and stops the matching OBS stream. The timeout is disarmed as soon as YouTube reports live or starting live.
3. **Start Streaming** becomes available when OBS is sending to the selected stream and the Worker reports `canStart`. Clicking it starts YouTube and changes the button to **Stop Streaming**.
4. **Stop Streaming** counts down the configured delay, asks the Worker to end YouTube, waits for `complete` or `revoked`, and only then stops the matching OBS stream.
5. **End YouTube Now** skips the wait or retries an end request. **Stop OBS Now** is an emergency override with confirmation; it stops the local encoder even if YouTube cannot be reached. **Cancel Prestart** stops the encoder before going live.

Polling is state-dependent: one service-list fetch at startup, then **5 minutes while idle**, **15 seconds during Prestart**, **30 seconds while live or counting down to end**, and **15 seconds while waiting for YouTube to confirm the end**. Requests never overlap; the interval begins after the previous response. Selection and explicit actions can request an immediate refresh. A local one-second UI timer updates countdowns without contacting Service Manager.

Connection, DNS, TLS, timeout, authentication, HTTP, and malformed-response failures appear in the dock. Failed status checks disable normal start controls, preserve OBS output, and keep overrides available. Failed **start/end commands retry up to three times, one second apart**, including access-denied responses (four attempts maximum). Polling pauses during those retries, and stops retrying commands once they succeed or exhaust the limit. Status polling then reconciles uncertain results. Authentication and configuration failures on ordinary reads require a manual retry or corrected settings. You can correct the API key during an active session, but cannot switch its host or selected service.

### Optional Service Manager

Settings includes **Enable Service Manager controls** (on by default). Turning it off hides the complete service selector, refresh, YouTube links, streaming and override controls. Polling, queued command retries, countdowns, and Prestart timeout automation stop. Existing OBS/YouTube streams are left running; a request already sent may still finish remotely. Stored credentials are preserved for re-enabling. Logging, connection monitors, marker detection/stats, and Camera Assist continue independently.

### Compact dock controls

The service selector and Refresh share a row, followed by Live Control Panel / YouTube links. One primary button changes from **Prestart** to **Start Streaming** to **End Streaming**, with disabled progress labels during waits. The adjacent override split button changes to the relevant action (Cancel Prestart, End YouTube Now, Retry YouTube End, or Stop OBS Now); its arrow exposes the available alternatives. OBS stop still requires confirmation. Small log and settings icons sit beside the title. Connection dots share a row, and detailed encoder, connection, and marker information is available in tooltips.

### Event logging

**Open Log Stream** opens a non-blocking terminal-style viewer with brief local-time lines, such as `15:22:08  OpenLP: Amazing Grace | Slide 3`. New saved JSONL records retain only UTC timestamp, source, event, and relevant details; session IDs, sequence numbers, transport metadata, and intermediate status chatter are omitted. Existing log files are left intact. Logging continues while closed or paused. Per-run and per-stream JSONL files are saved in `%APPDATA%\obs-studio\plugin_config\StreamingQATool\logs`. Settings includes **Open Log Folder** and retention days (default **90**). Only expired StreamingQATool logs are removed; active run and stream files are protected. Each OBS run gets a unique `streamingqa-run-*.jsonl`; each Prestart opens `streamingqa-stream-*.jsonl` alongside it. Stream logs include all events through local OBS and remote YouTube completion. Cancelled Prestarts close after OBS stops. Starting another attempt or closing OBS marks an unfinished stream as interrupted. Existing daily logs remain readable and subject to retention.

Logs cover Prestart, YouTube start/end requests and confirmations, retries, actual local OBS start/end, OBS scene changes, OpenLP slides and Companion key presses. Requests and confirmations are separate events. Timestamps are UTC; stream credentials and API keys are excluded.

OpenLP defaults to **localhost:4316**. Select **OpenLP version** 3.0 (WebSocket discovery) or 2.4 (HTTP polling every 500 ms). Slide events include `serviceItem` (title, matching `$(openlp:service_item)`), `serviceItemId`, and `slide` (one-based number, matching `$(openlp:slide)`). Initial state is logged as a snapshot. Failed title lookups are reported as unavailable.

Companion defaults to **localhost:8000**, subscribing to Companion 5.x `logs.watch` over `ws://localhost:8000/trpc`. Recognized button press entries are logged; releases and initial historical batches are skipped. Companion must emit button debug log entries. Both integrations can be toggled in Settings and retry failed connections every five seconds. Events missed while disconnected cannot be recovered by the live monitor. Local integrations use HTTP/plain WebSocket and send no control commands.

Optional read-only local connection check: `build_tests/Release/event-log-tests.exe --live` with Qt and obs-deps bin directories on PATH.

## Code organization and tests

- `src/plugin-ui.cpp`: OBS dock registration and encoder integration.
- `src/live-control.*`: service selection, button state, readiness polling, and start/stop sequencing.
- `src/live-control-requests.cpp` and `src/request-policy.h`: state-specific polling intervals and bounded command retries.
- `src/service-manager-client.*`: authenticated HTTP requests, timeouts, and error classification.
- `src/windows-http.*`: native Windows HTTPS transport, independent of Qt TLS backends.
- `src/settings.*`: settings dialog, validation, atomic persistence, and redacted startup logging.
- `tests/`: local mock-Worker and encoder tests; these never start OBS or publish to YouTube.

After the first project configure downloads dependencies, run the Windows tests with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/Run-Tests.ps1
```

### Camera Assist

Enable **Camera Assist** in Settings (off by default), select two different OBS scenes (**Slide-only scene** and **Slide + camera scene**), and set the return delay (default **15 seconds**, range 1–3600).

While the current program scene is either assigned scene, a confirmed `sermon-start` marker shows a dismissible overlay asking to switch to the slide + camera scene. This switch requires a click. A confirmed `sermon-end` marker arms the return; only after that marker disappears does the overlay offer **Switch to slides now** with an automatic countdown. **Dismiss** cancels that occurrence's countdown. No prompt appears if already on its destination scene.

Camera Assist works while streaming or idle. Moving to an unassigned scene disables it for that scene and cancels pending actions; manually changing scenes or disabling/reconfiguring the feature also cancels a pending prompt/countdown. The destination is validated when switching, so a removed or renamed scene produces an error instead of an automatic fallback. Rename/reassign scene selections in Settings as needed. Scene changes, assist switches, dismissals, and errors enter the existing logs.

### Timestamp markers on OBS program output

The main panel shows the current confirmed marker, appearance/disappearance counts, valid readings versus sampled frames, and last-seen time. Counts start with the OBS session and update once per second or on a confirmed change.

The marker reader automatically samples the composed **program canvas** at up to 10 Hz, including scene transitions and overlays. It does not inspect the Studio Mode preview or require streaming to start. Decoding runs on a worker thread with at most one frame in flight. No screenshots or per-frame observations are saved; only confirmed changes enter the brief event log.

It follows the sibling `timestamp-marker-reader` protocol: finder/contrast validation, extended Hamming (13,8), CRC-16/CCITT-FALSE, service v1 and hymn v2. Three matching samples confirm an appearance. One second of absent readings confirms disappearance. Replacing one valid marker with another logs the old marker disappearing and the new one appearing. Each event uses the first matching/absent sample's UTC timestamp and includes monotonic elapsed milliseconds. Events may therefore be appended after other events with later timestamps while confirmation is pending.

Example: `15:22:08  Marker appeared: sermon-start | 2026-09-27 chinese`. Disappearance includes the same decoded data. Hymn markers contain only `worship-song` or `response-song`; the reader does not invent dates, service types, song titles, or page numbers. A marker returning after confirmed loss creates another appearance.

Supports full-canvas and smaller repositioned slide sources using visible OBS scene-item bounds (including groups and nested scenes), centered 16:9/4:3 letterboxing, stretched aspect ratios, and small alignment offsets. Reads come from the final program image so covered markers remain absent. The footer must retain at least 112x8 pixels on the OBS base canvas (two pixels per cell); resizing below that loses required information. Rotated, flipped, partially off-canvas, cropped, or covered markers may not decode. The speaker-placeholder slide is intentionally unmarked by the generator. Current capture supports SDR 8-bit program textures; unsupported formats and capture errors appear in the panel. Capture failure is not treated as proof that a marker disappeared. Decoder tests use a checked-in copy of the reference reader's generator-produced fixtures (`tests/marker-fixtures.json`). Actual OBS presentation capture still needs validation after deployment.

## Introduction

The plugin template is meant to be used as a starting point for OBS Studio plugin development. It includes:

* Boilerplate plugin source code
* A CMake project file
* GitHub Actions workflows and repository actions

## Supported Build Environments

| Platform  | Tool   |
|-----------|--------|
| Windows   | Visual Studio 17 2022 |
| macOS     | XCode 16.0 |
| Windows, macOS  | CMake 3.30.5 |
| Ubuntu 24.04 | CMake 3.28.3 |
| Ubuntu 24.04 | `ninja-build` |
| Ubuntu 24.04 | `pkg-config`
| Ubuntu 24.04 | `build-essential` |

## Quick Start

An absolute bare-bones [Quick Start Guide](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide) is available in the wiki.

## Documentation

All documentation can be found in the [Plugin Template Wiki](https://github.com/obsproject/obs-plugintemplate/wiki).

Suggested reading to get up and running:

* [Getting started](https://github.com/obsproject/obs-plugintemplate/wiki/Getting-Started)
* [Build system requirements](https://github.com/obsproject/obs-plugintemplate/wiki/Build-System-Requirements)
* [Build system options](https://github.com/obsproject/obs-plugintemplate/wiki/CMake-Build-System-Options)

## GitHub Actions & CI

Default GitHub Actions workflows are available for the following repository actions:

* `push`: Run for commits or tags pushed to `master` or `main` branches.
* `pr-pull`: Run when a Pull Request has been pushed or synchronized.
* `dispatch`: Run when triggered by the workflow dispatch in GitHub's user interface.
* `build-project`: Builds the actual project and is triggered by other workflows.
* `check-format`: Checks CMake and plugin source code formatting and is triggered by other workflows.

The workflows make use of GitHub repository actions (contained in `.github/actions`) and build scripts (contained in `.github/scripts`) which are not needed for local development, but might need to be adjusted if additional/different steps are required to build the plugin.

### Retrieving build artifacts

Successful builds on GitHub Actions will produce build artifacts that can be downloaded for testing. These artifacts are commonly simple archives and will not contain package installers or installation programs.

### Building a Release

To create a release, an appropriately named tag needs to be pushed to the `main`/`master` branch using semantic versioning (e.g., `12.3.4`, `23.4.5-beta2`). A draft release will be created on the associated repository with generated installer packages or installation programs attached as release artifacts.

## Signing and Notarizing on macOS

Basic concepts of codesigning and notarization on macOS are explained in the correspodning [Wiki article](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS) which has a specific section for the [GitHub Actions setup](https://github.com/obsproject/obs-plugintemplate/wiki/Codesigning-On-macOS#setting-up-code-signing-for-github-actions).
