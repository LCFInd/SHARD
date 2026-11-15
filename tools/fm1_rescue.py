#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 LCFInd (DUO v0.8.6's tools/fm1_rescue.py, for SHARD)
"""Install SHARD on an FM-1 whose running firmware will not start an update, through the chip's USB download mode.

An FM-1 whose running firmware does not bring up the update loader (fm1_install.py: "the update loader did not appear",
or "the FM-1 stopped answering after 0 requests"): official V16 on some units, or a firmware that only starts its
updater on the one flash part it knew. This tool goes around the running firmware:

  1. the UBOOT soft key over USB-MIDI (F0 22 24 35 7D F7, as stock V15 and every Felucca/DUO answer): the FM-1
     restarts into the mask ROM's USB download mode, a USB disk "UBOOT1.00" (4C4A:8057);
  2. the package's own update loader, goes into RAM through the ROM's memory-write command (each
     512-byte block scrambled as the ROM wants it), is read back and compared, then started;
  3. fm1_install.py's write: the loader reads the package over USB-MIDI and writes the app area [0x4000, 0x93000), as
     in every install. Nothing below 0x4000 (the head) or above (your stored sounds) is touched.

Until step 3 nothing in the flash is written: a failure before it leaves the FM-1 as it was (the ROM restarts it
after 4 s, a loader no installer talks to restarts it after 60 s). If step 3 stops halfway, run this again.

  sudo python3 fm1_rescue.py PACKAGE.fwsc [--port NAME] [--yes] [--no-install]

Needs pyusb and libusb (pip install pyusb libusb-package), mido with python-rtmidi. On a Mac it needs sudo: macOS
keeps USB disks for itself and only root may take one over. The protocol: kagaimiq's jl-uboot-tool
(docs/usb-protocol.md) and FM-1-transporter (docs/PROTOCOL.md).

Exit codes: as fm1_install.py; 7 the download mode did not answer as expected (nothing written).
"""
import argparse
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1_install as fi   # noqa: E402
import lz4blk              # noqa: E402

UBOOT_KEY = bytes([0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7])
UBOOT_VID = 0x4C4A
LOADER_LOAD = 0x01C0A800                      # tools/build.py LOADER_LOAD, firmware/loader/loader.ld
LOADER_LIMIT = 0x14000                        # its RAM region (code, data and .bss)
ROM_STARTS = (b"SHD-ROMSTART2-0", b"DUO-ROMSTART2-0")   # firmware/loader/loader.c ldr_rom_start: '0' -> '1' (SHARD, DUO)
BLOCK = 512
CMD_WRITE_MEMORY, CMD_READ_MEMORY, CMD_JUMP = 0xFB06, 0xFD07, 0xFB08
MENGLI = bytes.fromhex("c3cfc0e8ced2b0aec4e3a3acd3f1c1d6")   # the ROM's "CrcDecode" key text (GB2312)

DELAY = {"uboot": 15.0, "loader": 30.0}
EXIT_ROM = 7


class RescueError(fi.InstallError):
    pass


def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def crc_cipher(data, key=0xFFFFFFFF):
    """the mask ROM's memory-transfer scrambling (self-inverse), one call per block"""
    out = bytearray(data)
    c = crc16((key >> 16).to_bytes(2, "little"), key & 0xFFFF)
    for i in range(len(out)):
        c = crc16(MENGLI[i % len(MENGLI):i % len(MENGLI) + 1], c)
        out[i] ^= c & 0xFF
    return bytes(out)


# ------------------------------------------------------------ the package ---

