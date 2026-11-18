"""Capture QEMU RAMFB after the native NT test has finished drawing."""
import json
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

READY = b"PASS: display scanout final frame ready"
MOUSE_READY = b"READY: native USER waiting for VirtIO mouse"
KEYBOARD_READY = b"READY: native USER waiting for VirtIO keyboard"
EDIT_CLICK = b"READY: native edit window waiting for focus click"
EDIT_INITIAL = b"READY: native edit window waiting for initial text"
EDIT_KEYS = b"READY: native edit window waiting for editing keys"
EDIT_PAINTED = b"READY: native edit window painted and waiting for Return"
EDIT_PASS = b"PASS: native TranslateMessage queues WM_CHAR, USER32 EDIT reads cB and text extent is 16x8"


def qmp_command(channel, name, arguments=None):
    request = {"execute": name}
    if arguments:
        request["arguments"] = arguments
    channel.write(json.dumps(request).encode() + b"\n")
    channel.flush()
    while line := channel.readline():
        reply = json.loads(line)
        if "return" in reply:
            return reply["return"]
        if "error" in reply:
            raise RuntimeError(f"QMP {name} failed: {reply['error']}")
    raise RuntimeError(f"QMP closed before {name} completed")


def ppm(path):
    data = path.read_bytes()
    header = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\r?\n", data)
    if not header:
        raise RuntimeError(f"Invalid PPM from {path}")
    width, height = map(int, header.groups())
    pixels = data[header.end():]
    if len(pixels) != width * height * 3:
        raise RuntimeError(f"PPM size differs from geometry in {path}")
    return width, height, pixels


def verify_capture(target, reference, log):
    evidence = re.search(rb"Scanout evidence width=(0x[0-9a-f]+) height=(0x[0-9a-f]+)"
                         rb" stride=(0x[0-9a-f]+) format=(0x[0-9a-f]+) hdc=(0x[0-9a-f]+)", log)
    markers = (b"PASS: original XP clock setup initializes native reciprocal timer buckets",
               b"PASS: ARM64 EL0 reads XP shared tick count and converts it to milliseconds",
               b"PASS: live HWND overlap and client clipping match all 224 framebuffer pixels",
               b"PASS: native CREATESTRUCT callbacks, rejection, nested replacement and handle rundown",
               b"PASS: native per-queue focus, ordered callbacks and reentrant HWND changes",
               b"PASS: native child HWND ancestry, clipped Z order, descendant messages and cascade rundown",
               b"PASS: native synchronous Unicode window text, selection replacement and bounded callback output",
               b"PASS: native USER32 registers EDIT and keeps distinct single and multiline control state",
               b"PASS: native WM_PAINT, disjoint update clipping, nested erase and EndPaint rundown",
               b"PASS: native GetMessage blocks, cross-thread posts wake and dispatch, WM_QUIT drains",
               b"PASS: VirtIO keyboard and mouse IRQs wake native USER, dispatch and paint", EDIT_PASS)
    if not evidence or any(marker not in log for marker in markers):
        raise RuntimeError("The guest did not verify the native desktop and VirtIO input")
    width, height, stride, pixel_format, dc = (int(value, 16) for value in evidence.groups())
    captured_width, captured_height, actual = ppm(target)
    if (captured_width, captured_height) != (width, height) or stride < width or pixel_format not in (0, 1) or not dc:
        raise RuntimeError("QMP screenshot geometry differs from the GOP handoff")
    tile_width, tile_height, tile = ppm(reference)
    if (tile_width, tile_height) != (16, 12) or width < 192 or height < 152:
        raise RuntimeError("Scanout mode cannot show the full test pattern")
    expected = bytearray(width * height * 3)
    for y in range(120):
        row = tile[(y % tile_height) * 48:(y % tile_height + 1) * 48] * 10
        start = ((32 + y) * width + 32) * 3
        expected[start:start + len(row)] = row
    if expected != actual:
        offset = next(at for at, pair in enumerate(zip(expected, actual)) if pair[0] != pair[1])
        x, y = (offset // 3) % width, (offset // 3) // width
        raise RuntimeError(f"QMP scanout pixel ({x}, {y}) differs from the guest-rendered bitmap")


def wait_for_marker(process, console, marker, deadline):
    while True:
        log = console.read_bytes()
        if b"FAIL:" in log or process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"QEMU did not reach {marker.decode()}")
        if marker in log:
            return log
        if READY in log:
            raise RuntimeError(f"QEMU finished before {marker.decode()}")
        time.sleep(0.02)


