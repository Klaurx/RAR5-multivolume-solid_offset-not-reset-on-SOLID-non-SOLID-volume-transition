# RAR5-multivolume-solid_offset-not-reset-on-SOLID-non-SOLID-volume-transition

Latent logic bug in libarchive's RAR5 parser. When a multivolume archive transitions from a SOLID to a non-SOLID volume across a block boundary, `solid_offset` is left stale while the decompression window is reinitialized, producing silent output corruption.

Not triggerable in vanilla libarchive 3.7.x / 3.8.x. Relevant to downstream builds that implement full RAR5 multivolume chaining.

## Affected Component

`libarchive/archive_read_support_format_rar5.c`
Confirmed in 3.7.2 and 3.8.5.

## Details

See [docs/writeup.md](docs/writeup.md).

## PoC

See [PoC/trigger_solid_offset.c](PoC/trigger_solid_offset.c).

## License

See [LICENSE](LICENSE).
