# g4pys Lyrics Display Bridge

Build a store/upload zip:

```sh
python3 tools/package_browser_extension.py
```

Optional Firefox unlisted signing, after installing `web-ext` and exporting
`WEB_EXT_API_KEY` / `WEB_EXT_API_SECRET`:

```sh
python3 tools/package_browser_extension.py --firefox-sign
```

Chrome Web Store signing is performed by the store during upload. For local
Chrome development, load this directory unpacked.

## Firmware target

The popup has a Firmware section for the ESP32 board IP/host and optional MAC
address. The Connect button requests permission for that board origin and checks
`http://<ip-or-host>/usage.json`.

Browsers connect to the firmware by IP/host. The MAC value is stored with the
extension settings for identification; it is not used as a network transport
address.