def send_input(channel, events):
    qmp_command(channel, "input-send-event", {"events": events})


def key(channel, code, down):
    send_input(channel, [{"type": "key", "data": {"key": {"type": "qcode", "data": code}, "down": down}}])


def tap(channel, code):
    key(channel, code, True)
    key(channel, code, False)


def verify_edit_frame(target):
    width, height, pixels = ppm(target)
    if width < 369 or height < 99:
        raise RuntimeError("RAMFB cannot display the edit window")
    c = (0x00, 0x00, 0x1e, 0x33, 0x03, 0x33, 0x1e, 0x00)
    b = (0x3f, 0x66, 0x66, 0x3e, 0x66, 0x66, 0x3f, 0x00)
    for y in range(-1, 19):
        for x in range(-1, 65):
            inside = 0 <= x < 64 and 0 <= y < 18
            glyph = 2 <= y < 10 and (2 <= x < 10 and c[y - 2] & (1 << (x - 2))
                                     or 10 <= x < 18 and b[y - 2] & (1 << (x - 10)))
            caret = x == 10 and y in (12, 13, 14)
            value = 255 if inside and not (glyph or caret) else 0
            offset = ((80 + y) * width + 304 + x) * 3
            if pixels[offset:offset + 3] != bytes((value, value, value)):
                raise RuntimeError(f"QMP edit window and clip pixel ({x}, {y}) differs")
    print("PASS: QMP RAMFB edit glyphs, clipped caret and window bounds match")


def capture(target, reference, command):
    target.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="xp-scanout-", dir="/tmp") as work:
        endpoint = Path(work) / "qmp.sock"
        console = Path(work) / "serial.log"
        devices = ["-device", "ramfb", "-global", "virtio-mmio.force-legacy=false",
                   "-device", "virtio-keyboard-device", "-device", "virtio-mouse-device"]
        with console.open("wb") as stream:
            process = subprocess.Popen([*command, *devices, "-qmp", f"unix:{endpoint},server=on,wait=off"],
                                       stdout=stream, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 45
            wait_for_marker(process, console, MOUSE_READY, deadline)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(5)
                client.connect(str(endpoint))
                with client.makefile("rwb") as channel:
                    if "QMP" not in json.loads(channel.readline()):
                        raise RuntimeError("QEMU did not supply a QMP greeting")
                    qmp_command(channel, "qmp_capabilities")
                    send_input(channel, [{"type": "rel", "data": {"axis": axis, "value": offset}}
                                         for axis, offset in (("x", 260), ("y", 68))])
                    for down in (True, False):
                        send_input(channel, [{"type": "btn", "data": {"button": "left", "down": down}}])
                    wait_for_marker(process, console, KEYBOARD_READY, deadline)
                    tap(channel, "a")
                    wait_for_marker(process, console, EDIT_CLICK, deadline)
                    send_input(channel, [{"type": "rel", "data": {"axis": axis, "value": offset}}
                                         for axis, offset in (("x", 50), ("y", 20))])
                    for down in (True, False):
                        send_input(channel, [{"type": "btn", "data": {"button": "left", "down": down}}])
                    wait_for_marker(process, console, EDIT_INITIAL, deadline)
                    tap(channel, "a")
                    key(channel, "shift", True)
                    tap(channel, "b")
                    key(channel, "shift", False)
                    wait_for_marker(process, console, EDIT_KEYS, deadline)
                    for code in ("left", "backspace", "c"):
                        tap(channel, code)
                    wait_for_marker(process, console, EDIT_PAINTED, deadline)
                    edit_frame = Path(work) / "edit.ppm"
                    qmp_command(channel, "screendump", {"filename": str(edit_frame)})
                    verify_edit_frame(edit_frame)
                    tap(channel, "ret")
                    log = wait_for_marker(process, console, READY, deadline)
                    qmp_command(channel, "screendump", {"filename": str(target.resolve())})
                    verify_capture(target, reference, log)
                    qmp_command(channel, "quit")
            if process.wait(timeout=5):
                raise RuntimeError("QEMU failed to exit after capture")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            print(console.read_text(errors="replace"), end="")
    print("PASS: QMP RAMFB screenshot matches every expected NT GDI scanout pixel")


if __name__ == "__main__":
    capture(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:])
