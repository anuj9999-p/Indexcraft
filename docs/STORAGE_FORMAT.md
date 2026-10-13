# Storage format (version 1, little-endian)

One file, `indexcraft.db`: a 64-byte header followed by back-to-back slots.

**File header (64 bytes):** magic `INDXCRFT` · format version · header size · slot header size · creation time · reserved · CRC32 of the first 60 bytes.

**Slot (36-byte header + payload area):**
| field | size | notes |
|---|---|---|
| magic `REC1` | 4 | detects misaligned/garbage slots |
| status | 1 | `1` live, `0` deleted (tombstone) |
| reserved | 3 | must be zero |
| record id (rid) | 8 | monotonically increasing, never reused |
| key | 8 | signed 64-bit |
| capacity | 4 | bytes reserved for payload |
| length | 4 | bytes used (≤ capacity ≤ 1 MiB) |
| CRC32 | 4 | over header fields + payload bytes |

**Operations.** Insert appends a slot (or reuses a deleted one: best fit with slack rule `capacity <= 2*len + 64`; a reused slot receives a *new* rid). Delete flips the status byte in place and puts the slot on an in-memory free list. Reads recheck the slot's rid so a stale locator cannot return another record.

**Open validation.** The whole file is scanned: bad magic, truncation, CRC mismatch, `length > capacity`, duplicate live rid produce `Code::Corrupt` with the byte offset; a duplicate live key is detected while rebuilding the index and fails the open as well. The file is not modified. An advisory `flock` prevents two processes opening the same file.

**Not provided:** WAL, page cache, checksummed free-list persistence, compaction. The free list and the B+ Tree are rebuilt from the file at open.
