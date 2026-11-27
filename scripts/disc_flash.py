import argparse
import fcntl
import hashlib
import os
import stat
import subprocess
import sys
import time

if sys.version_info < (3, 9):
    sys.exit("disc-flash: needs Python 3.9 or newer")

OS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DISKOS = os.environ["DISKOS_DIR"]
sys.path.insert(0, DISKOS)

from diskos_installer import flasher, platform_probe  # noqa: E402

BASE_VER = "257"
SIZE = 100663296
IMAGES = os.path.join(OS_DIR, "output", "images")
DEFAULT_IMG = os.path.join(IMAGES, "disc-rootfs-96M.bin")
STOCK_IMG = os.path.join(IMAGES, "stock-rootfs-v257-96M.bin")
STOCK_LEN = 80596992
STOCK_SHA256 = "111e4dd7ee3d7ff91ba7e61181690be7ffd22bd1bbd13ad513f5f015ffb302ae"
UNSQUASHFS = os.path.join(OS_DIR, "output", "host", "bin", "unsquashfs")
LOCK = "/tmp/diskos-installer.lock"
UDEV_DIRS = ("/etc/udev/rules.d", "/run/udev/rules.d", "/usr/lib/udev/rules.d", "/lib/udev/rules.d")
MASKROM_HOWTO = "Power the Disc OFF, hold Volume-Down, plug in USB (screen stays black), release after 3 s."
AFTER_FAILURE = ("Do not retry at once. If the result is unknown, the Disc may still be writing: leave it connected for 25 "
                 "minutes. Then unplug USB and hold Power about 6 s. Every new attempt needs a full power-off and a fresh "
                 "start in mask-ROM mode.")


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


def check_stock():
    if not os.path.isfile(STOCK_IMG):
        die(f"no stock image at {STOCK_IMG} (run make); without it there is no way back to stock")
    if os.path.getsize(STOCK_IMG) != SIZE:
        die(f"{STOCK_IMG} must be exactly {SIZE} bytes")
    h = hashlib.sha256()
    with open(STOCK_IMG, "rb") as f:
        h.update(f.read(STOCK_LEN))
        if h.hexdigest() != STOCK_SHA256:
            die(f"{STOCK_IMG} is not FiiO's V2.57 rootfs")
        for chunk in iter(lambda: f.read(1 << 20), b""):
            if chunk.count(0) != len(chunk):
                die(f"{STOCK_IMG} has data after FiiO's V2.57 rootfs")
    print(f"rollback: {STOCK_IMG} is FiiO's V2.57 rootfs, verified")


def check_udev():
    if os.geteuid() == 0:
        return
    for d in UDEV_DIRS:
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            try:
                with open(os.path.join(d, n), errors="replace") as f:
                    text = f.read().lower()
            except OSError:
                continue
            if "a108" in text and "eaef" in text:
                return
    rule = os.path.join(DISKOS, "udev", "70-diskos-maskrom.rules")
    die("no udev rule lets your user open the Disc in mask-ROM mode. Install it once:\n"
        f"sudo install -m 0644 {rule} /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger")


def lock():
    try:
        fd = os.open(LOCK, os.O_CREAT | os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, 0o644)
    except OSError as e:
        die(f"cannot open {LOCK}: {e}")
    if not stat.S_ISREG(os.fstat(fd).st_mode):
        die(f"{LOCK} is not a regular file")
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        die("another disc-flash or diskOS installer is running; only one may use the Disc at a time")
    return fd


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


def flash(img, what, timeout):
    check_udev()
    print(f"image: {img}\nsha256: {sha256(img)}")
    held = lock()
    wait_maskrom(timeout)
    confirm(f"Writes ONLY the rootfs partition (mtd2, 96 MiB) with {what}, after the Disc proves its kernel and "
            f"recovery are FiiO V{BASE_VER[0]}.{BASE_VER[1:]}. Takes about 18 minutes; do not unplug.")
    log = os.path.join(OS_DIR, "output", "flash.log")
    try:
        result = flasher.flash(img, BASE_VER, log_path=log)
    except flasher.FlashError as e:
        die(f"{e}\nlog: {log}" + ("" if getattr(e, "pre_launch", False) else f"\n{AFTER_FAILURE}"))
    finally:
        os.close(held)
    print(f"flashed: {result}\nTo boot: unplug USB, then hold Power about 6 s.")


def cmd_rootfs(args):
    img = os.path.abspath(args.image)
    check_image(img)
    check_stock()
    flash(img, "discfw", args.timeout)


def cmd_stock(args):
    check_stock()
    flash(STOCK_IMG, "FiiO's stock V2.57 rootfs", args.timeout)


def cmd_check(args):
    check_image(os.path.abspath(args.image))
    check_stock()
    check_udev()
    print("ok")


def main():
    p = argparse.ArgumentParser(prog="disc-flash", description="Flash discfw or the stock rootfs over USB (mask-ROM mode)")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("rootfs", help="write output/images/disc-rootfs-96M.bin to mtd2")
    r.add_argument("image", nargs="?", default=DEFAULT_IMG)
    r.add_argument("--timeout", type=int, default=300, help="seconds to wait for mask-ROM device")
    r.set_defaults(func=cmd_rootfs)
    s = sub.add_parser("stock", help="write FiiO's stock V2.57 rootfs (output/images/stock-rootfs-v257-96M.bin) to mtd2")
    s.add_argument("--timeout", type=int, default=300, help="seconds to wait for mask-ROM device")
    s.set_defaults(func=cmd_stock)
    c = sub.add_parser("check", help="check both images and the udev rule (no device needed)")
    c.add_argument("image", nargs="?", default=DEFAULT_IMG)
    c.set_defaults(func=cmd_check)
    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
