# Wi-Fi Provisioning Plan

> **Implementation status:** Implemented on 2026-07-18. The firmware builds
> successfully with ESP-IDF 5.5.1 without including `wifi_secrets.h`.
> Compilation and static credential-log checks pass; the hardware scenarios in
> Section 12 still need to be exercised on a physical board and representative
> Android, iOS/macOS, and Windows clients.

## 1. Goal

Replace compile-time Wi-Fi credentials with an on-device provisioning flow. A newly flashed board, or a board that cannot reach its saved network, creates a protected local access point. The user joins that network, opens a captive portal, selects a 2.4 GHz Wi-Fi network, and submits its password.

The firmware validates candidate credentials before saving them. Provisioning and recovery happen without reflashing and, after successful setup, without rebooting.

## 2. Agreed product behavior

### First boot and boot-time recovery

1. Initialize NVS, the display, buttons, peripherals, and normal local application state.
2. Load the single saved Wi-Fi record from the provisioning NVS namespace.
3. If no valid record exists, start provisioning immediately and leave it active indefinitely.
4. If a record exists, try it in station mode for up to 30 seconds.
5. If the board obtains a DHCP address, continue normal startup.
6. If the attempt times out, start the protected portal in AP+STA mode and continue attempting the saved network in the background.
7. If the saved network becomes available again, close the automatically opened portal and resume normal operation.

### Disconnect recovery after a successful connection

1. Retry the saved network silently in station mode for five minutes.
2. Do not expose the setup access point during that recovery window.
3. After five minutes offline, start the protected portal in AP+STA mode while continuing background reconnect attempts.
4. If the saved network recovers, stop the portal and restore normal services.

### Manual reconfiguration

1. Holding both board buttons for five seconds opens setup mode.
2. The known-good saved credentials are not erased.
3. Normal display interactions and services are paused while setup owns the board.
4. A manually opened portal closes after 10 minutes without meaningful user activity.
5. Status polling must not extend this timeout indefinitely. Page loads, scans, submissions, and other explicit user actions may refresh it.
6. A `Cancel setup` action is shown only when a known-good record exists. Canceling changes no stored data, restores the saved station configuration, closes provisioning, and resumes normal services.

### Network reset

1. Continue holding both buttons after the five-second setup threshold.
2. Show a visible reset countdown between five and 15 seconds.
3. Releasing before 15 seconds cancels the reset but leaves setup mode active.
4. At 15 seconds, erase only the provisioning namespace: the active Wi-Fi record and generated setup password.
5. Preserve hydration settings, pet progress, and all other application NVS namespaces.
6. Generate a new setup password and start a fresh provisioning access point.

## 3. Setup network identity

- SSID: `G4PYS-Setup-XXXX`
- `XXXX`: final four uppercase hexadecimal digits of the ESP32 factory MAC address
- Security: WPA2-Personal
- Address: `192.168.4.1`
- Password: random 12-character value generated from the hardware RNG
- Password alphabet: uppercase letters and digits excluding ambiguous characters such as `0/O`, `1/I`, and `5/S`
- Password lifetime: generate on first use, persist in NVS, and regenerate only after network reset
- Maximum connected portal clients: use a small fixed limit appropriate for setup; several clients may view the page, but validation remains single-flight

The board setup screen displays:

- setup SSID;
- setup password;
- `http://192.168.4.1`;
- a standard Wi-Fi join QR code;
- the current provisioning or validation state; and
- the network-reset countdown when applicable.

Use Espressif's managed [`espressif/qrcode`](https://components.espressif.com/components/espressif/qrcode/versions/0.2.0/readme) component to generate the QR code at runtime.

## 4. Supported target networks

Provisioning v1 supports:

- 2.4 GHz open networks;
- WPA2-Personal networks; and
- WPA2/WPA3-Personal transition-mode networks.

Provisioning v1 does not support:

- WPA3-only networks;
- enterprise/802.1X authentication;
- 5 GHz-only networks;
- static IP configuration; or
- storing or prioritizing multiple networks.

