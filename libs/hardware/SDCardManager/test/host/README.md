# SD stream host tests

Run `sh libs/hardware/SDCardManager/test/host/run.sh` from the SDK root.
The runner compiles the production `SDCardManager.cpp` and public header with
host stubs for Arduino, SdFat, board configuration and the task watchdog.
No implementation is copied or extracted into the tests.

Coverage includes exact output across buffer/chunk boundaries, positive short
writes, zero-progress writes, invalid output counts, SD read errors before and
after partial output, premature EOF, zero-progress reads, short reads, 64-bit
file sizes, file closure, uninitialized/open-failure paths, watchdog subscription
and time-budgeted yields during both ordinary transfers and partial writes.
The timer rollover case checks unsigned elapsed-time arithmetic.

For host sanitizers:

```sh
CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  sh libs/hardware/SDCardManager/test/host/run.sh
```

These tests verify transfer logic, not SD hardware or network timing. On a
device, transfer a multi-chunk file through a `Print`-derived network client and
compare its size and hash with the SD original. Then interrupt the connection
and check that the helper reports failure without restarting the device.

## SDMMC volume layouts (real SdFat)

The separate mount regression compiles production `src/SdmmcVolume.h` with the
unmodified filesystem sources from SdFat 2.3.1. Supply an existing checkout; the
test never downloads dependencies or opens a physical disk:

```sh
python3 libs/hardware/SDCardManager/test/host/test_sdmmc_volume.py \
  --sdfat /path/to/SdFat
```

The in-memory block device represents a sparse 16 GB FAT32 card (512-byte
sectors, 8192-byte clusters). It covers an MBR partition, a whole-device
superfloppy, MBR priority when both layouts are valid, file contents read through
`FsFile`, remount after filesystem detachment/read failure, and 14 invalid-media cases.
Every device write callback rejects and counts writes; successful and failed
mounts must leave that count at zero. Filesystem parsing is real SdFat code, not
a fake `begin()` or a copied BPB parser. The host-only compatibility header adds
the missing `__FlashStringHelper` declaration and unused filename-hash clock.

Memory allocation in the sparse test image is host-only. The production helper
reuses the existing `FsVolume` and device and adds no heap allocation, buffer,
task, or hardware initialization. The SPI mounting path is unchanged.

For AddressSanitizer:

```sh
CXXFLAGS='-fsanitize=address -fno-omit-frame-pointer' \
  python3 libs/hardware/SDCardManager/test/host/test_sdmmc_volume.py \
  --sdfat /path/to/SdFat
```

SdFat 2.3.1 itself triggers UBSan alignment checks in its default little-endian
helpers and pointer-overflow checks for a null-buffer skip in `FatFile::read()`.
To check other undefined behavior as well, use
`-fsanitize=address,undefined -fno-sanitize=alignment,pointer-overflow
-fno-sanitize-recover=all -fno-omit-frame-pointer` in `CXXFLAGS`. These exclusions
apply to this host run; no vendor source or firmware flags are changed.

The invalid cases cover fields SdFat validates: sector size, non-power-of-two
cluster sizes, FAT count, blank sectors, and read failures. They do not claim
exhaustive filesystem validation: for example, SdFat 2.3.1 accepts a zero
sectors-per-cluster BPB during mounting. The fallback intentionally delegates
validation to SdFat, just like the existing MBR path. On hardware, verify both
layouts mount and an existing book opens, then check a USB disk exit/remount.
