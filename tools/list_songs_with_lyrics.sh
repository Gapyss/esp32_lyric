#!/usr/bin/env bash
set -euo pipefail

sqlite3 -header -column /Users/g4pys/.clawdmeter/lyrics.sqlite3 "
WITH songs AS (
  SELECT artist, title
  FROM lyrics
  WHERE lrc IS NOT NULL AND TRIM(lrc) <> ''

  UNION ALL

  SELECT artist, track AS title
  FROM lyrics_cache
  WHERE
    (plain_lyrics IS NOT NULL AND TRIM(plain_lyrics) <> '')
    OR
    (synced_lyrics IS NOT NULL AND TRIM(synced_lyrics) <> '')
)
SELECT DISTINCT
  COALESCE(NULLIF(artist,''),'(unknown artist)') AS artist,
  COALESCE(NULLIF(title,''),'(unknown title)') AS title
FROM songs
ORDER BY artist COLLATE NOCASE, title COLLATE NOCASE;
"
