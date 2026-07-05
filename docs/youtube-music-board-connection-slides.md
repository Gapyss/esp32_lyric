# YouTube Music Lyric Display: Board Connection Slides

## Slide 1: Big Idea

```text
YouTube Music in Chrome
    -> Chrome extension
    -> Mac lyrics daemon
    -> ESP32 board over Wi-Fi
    -> 400x300 lyric display
```

The Mac plays the music. The ESP32 board is only a display.

Animation idea:

1. Show YouTube Music playing on the Mac.
2. Show the ESP32 display beside it.
3. Draw an arrow from Mac to board.
4. Show lyric text appearing on the board.

## Slide 2: Chrome Extension Reads YouTube Music

The Chrome extension runs inside `music.youtube.com`.

It reads:

- song title
- artist
- album
- video ID
- current playback position
- play / pause / seek events

In this project, the code is:

```text
browser_extension/content_script.js
```

Animation idea:

1. Highlight the YouTube Music page.
2. Pop up labels: `title`, `artist`, `currentTime`.
3. Move those labels into a box called `Chrome Extension`.

## Slide 3: Extension Sends Data to the Mac Daemon

The extension opens a WebSocket to the local Mac:

```text
ws://127.0.0.1:8765/extension
```

Example song message:

```json
{
  "type": "now-playing",
  "payload": {
    "title": "Song Name",
    "artist": "Artist",
    "videoId": "abc123"
  }
}
```

Example timing message:

```json
{
  "type": "tick",
  "payload": {
    "positionSec": 42.5,
    "paused": false,
    "playbackRate": 1
  }
}
```

Animation idea:

1. Send one `now-playing` packet first.
2. Send repeated `tick` packets every short interval.
3. Flash `play`, `pause`, and `seek` events when the user controls the song.

## Slide 4: Mac Daemon Does the Heavy Work

The Mac daemon receives the extension messages.

It then:

- resolves lyrics
- checks cached lyrics
- fetches lyrics if needed
- tracks playback time
- renders a 400x300 black-and-white bitmap frame
- sends the frame to the ESP32 board

In this project, the code is:

```text
daemon/lyrics_display_daemon.py
```

The daemon listens on:

```text
Extension socket: ws://127.0.0.1:8765/extension
Board socket:     ws://0.0.0.0:8766/board
```

Animation idea:

1. Song data enters the daemon.
2. Lyrics database appears.
3. A rendered lyric frame appears.
4. The frame becomes a binary socket packet.

## Slide 5: ESP32 Finds the Mac

The ESP32 board connects to Wi-Fi first.

Then it searches the network for the Mac daemon using mDNS:

```text
_lyrics._tcp
```

After discovery, it connects to the daemon:

```text
ws://<mac-ip>:8766/board
```

In this project, the code is:

```text
firmware/main/board_client.cpp
```

Animation idea:

1. Board powers on.
2. Wi-Fi icon appears.
3. Board searches for `_lyrics._tcp`.
4. Board connects to the Mac daemon.

## Slide 6: Mac Sends Display Frames to the Board

The daemon sends binary WebSocket messages to the ESP32.

Each message contains a `LYR1` frame envelope:

```text
magic:      LYR1
width:      400
height:     300
format:     1-bit bitmap
swapInMs:   when to show the frame
payload:    display pixels
```

Full frame size:

```text
400 * 300 / 8 = 15000 bytes
```

The board receives the frame and blits it to the ST7305 LCD.

Animation idea:

1. A `LYR1` packet moves from Mac to board.
2. ESP32 decodes the packet.
3. The lyric line appears on the display.
4. A scheduled next frame waits, then flips at the right time.

## Slide 7: Why the Browser Does Not Talk Directly to the Board

The browser extension only talks to the local Mac daemon because the daemon can do things the browser should not do:

- run a stable WebSocket server
- use SQLite/cache files
- fetch and store lyrics
- render fonts correctly
- keep accurate timing even when Chrome throttles background tabs

The ESP32 also stays simple. It does not need lyric parsing or font rendering.

Animation idea:

1. Show browser as a small reader.
2. Show daemon as the main worker.
3. Show ESP32 as a display-only client.

## Final Architecture

```text
Chrome / YouTube Music
  reads song + position
        |
        v
Chrome Extension
  WebSocket to localhost
  ws://127.0.0.1:8765/extension
        |
        v
Mac Lyrics Daemon
  lyrics + timing + rendering
  WebSocket server on port 8766
        |
        v
ESP32-S3 Board
  receives LYR1 bitmap frames
        |
        v
ST7305 400x300 LCD
  shows synced lyrics
```

## Short Explanation

The Chrome extension watches YouTube Music and sends song information plus playback time to a local Mac daemon. The daemon finds the lyrics, renders the correct lyric screen as a 400x300 bitmap, and sends that bitmap to the ESP32 over a WebSocket. The ESP32 only receives display frames and shows them on the LCD.
