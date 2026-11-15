#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 LCFInd (DUO v0.8.6, for SHARD)
"""tools/fm1_rescue.py against a simulated FM-1 and a simulated mask ROM (no hardware, no mido, no pyusb).
The FM-1 (tests/install_test.py FakeFM1) answers the UBOOT soft key by leaving USB-MIDI; the ROM is a USB disk
"UBOOT1.00" that takes memory writes (scrambled, CRC-checked), reads them back and jumps; the jump to the loader
brings the FM-1 back as "Felucca Update", which fm1_install.py's write then talks to. Run from the repo root:
  python3 tests/rescue_test.py"""
import io
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))
import install_test as T   # noqa: E402  (FakeFM1, short delays)
import fm1_rescue as R     # noqa: E402
import fm1pkg_make         # noqa: E402
import lz4blk              # noqa: E402

R.DELAY.update(uboot=0.5, loader=2)
failed = 0


def ok(cond, what):
    global failed
    print(f"{what:<72} {'ok' if cond else 'FAIL'}")
    failed += not cond


# --------------------------------------------------------------- a package ---

def ldr_head(dcrc, a, b, attr, name=b"usb_hid_ota.bin"):           # tools/build.py ldr_head
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", R.crc16(body)) + body


def ldr_wrap(image, load=R.LOADER_LOAD):                            # tools/build.py ldr_wrap
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        c = lz4blk.compress(image[i:i + 4096])
        blocks += struct.pack("<II", len(c), len(image[i:i + 4096])) + c
    body = ldr_head(R.crc16(image), len(image), load, 0) + bytes(blocks)
    return ldr_head(R.crc16(body), 0x20, len(body), 0x41) + body


def loader(rom_mark=R.ROM_STARTS[0], size=9000):
    img = bytearray((i * 13 + (i >> 8)) & 0xFF for i in range(size))
    img[0:2] = b"\x00\x00"
    img[2:18] = T.I.LOADER_MARK
    if rom_mark:
        img[5000:5000 + len(rom_mark) + 1] = rom_mark + b"\0"
    return bytes(img)


def package(ldr=None, product="FM-1_900", load=R.LOADER_LOAD):
    flash = bytes((i * 5) & 0xFF for i in range(0x3000))
    return fm1pkg_make.ufw(flash, ldr_wrap(ldr if ldr is not None else loader(), load), product)


# ------------------------------------------------------------ the mask ROM ---

class FakeROM:
    """the download mode's disk; mem holds what the ROM wrote (descrambled)"""

    def __init__(self, fm1, product="UBOOT1.00", plain_reads=False, corrupt=None):
        self.fm1, self.product, self.plain_reads, self.corrupt = fm1, product, plain_reads, corrupt
        self.mem, self.jumped, self.cmds, self.closed = {}, None, [], False

    def execute(self, cdb, data_out=None, data_in=0):
        if cdb[0] == 0x12:
            return (b"\0" * 8 + b"WL80    " + self.product.ljust(16).encode() + b"1.00")[:data_in]
        cmd = int.from_bytes(cdb[:2], "big")
        addr, n = int.from_bytes(cdb[2:6], "big"), int.from_bytes(cdb[6:8], "big")
        self.cmds.append(cmd)
        if cmd == R.CMD_WRITE_MEMORY:
            if len(data_out) != n or R.crc16(data_out) != int.from_bytes(cdb[9:11], "little") or n > 512:
                raise R.BotError("refused")
            d = R.crc_cipher(data_out)
            if self.corrupt is not None and addr == R.LOADER_LOAD + 512 * self.corrupt:
                d = bytes([d[0] ^ 1]) + d[1:]
            for i, b in enumerate(d):
                self.mem[addr + i] = b
            return b""
        if cmd == R.CMD_READ_MEMORY:
            d = bytes(self.mem.get(addr + i, 0xFF) for i in range(n))
            return d if self.plain_reads else R.crc_cipher(d)
        if cmd == R.CMD_JUMP:
            self.jumped = addr
            self.fm1.boot("ota-FM-1_900", "Felucca Update")
            raise R.BotError("gone")
        raise R.BotError("unknown")

    def close(self):
        self.closed = True

    def image(self, n):
        return bytes(self.mem.get(R.LOADER_LOAD + i, 0xFF) for i in range(n))