The portal must visibly mark unsupported scan results and prevent their selection. For hidden networks, allow manual SSID entry and apply the same authentication constraints. DHCP address acquisition is the only success criterion; internet access is not required.

Special grouping for duplicate SSIDs/BSSIDs is out of scope. Scan results may be presented directly.

## 5. Captive portal experience

### Discovery

- Run a small DNS responder on the setup interface that resolves captive-portal hostnames to `192.168.4.1`.
- Serve common Android, iOS/macOS, and Windows connectivity-check paths so those systems detect the captive portal.
- Redirect unknown browser paths to the setup page.
- Keep direct access at `http://192.168.4.1` available when automatic captive-portal launch fails.

### Network selection

- Perform one scan when the portal first opens.
- Provide an explicit `Refresh` action.
- Do not scan continuously.
- Disable refresh while candidate credentials are being validated.
- Display SSID, signal strength, and security status.
- Include a `Hidden network` option for manual SSID entry.
- Ask for a password only when the selected security mode requires one.

### Candidate validation

1. Accept only one credential submission at a time.
2. Additional submissions receive a clear `setup already in progress` response.
3. Keep the setup AP and captive portal alive.
4. Configure the candidate in RAM and test it in AP+STA mode.
5. Allow up to 30 seconds to obtain a station IP address.
6. Report progress such as `connecting` and `waiting for IP`.
7. Map Wi-Fi disconnect reasons into safe user messages where possible:
   - wrong password/authentication failed;
   - network not found;
   - unsupported network; or
   - connection timed out.
8. On failure, do not change the active NVS record. Restore the known-good station configuration in RAM and reconnect it in the background while keeping a manually opened portal available for another attempt.
9. On success, atomically replace the active NVS record.

An automatically opened outage portal closes if the saved network recovers. A manually opened portal remains available after restoring the old connection so the user can retry, cancel, or reach its inactivity timeout.

### Successful handoff

1. Commit the validated network record.
2. Derive the normal hostname as `g4pys-company-XXXX.local`, using the same MAC suffix as the setup SSID.
3. Show the assigned station IP and unique hostname in both the portal and board UI for about 10 seconds.
4. Stop captive DNS and the provisioning HTTP server.
5. Stop the setup AP and switch to normal station operation.
6. Restore the normal HTTP API and network services.
7. Continue without rebooting.

## 6. HTTP contract

The exact payload representation may be form-encoded or JSON, but the provisioning server should expose these logical operations:

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Mobile-friendly setup page |
| `GET` | `/api/networks` | Current scan results and scan state |
| `POST` | `/api/scan` | Start an explicit scan |
| `POST` | `/api/configure` | Begin single-flight candidate validation |
| `GET` | `/api/status` | Validation, connection, and handoff status |
| `POST` | `/api/cancel` | Cancel manual setup when a saved network exists |

All provisioning responses must include `Cache-Control: no-store`. The server must never:

- return the submitted or stored target-network password;
- include either Wi-Fi password in logs;
- interpolate an SSID into HTML without escaping it; or
- accept an SSID or password outside ESP-IDF's supported length constraints.

Use plain HTTP only. The WPA2 setup network is the transport boundary; local HTTPS and its certificate-warning flow are out of scope.

## 7. State machine

The network manager should own all Wi-Fi mode changes and expose state to the display and service coordinator.

| State | Entry condition | Main behavior | Exit condition |
| --- | --- | --- | --- |
| `UNPROVISIONED` | No valid active record | Start protected AP and portal indefinitely | Candidate accepted |
| `BOOT_CONNECTING` | Saved record loaded | STA retry for 30 seconds | IP acquired or timeout |
| `ONLINE` | Station has an IP | Run normal application network services | Disconnect or manual setup |
| `RECONNECTING` | Previously online station disconnects | Retry silently for five minutes | IP acquired or timeout |
| `AUTO_PROVISIONING` | Boot attempt or five-minute retry expires | AP+STA portal; retry saved record | Saved network recovers or candidate succeeds |
| `MANUAL_PROVISIONING` | Both buttons held for five seconds | AP+STA portal; retain active record | Candidate succeeds, cancel, or inactivity timeout |
| `VALIDATING` | Portal accepts a candidate | Single-flight AP+STA connection test for 30 seconds | Success or failure |
| `HANDOFF` | Candidate receives an IP | Commit once, show result for 10 seconds | Resume `ONLINE` |
| `NETWORK_RESET` | Both buttons held for 15 seconds | Erase provisioning data and regenerate AP password | Enter `UNPROVISIONED` |

