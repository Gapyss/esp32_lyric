# Lyrics search: เลือกเนื้อเพลงจาก LRCLIB ให้ตรงเพลงที่เล่นจริง

โค้ดอยู่ที่ `esp8266/daemon/lyrics_search.py` และ daemon เรียกใช้ผ่าน `fetch_lrclib()` ใน `lyrics_display_daemon.py`
หลักที่ยึดไว้คือ **ไม่มีเนื้อเพลงดีกว่าได้เนื้อผิดเพลง**

## ที่เปลี่ยนไปจากเดิม

| เรื่อง | ก่อน | หลัง |
|---|---|---|
| เกณฑ์รับ record | ขอแค่เป็น synced และความยาวต่างกันไม่เกิน ±3 วินาที | ชื่อเพลง, version, ศิลปิน และความยาว ต้องผ่านครบทั้ง 4 ข้อ |
| ชื่อเพลงที่มีของแถม | `(Official MV)`, `[Lyrics]`, `- 2011 Remaster` ทำให้หาไม่เจอ | ตัดของแถมออกก่อนค้นหาและก่อนเทียบ |
| Version | live หรือ remix ที่ยาวพอดีถูกรับไปด้วย | live, remix, acoustic, sped up, Thai Ver. ฯลฯ ต้องตรงกันทั้งสองฝั่ง |
| ศิลปิน | ไม่ได้เช็คเลย | ต้องมีชื่อซ้ำกันอย่างน้อยหนึ่งชื่อ ตัด `VEVO`, `- Topic`, `Official` ออกก่อน และแยกชื่อด้วย `, & / x feat. และ` |
| MV หรือคลิปจากช่องค่ายเพลง | ใช้ชื่อช่องเป็นศิลปิน เลยหาไม่เจอ | อ่านชื่อแบบ `Artist - Song` และ `Song - Artist` ได้ ถ้าฝั่งไหนตรงกับชื่อช่อง จะลองฝั่งนั้นก่อน |
| ความยาวของ MV | ±3 วินาที ทำให้ MV ที่มี intro ตกทั้งหมด | เพลงสั้นกว่าวิดีโอได้ไม่เกิน 90 วินาที และยาวกว่าได้ไม่เกิน 3 วินาที |
| ภาษาไทย | – | เก็บสระและวรรณยุกต์ไว้ตอนเทียบ ส่วนภาษาละตินตัด accent ออก (Beyoncé = Beyonce) |
| จำนวน request ต่อเพลง | 2 | ไม่เกิน 4 และหยุดทันทีเมื่อเจอ synced ที่มั่นใจ |
| Log | – | ทุกเพลงมีหนึ่งบรรทัด `lyrics: ...` บอกว่าได้ record ไหน และหาเจอจาก query ไหน |
| Windows | – | CI รัน test บน macOS และ Windows, ปิด SQLite ก่อนลบไฟล์, CLI พิมพ์ภาษาไทยลง log ได้ |

ส่วนที่เหมือนเดิม: ยังรับเฉพาะ synced หรือ instrumental, ยังยิงผ่าน `lrclib_request` ตัวเดิม (retry เมื่อเจอ 503 และยกเลิกได้เมื่อเปลี่ยนเพลง) และใช้ cache SQLite ตัวเดิม

## ขั้นตอนการค้นหา

```
Track (title, artist, album, duration)  ← มาจาก Chrome extension (mediaSession + <video>)
 │
 ├─ มี album และ duration (โหมด Song)
 │     └─ 1. /api/get  title + artist + album + duration
 ├─ 2. /api/search  ชื่อที่ทำความสะอาดแล้ว + ศิลปิน
 ├─ 3. /api/search  "Artist - Song" / "Song - Artist"   (เฉพาะชื่อที่มีขีดคั่น)
 └─ 4. /api/search  ชื่อเพลงอย่างเดียว (ทางสุดท้าย ศิลปินยังต้อง match)
        │
        ▼
   ทุก record ต้องผ่าน matcher ทั้ง 4 ข้อ
   จากนั้นเลือก synced ก่อน → ความยาวใกล้ที่สุด → album ตรงกัน
   ไม่มีตัวไหนผ่าน → "ไม่เจอ" (เก็บ negative cache 14 วัน)
```