class Rig:
    """an FM-1 that goes into the download mode on the UBOOT key"""

    def __init__(self, logical, identity="FM-1_900", report=None, **rom):
        self.fm1 = T.FakeFM1(logical, identity=identity, name="FM-1")
        self.fm1.waiting, self.fm1.queue = None, []
        self.rom = FakeROM(self.fm1, **rom)
        self.in_uboot, self.keys = False, 0
        rx = self.fm1.rx

        def on_rx(d):
            if d == R.UBOOT_KEY:
                self.keys += 1
                self.fm1.connected, self.in_uboot = False, True
            elif d == T.I.UPGRADE and report is not None and self.fm1.identity.startswith("ota-"):
                q = []                              # firmware/loader/loader.c ldr_tell
                for i, v in enumerate(report):
                    v &= 0xFFFFFFFF
                    q += [(0xD0000000 | (2 * i) << 16 | (v & 0xFFFF), 4), (0xD0000000 | (2 * i + 1) << 16 | v >> 16, 4)]
                self.fm1.queue = q + [(0xD0FF0000, 4)]
                self.fm1.next()
            else:
                rx(d)
        self.fm1.rx = on_rx

    def find(self, secs):
        return self.rom if self.in_uboot else None


def run(args, rig, answer=True):
    out, err = io.StringIO(), io.StringIO()
    old, sys.stderr = sys.stderr, err
    try:
        rc = R.main(args, backend=rig.fm1, out=out, ask=lambda _p: answer,
                    find=rig.find, open_bot=lambda dev: dev)
    finally:
        sys.stderr = old
    return rc, out.getvalue(), err.getvalue()


# ------------------------------------------------------------------- tests ---

def unit():
    d = bytes(range(256)) * 2
    ok(R.crc_cipher(R.crc_cipher(d)) == d and R.crc_cipher(d) != d, "ROM scrambling: self-inverse, not identity")
    ok(R.MENGLI == "孟黎我爱你，玉林".encode("gb2312"), "ROM scrambling: its key text")
    ok(R.crc16(b"123456789") == 0x31C3, "CRC-16 (XMODEM: 31C3)")
    raw = package()
    img = R.loader_image(T.I.logical_image(raw))
    want = bytearray(loader())
    want[5014] = ord("1")
    ok(img == bytes(want), "the loader unpacked from the package, marked started-from-ROM ('0' -> '1')")
    for what, p in (("a loader without the ROM-start mark (SHARD before 2026-11-15)", package(loader(rom_mark=None))),
                    ("a loader for another load address", package(load=0x01C02000)),
                    ("a loader over its RAM region", package(loader(size=R.LOADER_LIMIT + 4)))):
        try:
            R.loader_image(T.I.logical_image(p))
            ok(False, f"refused: {what}")
        except R.RescueError:
            ok(True, f"refused: {what}")
    bad = bytearray(T.I.logical_image(raw))
    bad[-100] ^= 0xFF
    try:
        R.loader_image(bytes(bad))
        ok(False, "refused: a damaged ota.bin")
    except R.RescueError:
        ok(True, "refused: a damaged ota.bin")


