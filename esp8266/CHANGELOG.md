# Changelog: esp8266/

## Unreleased: Lyrics search ที่ match เพลงจริง (PR #15)

รายละเอียดทั้งหมดและวิธีตั้งค่าอยู่ที่ [docs/lyrics-search.md](docs/lyrics-search.md)

### Added
- `daemon/lyrics_search.py`: เลือก record จาก LRCLIB เมื่อชื่อเพลง (หลังตัดของแถม), version tag, ศิลปิน และความยาวตรงกันครบเท่านั้น
- อ่านชื่อ MV แบบ `Artist - Song` และ `Song - Artist` ได้ รองรับคลิปจากช่องค่ายเพลง ถ้ายังไม่เจอจะค้นด้วยชื่อเพลงอย่างเดียวเป็นทางสุดท้าย
- Log หนึ่งบรรทัดต่อเพลง: `lyrics: <title> -> lrclib #id ... via <query>`
- `LyricsStore.close()`
- `tools/compare_lyrics_search.py`: เทียบ resolver เก่ากับใหม่บน lrclib.net จริง ใช้ cache ของคุณหรือไฟล์ JSON ก็ได้
- CI `.github/workflows/esp8266-tests.yml` รัน test บน macOS และ Windows
- `CLAUDE.md` บันทึกเงื่อนไขว่าต้องรันได้ทั้ง macOS และ Windows

### Changed
- `fetch_lrclib()` เรียกผ่าน `lyrics_search` ยังรับเฉพาะ synced หรือ instrumental และยังใช้ `lrclib_request` (retry, cancel) ตัวเดิม
- เพลงที่ไม่มี album ไม่เรียก `/api/get` แล้ว (เดิมได้ 400 ทุกครั้ง)
- จำนวน request ต่อเพลงเพิ่มจากสูงสุด 2 เป็นสูงสุด 4

### Fixed
- ได้เนื้อของศิลปินอื่นหรือเวอร์ชัน live/remix เพราะความยาวบังเอิญใกล้กัน
- MV ที่มี intro/outro หาเนื้อไม่เจอ
- Test บน Windows: ปิด SQLite ก่อนลบไฟล์ชั่วคราว (WinError 32) และข้ามการเช็ค mode `0600` บน Windows
- Test theme บน Windows เคยพังเป็นบางครั้งเพราะนาฬิกาเดินระหว่างการ render สองครั้ง
- CLI ของ `lyrics_search` พิมพ์ภาษาไทยลง log ที่ redirect บน Windows (cp1252) ได้

### Upgrade
- แนะนำให้ล้าง cache หลังอัปเดต: `python3 esp8266/daemon/lyrics_display_daemon.py cache clear`