Wi-Fi and IP event handlers must enqueue state-machine events rather than performing blocking work. Timers should use FreeRTOS/ESP timer deadlines; no code path should wait forever for `WIFI_CONNECTED_BIT`.

## 8. Persistent data

Use a dedicated namespace such as `wifi_prov`. Keep it separate from ESP-IDF driver persistence and existing application namespaces.

Recommended records:

- `active`: one versioned blob containing SSID length/data, password length/data, and the necessary authentication metadata;
- `ap_pass`: generated 12-character setup password; and
- optional schema version metadata if it is not included in the blob.

Configure the ESP-IDF Wi-Fi driver to use RAM storage for candidate configuration so `esp_wifi_set_config()` cannot accidentally persist an unvalidated candidate. Commit the custom `active` blob only after DHCP succeeds. A failed or interrupted test therefore leaves the previous record intact.

Treat a missing, malformed, or unsupported-version active record as unprovisioned. Never log the blob or password fields.

NVS encryption, flash encryption, secure boot, and resistance to physical flash extraction are separate security work and are not part of this change.

## 9. Firmware architecture

### New network manager

Add a module such as `network_manager.h/.cpp` responsible for:

- ESP-NETIF and Wi-Fi initialization;
- loading and validating provisioning NVS records;
- the network state machine and deadlines;
- station reconnect policy;
- AP+STA transitions;
- candidate validation and reason mapping;
- stable MAC-derived identity;
- setup-password generation; and
- notifications to the UI and normal-service coordinator.

### New provisioning portal

Add a module such as `provisioning_portal.h/.cpp` responsible for:

- AP DHCP configuration at `192.168.4.1`;
- captive DNS;
- provisioning HTTP routes;
- embedded HTML/CSS/JavaScript assets;
- scan-result serialization and HTML escaping;
- single-flight request coordination; and
- the manual inactivity deadline.

The network manager, not HTTP request handlers, performs connection tests. Request handlers submit commands and read snapshots so they never block for 30 seconds.

### Setup display

Add a provisioning renderer that receives immutable state snapshots and renders:

- join QR code and fallback text;
- scanning/connecting/waiting/error states;
- successful IP/hostname handoff; and
- reset progress.

When provisioning has display priority, the render task must skip normal mode rendering and normal button actions.

### Button handling

Refactor the existing independent button polling into a chord-aware input path:

- suppress single-button actions when both buttons form a chord;
- enter manual setup at five seconds;
- show reset progress from five to 15 seconds;
- reset network data at 15 seconds; and
- prevent the current BOOT-button mode toggle from firing before the chord is recognized.

Individual button behavior outside provisioning must remain unchanged.

### Normal service lifecycle

Add `http_api_stop()` and make normal HTTP startup/stopping idempotent. Port 80 has one owner at a time:

- the normal board API while online; or
- the dedicated provisioning server while setup is active.

Coordinate mDNS advertisement with the active service and advertise the unique hostname. Long-lived tasks such as the board client or comic fetcher should start at most once and tolerate network loss. SNTP should run asynchronously after network availability rather than blocking provisioning or display startup.

## 10. Existing code changes

### `main.cpp`

- Remove `wifi_secrets.h`.
- Replace blocking `wifi_start()` and its infinite event-group wait with the network manager.
- Delegate Wi-Fi/IP events and service lifecycle transitions.
- Give provisioning display priority.
- Integrate chord-aware button handling.
- Keep display and local features usable before Wi-Fi initialization finishes.

### `board_client.cpp`

- Remove `wifi_secrets.h`.
- Use mDNS discovery by default.
- Keep the fallback host empty, port `8766`, and optional token empty in normal source/config defaults.
- Leave daemon configuration outside the Wi-Fi portal.