ถ้าไม่มี album จะถือว่าเป็น MV หรือคลิปที่ผู้ใช้อัปโหลด จึงข้าม `/api/get` เพราะ endpoint นี้ต้องมีครบ 4 field ไม่งั้นจะตอบ 400

## วิธีติดตั้งและใช้งาน

### macOS
```bash
cd esp32_lyric
git fetch origin && git checkout feat/esp8266-lyrics-matcher   # หรือ main หลัง merge
esp8266/tools/lyrics.sh stop
python3 esp8266/daemon/lyrics_display_daemon.py cache clear      # แนะนำ (ดูหัวข้อ cache ด้านล่าง)
esp8266/tools/lyrics.sh start
tail -f ~/Library/Logs/g4pys-lyrics-display.log | grep "lyrics:"
```

### Windows (PowerShell)
```powershell
cd esp32_lyric
git fetch origin; git checkout feat/esp8266-lyrics-matcher
py -3 -m pip install -r esp8266\daemon\requirements-windows.txt  # ครั้งแรกครั้งเดียว
powershell -ExecutionPolicy Bypass -File esp8266\tools\lyrics.ps1 stop
py -3 esp8266\daemon\lyrics_display_daemon.py cache clear
powershell -ExecutionPolicy Bypass -File esp8266\tools\lyrics.ps1 start
Get-Content "$env:LOCALAPPDATA\g4pys\lyrics-display.log" -Wait | Select-String "lyrics:"
```

ไม่ต้องติดตั้งอะไรเพิ่ม `lyrics_search.py` ใช้แค่ Python stdlib และ Chrome extension ไม่ต้องแก้

### อ่าน log
```
lyrics: 'Yellow' -> lrclib #12345 'Yellow' / 'Coldplay' (266.0s) via get
lyrics: no confident lrclib match for 'ชื่อเพลง' / 'ศิลปิน'
```
ค่าหลัง `via` บอกว่าหาเจอจาก query ไหน: `get`, `search:title`, `search:artist-song`, `search:song-artist` หรือ `search:title-only`
ถ้าเปลี่ยนเพลงแล้ว**ไม่มี**บรรทัด `lyrics:` ขึ้น แปลว่าผลนั้นมาจาก cache

### ลองเพลงเดียวโดยไม่ต้องเปิด daemon
```bash
python3 esp8266/daemon/lyrics_search.py "ชื่อเพลง" "ศิลปิน" "อัลบั้ม" 245
#   ไม่มีอัลบั้ม (MV):  python3 esp8266/daemon/lyrics_search.py "Artist - Song (Official MV)" "ช่อง" "" 262
```

## วัดผลกับเพลงจริง

`esp8266/tools/compare_lyrics_search.py` รันทั้ง resolver เก่าและใหม่กับ lrclib.net จริง แล้วสรุปผลเพลงต่อเพลง โดยไม่เขียนลง cache

```bash
# เพลงที่เคยเปิดจริง (อ่านจาก cache ของ daemon) ล่าสุด 40 เพลง
python3 esp8266/tools/compare_lyrics_search.py --db ~/.g4pys/lyrics-display.sqlite3 --limit 40
# Windows
py -3 esp8266\tools\compare_lyrics_search.py --db "$env:USERPROFILE\.g4pys\lyrics-display.sqlite3"
```
ผลแต่ละเพลงแบ่งเป็น `same` (ได้ record เดียวกัน), `new_only` (ตัวใหม่หาเจอฝ่ายเดียว), `old_only` (ตัวใหม่ไม่รับ ถ้าตัวเก่าได้ผิดเพลงจริงก็ถือว่าถูก), `different` (ได้เนื้อทั้งคู่แต่คนละ record ต้องดูเองว่าอันไหนถูก) และ `both_miss`