def flows():
    raw = package()
    logical = T.I.logical_image(raw)
    p = T.pkgfile("duo.fwsc", raw)
    img = R.loader_image(logical)

    rig = Rig(logical)
    rc, out, err = run([p], rig)
    ok(rc == 0 and rig.keys == 1, "rescue: the UBOOT key sent once, install done")
    ok(rig.rom.image(len(img)) == img and rig.rom.jumped == R.LOADER_LOAD,
       "  ... the loader in RAM at 0x01C0A800, byte for byte, then started there")
    ok(rig.rom.cmds.index(R.CMD_JUMP) == len(rig.rom.cmds) - 1 and R.CMD_READ_MEMORY in rig.rom.cmds,
       "  ... read back before the jump, the jump last")
    ok(rig.fm1.upgrades == 1 and rig.fm1.served == 7 and not rig.fm1.bad,
       "  ... then fm1_install's write: every read answered with the package")
    ok("WL80 UBOOT1.00" in out and "loader into RAM: ok" in out, "  ... what it printed")

    rig = Rig(logical, plain_reads=True)
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == 0 and rig.rom.jumped, "rescue: a ROM that reads back unscrambled is accepted too")

    rig = Rig(logical, corrupt=3)
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == R.EXIT_ROM and rig.rom.jumped is None and rig.fm1.upgrades == 0 and "block 3" in err,
       "rescue: a block that arrives damaged: no jump, nothing written")

    rig = Rig(logical, product="Disk")
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == R.EXIT_ROM and not rig.rom.cmds, "rescue: a USB disk that is not UBOOT: no command sent")

    rig = Rig(logical)
    rc, out, err = run([p], rig, answer=False)
    ok(rc == 1 and rig.keys == 0 and not rig.rom.cmds, "rescue: answered no: the FM-1 left alone")

    rig = Rig(logical)
    rig.find = lambda secs: None
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == R.EXIT_ROM and rig.keys == 1 and "did not show up" in err, "rescue: no download mode after the key")

    rig = Rig(logical)
    rig.in_uboot = True
    rig.fm1.connected = False
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == 0 and rig.keys == 0 and rig.rom.jumped, "rescue: already in download mode: straight to the upload")

    rig = Rig(logical)
    rc, out, err = run([p, "--yes", "--no-install"], rig)
    ok(rc == 0 and rig.rom.jumped and rig.fm1.upgrades == 0, "rescue --no-install: the loader started, no write")

    rig = Rig(logical, identity="ota-FM-1_900")
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == 0 and rig.keys == 0 and "already in update mode" in out, "rescue: the loader already up: left to fm1_install")

    old = T.I.LOADER_MARK
    rc, out, err = run([T.pkgfile("old.fwsc", package(loader(rom_mark=None)))], Rig(logical))
    ok(rc == 2 and "v0.8.6" in err and T.I.LOADER_MARK == old, "rescue: a package from before v0.8.6: refused")


def reports():
    raw = package()
    logical = T.I.logical_image(raw)
    p = T.pkgfile("duo.fwsc", raw)
    diag = [0x31, 0x3E, 0x3F, 0, 0, 0x1029, 0, 0x20, 0, 0xC86014, 3, -53, 0x12345678, 0x9ABCDEF0]
    rig = Rig(logical, report=diag)
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == 4 and "(-53): the flash write test failed. Nothing of the installed firmware was erased." in err,
       "a loader that stops: its report (why) printed, exit 4")
    ok("flash_id=00C86014" in err and "pins=3" in err and "rc=-53" in err and "PORTD_DIR=0000003E" in err
       and "head36=9ABCDEF0" in err, "  ... every value, each from its two halves")
    rig = Rig(logical, report=diag[:11] + [-10] + diag[12:])
    rc, out, err = run([p, "--yes"], rig)
    ok(rc == 4 and "run the install again" in err and "Nothing of" not in err,
       "  ... a write that stopped partway: no 'nothing erased'")
    src = (ROOT / "firmware/loader/loader.c").read_text()
    names = [n for n in ("LDR_D_ID = 9", "LDR_D_N = 14") if n in src]
    ok(len(names) == 2 and len(T.I.REPORT_NAMES) == 14 and T.I.REPORT_NAMES[9] == "flash_id",
       "  ... the report's layout is the loader's (14 values, the id at 9)")


def firmware_mark():
    src = (ROOT / "firmware/loader/loader.c").read_text()
    ok(f'"{R.ROM_STARTS[0].decode()}"' in src and "ldr_rom_start[14] == '1'" in src,
       "the loader's ROM-start mark is the one the tool sets (byte 14)")
    ld = (ROOT / "firmware/loader/loader.ld").read_text()
    ok("ORIGIN = 0x01C0A800, LENGTH = 0x14000" in ld, "the loader's RAM region is the one the tool loads")


if __name__ == "__main__":
    unit()
    flows()
    reports()
    firmware_mark()
    print("rescue:", "FAILED" if failed else "all ok")
    sys.exit(1 if failed else 0)