### `http_api.h/.cpp`

- Add safe, idempotent stop/restart support.
- Do not expose normal API routes through the provisioning server.

### `idf_component.yml` and `CMakeLists.txt`

- Add the Espressif QR generator dependency.
- Register the new network, portal, and provisioning display sources.
- Add any required lwIP/socket dependencies for the captive DNS responder.

### Legacy secrets files

- The firmware build must not require `wifi_secrets.h` or `wifi_secrets.example.h`.
- Do not add migration logic for credentials compiled into older firmware.
- After upgrade, a device without an active provisioning record enters first-time setup once.

## 11. Implementation phases

### Phase 1: Remove compile-time coupling

- Remove secrets-header includes and replace daemon settings with non-secret defaults.
- Add the provisioning NVS record format, identity helpers, and setup-password generator.
- Confirm a clean build no longer needs a local header.

### Phase 2: Introduce the non-blocking network manager

- Move Wi-Fi ownership out of `main.cpp`.
- Implement boot connection, online, reconnect, and timeout transitions.
- Keep normal behavior working with an already populated test NVS record.

### Phase 3: Add the captive portal

- Add SoftAP configuration, DHCP, captive DNS, probe handling, and direct-IP access.
- Add initial/manual scanning and the mobile setup form.
- Add asynchronous single-flight validation with safe error mapping.

### Phase 4: Add display and controls

- Integrate runtime Wi-Fi QR generation.
- Add the setup-status and handoff screens.
- Refactor button handling for the five/15-second chord and reset countdown.

### Phase 5: Complete lifecycle integration

- Add normal HTTP server stop/restart.
- Add unique mDNS hostname handling.
- Restore normal services after cancel, recovery, timeout, and successful handoff.
- Verify no task is started twice across repeated provisioning cycles.

### Phase 6: Harden and document

- Validate all input lengths and escape hostile SSIDs.
- Add no-store headers and audit logs for credential leakage.
- Exercise power interruption, reconnect, cancellation, and reset paths.
- Update build/flashing documentation with the new first-boot flow.

## 12. Verification plan

### Build and static checks

- Build from a clean checkout without any `wifi_secrets.h`.
- Confirm no source or generated build artifact contains a test SSID or password.
- Confirm provisioning and normal servers cannot own port 80 simultaneously.
- Confirm all credentials are redacted from normal and error logs.

### First-time setup

- Empty provisioning namespace starts `G4PYS-Setup-XXXX` immediately.
- AP password persists across reboot and matches the displayed text and QR code.
- Captive portal opens on representative Android and iOS devices.
- Direct navigation to `192.168.4.1` works.
- Initial scan, manual refresh, hidden SSID, open Wi-Fi, and supported secured Wi-Fi work.
- Unsupported networks are visible but cannot be submitted.

### Validation and atomicity

- Correct credentials obtain an IP, commit once, show the 10-second handoff, and continue without reboot.
- Wrong password, missing AP, and timeout keep the portal available and do not alter NVS.
- A second simultaneous submission receives `setup already in progress`.
- Power loss before successful commit leaves the old record valid.
- Power loss after commit boots using the new record.

### Recovery

- Saved credentials get a 30-second boot attempt before provisioning opens.
- A runtime disconnect does not expose the AP for the first five minutes.
- The AP opens after five minutes and closes if the saved network recovers.
- Manual setup reconnects old credentials after a failed candidate but leaves the portal available.
- Manual cancel restores normal operation without changing NVS.
- Manual inactivity closes setup after 10 minutes; status polling alone does not prevent expiry.

### Buttons and reset

- Normal single-button actions remain unchanged.
- Holding both buttons does not trigger a mode or screen action.
- Five seconds enters manual setup without erasing credentials.
- Reset countdown is visible between five and 15 seconds.
- Early release cancels reset.
- Fifteen seconds erases only provisioning data, regenerates the setup password, and preserves all application data.

### Service lifecycle