## Cache

- cache อยู่ที่ `~/.g4pys/lyrics-display.sqlite3` (เปลี่ยนได้ด้วย `--db`) และ**ใช้ร่วมกับ daemon ฝั่ง ESP32** ซึ่งยังใช้ logic เดิม
- ผลที่เจอเก็บถาวร ผลที่ไม่เจอเก็บ 14 วัน ถ้าไม่ล้าง เพลงที่เคยเปิดจะยังได้ผลจาก logic เดิม
- ล้างทั้งหมด (ไม่แตะเนื้อที่ import เองด้วย `import-lrc`):
  `python3 esp8266/daemon/lyrics_display_daemon.py cache clear`
- ล้างเพลงเดียว: `... cache clear --video-id <ค่า v= ใน URL>`
- ดูจำนวนแถว: `... cache stats`

## ค่าที่ปรับได้

ตอนนี้เป็นค่าคงที่ในโค้ด แก้แล้วต้อง restart daemon

| ค่า | ไฟล์ | ค่าเริ่มต้น | ใช้ทำอะไร |
|---|---|---|---|
| `SONG_TOLERANCE_S` | `lyrics_search.py` | `3.0` | ความยาวต่างกันได้เท่าไหร่ในโหมด Song |
| `VIDEO_MAX_EXTRA_S` | `lyrics_search.py` | `90.0` | MV ยาวกว่าเพลงจริงได้เท่าไหร่ (intro/outro) |
| `LRCLIB_MAX_REQUESTS_PER_TRACK` | `lyrics_display_daemon.py` | `4` | จำนวน request สูงสุดต่อเพลง |
| `NEGATIVE_CACHE_SECONDS` | `lyrics_display_daemon.py` | 14 วัน | ระยะเวลาที่จำว่า "ไม่เจอ" |
| `LRCLIB_TIMEOUT_SECONDS`, `LRCLIB_RETRY_ATTEMPTS` | `lyrics_display_daemon.py` | `15.0`, `3` | timeout และจำนวนครั้งที่ retry ต่อ request |
| คำบอก version / ของแถม | `_VERSION_RULES`, `_PACKAGING_RE` ใน `lyrics_search.py` | – | เพิ่มคำใหม่ เช่น tag ภาษาไทยที่เจอบ่อย |

`USER_AGENT` และ `TIMEOUT_S` ใน `lyrics_search.py` ใช้เฉพาะตอนรันเป็น CLI ส่วน daemon ใช้ `LRCLIB_USER_AGENT` และ `lrclib_request` ของตัวเอง

**กฎเวลาปรับ matcher**: เอาเพลงจริงที่พลาดมาเพิ่มเป็น test ใน `esp8266/tests/test_lyrics_search.py` ก่อนแก้กฎ แล้วรัน
`python3 -m unittest discover -s esp8266/tests -t .` ให้ผ่าน และ CI ต้องเขียวทั้ง macOS และ Windows

## ข้อจำกัดที่รู้แล้ว

- **ศิลปินไทยใน LRCLIB เขียนเป็นอักษรโรมัน** เช่น "Atom Chanakan" กับ "อะตอม ชนกันต์" จะไม่ match ผลคือหาไม่เจอ ไม่ใช่ได้เนื้อผิดเพลง
- **ชื่อเพลงสองภาษา** แบบ `ชื่อไทย (English Name)` ส่วนในวงเล็บจะถูกตัดทิ้ง ถ้า LRCLIB เก็บชื่อเป็นภาษาอังกฤษก็จะหาไม่เจอ
- **โหมด Song ที่ไม่มี album** จาก `mediaSession` จะถูกนับเป็น MV ใช้กฎความยาวที่หลวมกว่า และข้าม `/api/get`
- **ทางแก้ชั่วคราว** สำหรับเพลงที่หาไม่เจอ: ใช้ `import-lrc` ใส่ไฟล์ `.lrc` เอง
