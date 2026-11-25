import argparse
import hashlib
import os
import subprocess
import sys
import time

OS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISKOS = os.environ.get("DISKOS_DIR", os.path.expanduser("~/projects/github/diskOS"))
sys.path.insert(0, DISKOS)

from diskos_installer import flasher, platform_probe, state  # noqa: E402

BASE_VER = "257"
SIZE = 100663296
DEFAULT_IMG = os.path.join(OS_DIR, "output", "images", "disc-rootfs-96M.bin")
UNSQUASHFS = os.path.join(OS_DIR, "output", "host", "bin", "unsquashfs")
MASKROM_HOWTO = "Power the Disc OFF, hold Volume-Down, plug in USB (screen stays black), release after 3 s."


def die(msg):
    print(f"disc-flash: {msg}", file=sys.stderr)
    sys.exit(1)


def check_image(path):
    if not os.path.isfile(path):
        die(f"no image at {path} (run make)")
    if os.path.getsize(path) != SIZE:
        die(f"{path} must be exactly {SIZE} bytes")
    with open(path, "rb") as f:
        if f.read(4) != b"hsqs":
            die(f"{path} is not a squashfs image")
    listing = {line.strip().removeprefix("squashfs-root") for line in subprocess.run([UNSQUASHFS, "-l", path], capture_output=True, text=True).stdout.splitlines()}
    for need in ("/linuxrc", "/sbin/init", "/etc/discfw-release", "/etc/inittab"):
        if need not in listing:
            die(f"{path} lacks {need}; refusing to flash an image that cannot boot")


def check_rollback():
    if not state.have_stock_image():
        die("no verified diskOS stock backup; rollback would be impossible. Run diskos-installer once first.")
    st, gen = state.pointer_status()
    print(f"rollback: stock backup {state.stock_paths()[0]} ({st} {gen}) verified")


def wait_maskrom(timeout):
    print(MASKROM_HOWTO)
    end = time.time() + timeout
    while time.time() < end:
        n = platform_probe.maskrom_count()
        if n == 1:
            return
        if n > 1:
            die("more than one Ingenic device in mask-ROM mode")
        time.sleep(1)
    die("no device in mask-ROM mode")


def confirm(what):
    if input(f"{what}\nType 'flash' to continue: ").strip() != "flash":
        die("aborted, nothing written")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_rootfs(args):
    img = os.path.abspath(args.image)
    check_image(img)
    check_rollback()
    print(f"image: {img}\nsha256: {sha256(img)}")
    wait_maskrom(args.timeout)
    confirm("Writes ONLY the rootfs partition (mtd2, 96 MiB) after the device proves its kernel and recovery are "
            f"FiiO V{BASE_VER[0]}.{BASE_VER[1:]}. Takes about 18 minutes; do not unplug.")
    log = os.path.join(OS_DIR, "output", "flash.log")
    try:
        result = flasher.flash(img, BASE_VER, log_path=log)
    except flasher.FlashError as e:
        die(f"{getattr(e, 'code', '')} {e}\n{getattr(e, 'action', '') or ''}\nlog: {log}")
    print(f"flashed: {result}\nPower-cycle the Disc (unplug USB, hold power) to boot discfw.")


def cmd_stock(args):
    check_rollback()
    launcher = os.path.join(DISKOS, "diskos-installer")
    sys.exit(subprocess.call([launcher, "restore-stock"] + (["-y"] if args.yes else []), cwd=DISKOS))


def main():
    p = argparse.ArgumentParser(prog="disc-flash", description="Flash discfw rootfs or restore stock (mask-ROM USB)")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rootfs", help="write output/images/disc-rootfs-96M.bin to mtd2")
    r.add_argument("image", nargs="?", default=DEFAULT_IMG)
    r.add_argument("--timeout", type=int, default=300, help="seconds to wait for mask-ROM device")
    r.set_defaults(func=cmd_rootfs)
    s = sub.add_parser("stock", help="restore the stock V2.57 rootfs (diskos-installer restore-stock)")
    s.add_argument("-y", "--yes", action="store_true")
    s.set_defaults(func=cmd_stock)
    c = sub.add_parser("check", help="validate image and rollback readiness, no USB")
    c.add_argument("image", nargs="?", default=DEFAULT_IMG)
    c.set_defaults(func=lambda a: (check_image(os.path.abspath(a.image)), check_rollback(), print("ok")))
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