- Normal HTTP API is unavailable only while provisioning owns port 80.
- Normal API and `_http._tcp` mDNS advertisement return after provisioning closes.
- Multiple setup/cancel/success cycles do not leak tasks, sockets, event handlers, or heap.
- `g4pys-company-XXXX.local` resolves and does not collide with a second board using a different suffix.

## 13. Acceptance criteria

The feature is complete when:

1. Firmware builds and flashes without `wifi_secrets.h`.
2. A user can provision a blank board using only the board display and a phone.
3. Invalid candidate credentials never replace the known-good record.
4. Successful provisioning reaches normal operation without rebooting.
5. Temporary outages follow the 30-second boot and five-minute runtime fallback policies.
6. Manual setup, cancel, inactivity timeout, and network reset behave as specified.
7. Normal device APIs and mDNS recover after every exit from provisioning.
8. Wi-Fi passwords never appear in responses or logs.
9. Existing non-network NVS data survives network reset.

## 14. Explicitly out of scope

- Configuring lyrics-daemon host, port, or token in the Wi-Fi portal
- Migrating compile-time credentials
- Multiple saved networks or priority/failover lists
- Static IP configuration
- WPA3-only and enterprise Wi-Fi
- Internet-connectivity validation
- Encrypted NVS, flash encryption, and secure boot
- OTA updates or unrelated device settings in the portal
- Special mesh/BSSID grouping

## 15. Implementation record

### Module layout

- `main/network_manager.h/.cpp` owns Wi-Fi initialization, RAM-only driver
  configuration, provisioning NVS records, deadlines, recovery, candidate
  validation, scanning, MAC-derived identity, and service notifications.
- `main/provisioning_portal.h/.cpp` owns the setup HTTP server, captive DNS,
  connectivity-check redirects, embedded mobile UI, request validation,
  no-store responses, and meaningful-activity tracking.
- `main/provisioning_display.h/.cpp` renders the setup credentials, runtime
  Wi-Fi join QR code, connection state, handoff details, and reset countdown.
- `main/main.cpp` coordinates normal HTTP/mDNS services, asynchronous SNTP,
  provisioning display priority, and chord-aware button input.
- `main/http_api.cpp` now provides an idempotent `http_api_stop()` so port 80
  can transfer safely between the normal API and provisioning portal.
- `main/board_client.cpp` uses mDNS-first discovery with empty non-secret
  fallback defaults and starts its task at most once.

### Persistence and security behavior

- Namespace: `wifi_prov`.
- Keys: versioned `active` credential blob and persisted `ap_pass` string.
- The active record includes a checksum and is rejected when malformed or from
  an unsupported schema version.
- `esp_wifi_set_storage(WIFI_STORAGE_RAM)` prevents an unvalidated candidate
  from being persisted by the Wi-Fi driver.
- The candidate replaces `active` only after `IP_EVENT_STA_GOT_IP` and a
  successful NVS commit.
- Submitted and stored target-network passwords are never returned by the
  portal or written to firmware logs.
- Network reset calls `nvs_erase_all()` only on the `wifi_prov` namespace.

### Timing and limits implemented

| Behavior | Value |
| --- | --- |
| Saved-network boot attempt | 30 seconds |
| Candidate validation | 30 seconds |
| Silent runtime reconnect | 5 minutes |
| Manual setup inactivity | 10 minutes |
| Successful handoff screen | 10 seconds |
| Background retry while automatic portal is open | 10 seconds |
| Setup AP client limit | 4 |
| Scan results retained | 24 |

### Build verification

Verified with:

```sh
idf.py build
```

Result on ESP-IDF 5.5.1:

- build completed successfully for `esp32s3`;
- generated image: `build/g4pys_company_rlcd.bin`;
- image size: approximately 1.24 MiB;
- smallest application partition: 8 MiB, approximately 85% free;
- build dependency graph contains no reference to `wifi_secrets.h`; and
- the new provisioning modules contain no credential-bearing log statements.

This verification does not replace the physical-device test matrix in Section
12. In particular, captive-portal discovery, display QR readability, button
timing, DHCP validation, outage recovery, and repeated lifecycle tests require
hardware.
