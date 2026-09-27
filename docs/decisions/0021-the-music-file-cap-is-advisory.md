# 0021 — The streamed-music file cap is advisory

**Date:** 2026-09-26. Owner decision, on #573 row 4.

## Context

`play_music` streams a loose OS file through miniaudio. Its file cap,
`audio.max_music_file_bytes`, is checked from the file's size in metadata
before the stream opens. The stream then reads the file through
miniaudio's own file I/O, so a file that grows between the check and the
reads is streamed past the cap. Decision
[0011](0011-budgets-check-the-handle.md) requires a budget to be enforced
on the handle that is read. Doing that here would need a bounded
miniaudio VFS, a read-callback layer that owns the handle and refuses
past the cap.

Streamed music is never held in memory whole, so the cap does not bound
memory the way the sound and PCM caps do. What it prevents is a mistaken
multi-gigabyte file being opened as music at all.

## Decision

1. **The music cap is advisory.** It refuses a file whose size already
   exceeds the cap when `play_music` is called. It does not bound a file
   that grows while it streams.
2. **The cvar help and the refusal log say so,** so no author reads the
   cap as a guarantee.
3. **Sounds loaded whole are unchanged.** `load_sound` stays bounded on
   the handle it reads (0011), because it holds the file and its decoded
   PCM in memory.

## Consequences

- No bounded miniaudio VFS is written for music.
- If archive-backed streaming lands, its read callback owns the handle.
  The cap can then be enforced on that handle, and this record is
  superseded.
