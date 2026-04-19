# writeup: solid_offset desync on SOLID to non-SOLID multivolume transition in libarchive RAR5

## background

libarchive implements RAR5 parsing in `archive_read_support_format_rar5.c`. The RAR5 format supports solid compression, where the LZ77 back-reference dictionary is shared across multiple files in sequence. To track cumulative positions within this shared stream, the parser maintains `rar->cstate.solid_offset`. This value is used to offset decompression reads and writes relative to where the current file begins inside the shared window buffer.

RAR5 also supports multivolume archives, where a single logical archive is split across multiple `.part*.rar` files. A file's compressed block can span a volume boundary, meaning part of a file's data lives at the end of one volume and the rest at the start of the next.

The bug lives at the intersection of these two features.

## affected versions

Confirmed present in 3.7.2 and 3.8.5. Likely present in all prior 3.x releases. Currently latent in vanilla libarchive because full automatic multivolume chaining for RAR5 is not implemented.

## state machine involved

The RAR5 parser tracks several relevant fields:

```
rar->cstate.solid_offset      // cumulative offset into shared solid window
rar->cstate.write_ptr         // current write position in decompression window
rar->cstate.switch_multivolume // flag: currently crossing a volume boundary
rar->main.solid               // whether current volume's MAIN block declares SOLID
```

The decompression window itself is `rar->cstate.window_buf`, a circular buffer allocated and managed by `init_unpack()`.

## normal flow (no volume boundary)

When a new file entry begins in a normal (non-split) scenario:

1. `archive_read_next_header()` drives the parser forward.
2. `process_head_file()` identifies a new FILE block.
3. `reset_file_context()` is called, which either accumulates `solid_offset` (if `main.solid` is set) or zeroes it (if not), then resets per-file decompression state.
4. `do_uncompress_file()` begins decompression, calling `init_unpack()` if the window buffer needs initialization.

`solid_offset` is always consistent with `write_ptr` and `window_buf` in this path.

## volume boundary flow

When a file's compressed block spans a volume boundary:

1. `merge_block()` detects the continuation and sets `rar->cstate.switch_multivolume = 1`.
2. `advance_multivolume()` opens the next volume and parses its MAIN block, updating `rar->main.solid` from the new volume's header.
3. Back in the file processing path, `reset_file_context()` is guarded:

```
if (!rar->cstate.switch_multivolume)
    reset_file_context(rar);
```

Because `switch_multivolume` is 1, `reset_file_context()` is skipped entirely. `solid_offset` is neither accumulated nor zeroed.

4. `do_uncompress_file()` is called. Inside:

```
if (!rar->main.solid || !rar->cstate.window_buf)
    init_unpack(rar);
```

If the new volume's MAIN block declared non-SOLID (`rar->main.solid == 0`), `init_unpack()` runs. This zeroes `write_ptr` and reallocates or clears `window_buf`.

5. `solid_offset` now points into a previous volume's conceptual data space. The decompression window is fresh zeroed memory. The invariant is broken.

## what breaks

The decompression engine uses `solid_offset` to compute positions for LZ77 back-references and output writes. With a stale `solid_offset` and a zeroed window, every positional calculation is off. Depending on the offset value, output bytes are written to the wrong positions in the window, and back-references read from zeroes or previously written garbage rather than the correct history.

The corruption is silent. There is no internal consistency check that catches the mismatch between `solid_offset` and `write_ptr`. If the archive does not include per-file checksums (RAR5 makes these optional), there is no CRC mismatch either. The output file is written with wrong content and no error is raised.

## why it is latent

libarchive 3.7.x and 3.8.x do not implement automatic multivolume chaining for RAR5. `archive_read_next_header()` stops processing after the first volume. The `archive_read_set_switch_callback()` API exists to let callers feed subsequent volumes, but the internal state machine does not fully execute the `merge_block()` path in a way that drives `switch_multivolume` to 1 and then triggers `init_unpack()` with a changed `main.solid` in the same run.

The PoC in this repository uses the switch callback to feed a two-volume archive where vol1 is SOLID and vol2 is not, with a file block spanning the boundary. Despite this setup, `solid_offset` does not diverge at runtime and SHA-256 of the output matches expected. The code path exists and the logic flaw is structurally present, but the triggering sequence requires multivolume chaining to be fully operational end-to-end.

## execution path summary

```
vol1: SOLID, contains file_a.bin (complete)
vol2: non-SOLID, contains file_b.bin (block starts in vol1, ends in vol2)

merge_block()
    sets switch_multivolume = 1

advance_multivolume()
    parses vol2 MAIN block
    sets main.solid = 0

process_head_file()
    switch_multivolume == 1, so reset_file_context() is skipped
    solid_offset unchanged from vol1 state

do_uncompress_file()
    main.solid == 0, so init_unpack() runs
    write_ptr = 0
    window_buf zeroed

decompress file_b.bin
    solid_offset stale, window_buf fresh
    all positional calculations are wrong
    output corrupted silently
```

## suggested fix

In `init_unpack()`, after determining that the archive is non-SOLID, reset `solid_offset`:

```
if (!rar->main.solid) {
    rar->cstate.solid_offset = 0;
}
```

This is sufficient. It restores the invariant that `solid_offset` is only meaningful relative to the current decompression window, and is zero when that window does not carry solid history.

An alternative location is immediately after `switch_multivolume` is cleared, with an explicit check for the SOLID-to-non-SOLID transition. Either approach closes the gap.

## notes

This was found by reading the state machine logic in `archive_read_support_format_rar5.c` directly. The flaw is not a memory safety issue and has no known exploitability. Its practical risk is in downstream projects or future libarchive versions that complete multivolume RAR5 support without auditing the solid_offset reconciliation path.
