#!/usr/bin/env python3
"""Patch ieee80211_raw_frame_sanity_check in libnet80211.a to always return 0.

ESP-IDF v5 enforces a check inside the closed-source net80211 blob that
rejects any 802.11 frame whose source MAC (addr2) doesn't match this device's
own MAC. A deauth attack legitimately spoofs the AP's BSSID, so the only
reliable override is to neutralise the function body. linker --wrap doesn't
work here because the call originates inside the same archive and Xtensa
linker relaxation resolves the reference to the original symbol before the
wrap can take effect (verified empirically).

This script:
  1. Copies the system libnet80211.a into the build dir (read-only-safe).
  2. Extracts ieee80211_output.o from the archive.
  3. Patches the first 4 bytes after the `entry` instruction at the start of
     ieee80211_raw_frame_sanity_check with `movi.n a2, 0; retw.n` so the
     function returns 0 immediately, before reaching the type/MAC checks
     that trigger `unsupport frame type: 0c0` and err=258.
  4. Re-archives the patched .o into the local libnet80211.a.

Run from CMake (see CMakeLists.txt) before linking. Idempotent.
"""
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# args: <src_lib> <out_lib>
src_lib = Path(sys.argv[1])
out_lib = Path(sys.argv[2])
ar      = sys.argv[3] if len(sys.argv) > 3 else "xtensa-esp32s3-elf-ar"

# The .o file inside the archive that contains ieee80211_raw_frame_sanity_check.
TARGET_O = "ieee80211_output.o"

# Function header: original `entry a1, 64` (3 bytes: 36 81 00) at the start.
# We keep that and patch the next 4 bytes to `movi.n a2, 0; retw.n`.
# This requires finding the ELF section .text.ieee80211_raw_frame_sanity_check
# inside the .o and writing at section_offset + 3.

PATCH_BYTES = bytes([0x0c, 0x02, 0x1d, 0xf0])  # movi.n a2,0 ; retw.n
ENTRY_BYTES = bytes([0x36, 0x81, 0x00])         # entry a1, 64 — sanity check

def patch_o(o_path: Path) -> None:
    data = bytearray(o_path.read_bytes())
    # Parse ELF section table to find .text.ieee80211_raw_frame_sanity_check.
    if data[:4] != b'\x7fELF':
        raise SystemExit(f"{o_path}: not an ELF object")
    # ELF32 header: e_shoff @0x20, e_shentsize @0x2e, e_shnum @0x30, e_shstrndx @0x32
    e_shoff     = struct.unpack_from('<I', data, 0x20)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x2e)[0]
    e_shnum     = struct.unpack_from('<H', data, 0x30)[0]
    e_shstrndx  = struct.unpack_from('<H', data, 0x32)[0]
    # Read shstrtab
    shstr_off = e_shoff + e_shstrndx * e_shentsize
    shstr_file_off = struct.unpack_from('<I', data, shstr_off + 0x10)[0]
    shstr_size     = struct.unpack_from('<I', data, shstr_off + 0x14)[0]
    shstr = bytes(data[shstr_file_off:shstr_file_off + shstr_size])

    target_name = b'.text.ieee80211_raw_frame_sanity_check'
    section_off = None
    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        sh_name = struct.unpack_from('<I', data, sh + 0x00)[0]
        sh_off  = struct.unpack_from('<I', data, sh + 0x10)[0]
        sh_sz   = struct.unpack_from('<I', data, sh + 0x14)[0]
        # Read null-terminated name from shstrtab
        end = shstr.index(b'\x00', sh_name)
        name = shstr[sh_name:end]
        if name == target_name:
            section_off = sh_off
            section_sz  = sh_sz
            break
    if section_off is None:
        raise SystemExit(f"{o_path}: section {target_name.decode()} not found")

    # The function does NOT start at section offset 0 — there's a literal pool
    # in front of it. Scan for the `entry a1, 64` opcode (36 81 00) which is
    # the canonical 3-byte prolog of this function (frame size 64 → 0x40,
    # encoded as 008136 → little-endian 36 81 00).
    fn_start = data.find(ENTRY_BYTES, section_off, section_off + section_sz)
    if fn_start < 0:
        raise SystemExit("entry instruction not found in section")
    patch_at = fn_start + 3
    if data[patch_at:patch_at+4] == PATCH_BYTES:
        print(f"[patch_net80211] already patched at file offset 0x{patch_at:x}")
        return
    print(f"[patch_net80211] patching {o_path.name} at file offset 0x{patch_at:x}")
    data[patch_at:patch_at+4] = PATCH_BYTES
    o_path.write_bytes(bytes(data))

def main() -> None:
    out_lib.parent.mkdir(parents=True, exist_ok=True)
    if out_lib.exists() and out_lib.stat().st_mtime >= src_lib.stat().st_mtime:
        # Already patched and up-to-date.
        print(f"[patch_net80211] {out_lib} up-to-date")
        return
    shutil.copy2(src_lib, out_lib)
    workdir = out_lib.parent / "_net80211_extract"
    if workdir.exists():
        shutil.rmtree(workdir)
    workdir.mkdir(parents=True)
    # Extract single .o
    subprocess.check_call([ar, "x", str(out_lib.resolve()), TARGET_O], cwd=workdir)
    o_path = workdir / TARGET_O
    if not o_path.is_file():
        raise SystemExit(f"failed to extract {TARGET_O} from {src_lib}")
    patch_o(o_path)
    # Replace in archive
    subprocess.check_call([ar, "rs", str(out_lib.resolve()), TARGET_O], cwd=workdir)
    # Bump mtime so cmake sees it as newer than source.
    os.utime(out_lib, None)
    shutil.rmtree(workdir)
    print(f"[patch_net80211] patched {out_lib}")

if __name__ == "__main__":
    main()