def _enc(b):
    out, key = bytearray(b), 0xFFFF
    for i in range(len(out)):
        out[i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF
    return bytes(out)


def ufw_file(logical, name):
    """a file of the package's UFW container (fm1pkg_make.ufw)"""
    hdr = _enc(logical[:0x40])
    n = struct.unpack_from("<H", hdr, 8)[0]
    if crc16(hdr[2:0x40]) != struct.unpack_from("<H", hdr, 0)[0] or not 0 < n < 16:
        raise RescueError("badpkg", "the package's file list is damaged")
    for i in range(n):
        e = _enc(logical[0x40 + i * 0x50:0x90 + i * 0x50])
        off, size = struct.unpack_from("<II", e, 8)
        if e[0x40:].split(b"\0")[0] == name.encode():
            if off + size > len(logical) or crc16(logical[off:off + size]) != struct.unpack_from("<H", e, 4)[0]:
                raise RescueError("badpkg", f"the package's {name} is damaged")
            return logical[off:off + size]
    raise RescueError("badpkg", f"no {name} in the package")


def _head(b):
    """a JLFS head (tools/build.py ldr_head): its own CRC, data CRC, a, b"""
    if len(b) < 32 or crc16(b[2:32]) != struct.unpack_from("<H", b, 0)[0]:
        raise RescueError("badpkg", "the package's update loader is damaged")
    return struct.unpack_from("<HII", b, 2)


def loader_image(logical):
    """the package's update loader as it runs in RAM (build.py ldr_wrap undone), marked as started from the ROM"""
    ota = ufw_file(logical, "ota.bin")
    dcrc, _, blen = _head(ota[:32])
    body = ota[32:32 + blen]
    if len(body) != blen or crc16(body) != dcrc:
        raise RescueError("badpkg", "the package's update loader is damaged")
    icrc, ilen, load = _head(body[:32])
    image, i = bytearray(), 32
    while i < len(body):
        clen, dlen = struct.unpack_from("<II", body, i)
        blk = lz4blk.decompress(body[i + 8:i + 8 + clen])
        if len(blk) != dlen:
            raise RescueError("badpkg", "the package's update loader does not unpack")
        image += blk
        i += 8 + clen
    if len(image) != ilen or crc16(image) != icrc or load != LOADER_LOAD:
        raise RescueError("badpkg", "the package's update loader does not unpack")
    if fi.LOADER_MARK not in image[:32]:
        raise RescueError("badpkg", "the package has no Felucca-style update loader")
    hits = [(m, image.find(m)) for m in ROM_STARTS if image.find(m) >= 0]
    if len(hits) != 1 or image.find(hits[0][0], hits[0][1] + 1) >= 0:
        raise RescueError("badpkg", "this package's loader cannot be started from the download mode: "
                                    "use a SHARD build from 2026-11-15 or later (or DUO v0.8.6 or later)")
    k = hits[0][1]
    if len(image) > LOADER_LIMIT:
        raise RescueError("badpkg", "the package's update loader is too big")
    image[k + 14] = ord("1")
    return bytes(image)


# ------------------------------------------------------- USB mass storage ---

class BotError(Exception):
    pass


class UsbBot:
    """SCSI over USB Bulk-Only Transport on the download mode's disk (pyusb)"""

    def __init__(self, dev):
        import usb.util
        self.dev, self.tag = dev, 1
        try:
            dev.set_configuration()
        except Exception:
            pass
        cfg = dev.get_active_configuration()
        intf = next(i for i in cfg if i.bInterfaceClass == 8)
        self.ifn = intf.bInterfaceNumber
        try:
            if dev.is_kernel_driver_active(self.ifn):
                dev.detach_kernel_driver(self.ifn)
        except NotImplementedError:
            pass
        except Exception as e:
            raise BotError(f"cannot take the USB disk from the system ({e}): run with sudo") from e
        try:
            usb.util.claim_interface(dev, self.ifn)
        except Exception as e:
            raise BotError(f"cannot claim the USB disk ({e}): run with sudo") from e
        d = usb.util.ENDPOINT_IN
        self.ep_in = next(e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) == d)
        self.ep_out = next(e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) != d)

    def execute(self, cdb, data_out=None, data_in=0, timeout=3000):
        n = len(data_out) if data_out is not None else data_in
        flags = 0x80 if data_out is None and data_in else 0
        cbw = struct.pack("<IIIBBB", 0x43425355, self.tag, n, flags, 0, len(cdb)) + cdb.ljust(16, b"\0")
        tag, self.tag = self.tag, self.tag + 1
        try:
            self.ep_out.write(cbw, timeout)
            got = b""
            if data_out is not None and len(data_out):
                self.ep_out.write(data_out, timeout)
            elif data_in:
                got = bytes(self.ep_in.read(data_in, timeout))
            csw = bytes(self.ep_in.read(13, timeout))
        except Exception as e:
            raise BotError(str(e)) from e
        if len(csw) != 13 or csw[:4] != b"USBS" or struct.unpack_from("<I", csw, 4)[0] != tag or csw[12]:
            raise BotError("command refused")
        return got

    def close(self):
        import usb.util
        try:
            usb.util.release_interface(self.dev, self.ifn)
            usb.util.dispose_resources(self.dev)
        except Exception:
            pass


def usb_backend():
    try:
        import libusb_package
        return libusb_package.get_libusb1_backend()
    except ImportError:
        return None


def find_uboot(secs):
    """the download mode's disk, once it is there; None after secs"""
    import usb.core
    be, end = usb_backend(), time.monotonic() + secs
    while time.monotonic() < end:
        dev = usb.core.find(idVendor=UBOOT_VID, backend=be)
        if dev is not None:
            return dev
        time.sleep(0.05)
    return None


# ------------------------------------------------------- the ROM's commands ---

def cdb(cmd, args):
    return (cmd.to_bytes(2, "big") + args).ljust(16, b"\xFF")


def inquiry(bot):
    d = bot.execute(b"\x12\0\0\0\x24\0", data_in=36)
    return d[8:16].decode("ascii", "replace").strip(), d[16:32].decode("ascii", "replace").strip()


def mem_write(bot, addr, data):
    s = crc_cipher(data)
    bot.execute(cdb(CMD_WRITE_MEMORY, addr.to_bytes(4, "big") + len(s).to_bytes(2, "big") + b"\0"
                    + crc16(s).to_bytes(2, "little")), data_out=s)


def mem_read(bot, addr, n):
    return bot.execute(cdb(CMD_READ_MEMORY, addr.to_bytes(4, "big") + n.to_bytes(2, "big")), data_in=n)


def mem_jump(bot, addr, arg=0):
    try:
        bot.execute(cdb(CMD_JUMP, addr.to_bytes(4, "big") + arg.to_bytes(2, "big")), data_in=16)
    except BotError:
        pass                                    # (the loader takes the USB away: a missing answer is expected)


def upload(bot, image, step=lambda *a: None):
    """the loader into RAM, read back and compared; then started"""
    for i in range(0, len(image), BLOCK):
        mem_write(bot, LOADER_LOAD + i, image[i:i + BLOCK])
        step("upload", min(99, (i + BLOCK) * 100 // len(image)))
    for i in range(0, len(image), BLOCK):
        want = image[i:i + BLOCK]
        got = mem_read(bot, LOADER_LOAD + i, len(want))
        if got != want and crc_cipher(got) != want:
            raise RescueError("rom", f"the loader did not arrive intact in RAM (block {i // BLOCK}); nothing was "
                                     "written: the FM-1 restarts by itself in a few seconds")
    step("upload", 100)
    mem_jump(bot, LOADER_LOAD)


# ---------------------------------------------------------------- the run ---

class Steps:
    NAMES = {"upload": "loader into RAM"}

    def __init__(self, out):
        self.out, self.last = out, None

    def __call__(self, k, a=None):
        if k == "upload":
            if self.last != k:
                print(f"{self.NAMES[k]}: ", end="", file=self.out, flush=True)
            if a == 100:
                print("ok", file=self.out, flush=True)
            self.last = k


def rescue(a, backend, out, ask, find=find_uboot, open_bot=UsbBot):
    product, logical = fi.load_package(a.package, False)
    image = loader_image(logical)
    print(f"package: {product}  ({a.package}); its update loader: {len(image)} B", file=out)
    up = fi.Updater(backend, a.port)
    dev = find(0.2)
    if dev is None:
        mid = up.find()
        if not mid:
            raise fi.not_found(up)
        print(f"device:  {mid.id.text}  ({mid.name})", file=out)
        if mid.id.loader:                       # already in the update loader: the plain install finishes it
            mid.link.close()
            print("the FM-1 is already in update mode: run fm1_install.py to finish", file=out)
            return 0
        if not a.yes and not ask("Restart the FM-1 into its USB download mode and install from there? [y/N] "):
            mid.link.close()
            print("cancelled", file=out)
            return 1
        mid.link.send(UBOOT_KEY)
        time.sleep(0.05)
        mid.link.close()
        print("download mode: ", end="", file=out, flush=True)
        dev = find(DELAY["uboot"])
        if dev is None:
            print("not seen", file=out)
            raise RescueError("rom", "the FM-1 did not show up in download mode (USB 4C4A); nothing was written. "
                                     "Turn it off and on, and try again")
    bot = open_bot(dev)
    try:
        vendor, prod = inquiry(bot)
        if not prod.startswith("UBOOT"):
            raise RescueError("rom", f"the USB disk is {vendor} {prod}, not the download mode; nothing was written")
        print(f"{vendor} {prod}", file=out)
        upload(bot, image, Steps(out))
    finally:
        bot.close()
    if a.no_install:
        print("the loader runs: now fm1_install.py PACKAGE finishes the install (within 60 s)", file=out)
        return 0
    print("waiting for the update loader on USB-MIDI...", file=out)
    ota = up.wait_for(lambda i: i.loader, DELAY["loader"])
    if not ota:
        raise fi.InstallError("noloader", "the update loader did not show up on USB-MIDI; nothing was written. "
                                          "It restarts the FM-1 after 60 s; then turn it off and on if needed")
    print(f"loader:  {ota.id.text}  ({ota.name})", file=out)
    prog = fi.Progress(out)
    try:
        up.install(ota, logical, product, prog)
    finally:
        prog.end()
    return 0


def main(argv=None, backend=None, out=sys.stdout, ask=fi.ask_tty, **hooks):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package", help="SHARD package (.fwsc)")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    ap.add_argument("--no-install", action="store_true", help="only start the loader (then run fm1_install.py)")
    a = ap.parse_args(argv)
    try:
        if not hooks:
            try:
                import usb.core  # noqa: F401
            except ImportError:
                raise RescueError("usage", "needs pyusb and libusb (pip install pyusb libusb-package)")
        if backend is None:
            try:
                backend = fi.MidoBackend()
            except ImportError:
                raise fi.InstallError("usage", "needs mido and python-rtmidi (pip install mido python-rtmidi)")
        return rescue(a, backend, out, ask, **hooks)
    except fi.InstallError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT_ROM if e.code == "rom" else fi.EXIT.get(e.code, 1)
    except BotError as e:
        print(f"error: USB: {e}", file=sys.stderr)
        return EXIT_ROM
    except KeyboardInterrupt:
        print("\ninterrupted: if the FM-1 is in update mode, run fm1_install.py to finish", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
