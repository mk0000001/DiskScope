#!/usr/bin/env python3
"""Measure visible GTK results on the synthetic benchmark.dscan fixture under Xvfb."""

import argparse
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import threading
import time

import pyatspi
from Xlib import X, XK, display
from Xlib.ext import xtest


POLL_SECONDS = 0.005
RSS_POLL_SECONDS = 0.01
TIMEOUT_SECONDS = 120


def walk_accessibles(node):
    if node is None:
        return
    yield node
    for child in node:
        yield from walk_accessibles(child)


def wait_for(check, description, process):
    deadline = time.monotonic() + TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"DiskScope exited with status {process.returncode}")
        result = check()
        if result:
            return result
        time.sleep(POLL_SECONDS)
    raise RuntimeError(f"Timed out waiting for {description}")


class Desktop:
    def __init__(self, app_pid):
        self.x = display.Display()
        self.root = self.x.screen().root
        self.registry = pyatspi.Registry.getDesktop(0)
        self.app_pid = app_pid

    def find_window(self):
        for window in self.root.query_tree().children:
            if window.get_wm_name() == "DiskScope" and window.get_attributes().map_state == X.IsViewable:
                return window
        return None

    def accessible_app(self):
        for app in self.registry:
            if app.get_process_id() == self.app_pid:
                return app
        return None

    def find_frame(self):
        app = self.accessible_app()
        frame = None
        if app is not None:
            for child in app:
                if child.getRoleName() == "dialog":
                    labels = [node.name for node in walk_accessibles(child)
                              if node.getRoleName() == "label"]
                    raise RuntimeError("GTK displayed an error dialog: " + "; ".join(labels))
                if child.name == "DiskScope":
                    frame = child
        return frame
        return None

    def pixels(self, region):
        return self.window.get_image(*region, X.ZPixmap, 0xffffffff).data

    def move(self, x, y):
        xtest.fake_input(self.x, X.MotionNotify, x=x, y=y)
        self.x.sync()

    def click(self, x, y):
        self.move(x, y)
        started = time.monotonic_ns()
        xtest.fake_input(self.x, X.ButtonPress, 1)
        xtest.fake_input(self.x, X.ButtonRelease, 1)
        self.x.flush()
        return started

    def key(self, keysym):
        keycode = self.x.keysym_to_keycode(keysym)
        if not keycode:
            raise RuntimeError(f"X11 keyboard has no mapping for {keysym}")
        xtest.fake_input(self.x, X.KeyPress, keycode)
        xtest.fake_input(self.x, X.KeyRelease, keycode)

    def type_text(self, text):
        for char in text:
            self.key(ord(char))
        self.x.flush()

    def replace_last_digit(self, digit):
        self.key(XK.XK_BackSpace)
        started = time.monotonic_ns()
        self.key(ord(digit))
        self.x.flush()
        return started

    def row(self, index):
        try:
            return self.rows[index]
        except IndexError:
            return None


def bounds(node):
    rectangle = node.queryComponent().getExtents(pyatspi.WINDOW_COORDS)
    return rectangle.x, rectangle.y, rectangle.width, rectangle.height


def center(node):
    x, y, width, height = bounds(node)
    return x + width // 2, y + height // 2


def first_name(desktop, index):
    row = desktop.row(index)
    try:
        cell = row[0] if row is not None else None
        return cell.name if cell is not None else None
    except IndexError:
        return None


def elapsed_ms(started):
    return (time.monotonic_ns() - started) / 1_000_000


def measure(desktop, process, start, matches, region, expected_pixels):
    accessibility_ms = paint_ms = first_change_ms = None
    old_pixels = desktop.pixels(region)
    if old_pixels == expected_pixels or matches():
        raise RuntimeError("Expected result was already present before the test input")
    started = start()

    def poll():
        nonlocal accessibility_ms, paint_ms, first_change_ms
        pixels = desktop.pixels(region)
        if first_change_ms is None and pixels != old_pixels:
            first_change_ms = elapsed_ms(started)
        if paint_ms is None and pixels == expected_pixels:
            paint_ms = elapsed_ms(started)
        if accessibility_ms is None and matches():
            accessibility_ms = elapsed_ms(started)
        return paint_ms is not None and accessibility_ms is not None

    wait_for(poll, "the expected accessible text and X11 pixels", process)
    return first_change_ms, accessibility_ms, paint_ms


def reference(desktop, process, matches, region, before):
    wait_for(matches, "the reference row", process)
    wait_for(lambda: desktop.pixels(region) != before, "the reference frame", process)
    time.sleep(0.12)
    return desktop.pixels(region)


def stats(label, samples):
    for metric, values in zip(("first_change", "atspi", "pixels"), zip(*samples)):
        ordered = sorted(values)
        n = len(values)
        print(f"{label}.{metric}.samples={n} p50_ms={ordered[math.ceil(n * 0.50) - 1]:.2f} "
              f"p95_ms={ordered[math.ceil(n * 0.95) - 1]:.2f} max_ms={ordered[-1]:.2f}", flush=True)


def diagnostics_smoke(desktop, frame, process, work):
    def locate(scope, name, *roles):
        return wait_for(lambda: next((node for node in walk_accessibles(scope) if node.name == name and
                                      (not roles or node.getRoleName() in roles)), None), name, process)

    def activate(widget):
        actions = widget.queryAction()
        if not actions.nActions or not actions.doAction(0):
            raise RuntimeError(f"Could not activate {widget.name}")

    root = "/diskscope-synthetic"
    locate(frame, root, "label")
    menu = locate(frame, "진단 내보내기", "menu button", "toggle button", "push button")
    app = desktop.accessible_app()
    reports = []
    for include_root in (False, True):
        desktop.click(*center(menu))
        checkbox = locate(app, "스캔 루트 경로 포함", "check box")
        if checkbox.getState().contains(pyatspi.STATE_CHECKED):
            raise RuntimeError("The diagnostics root checkbox should open unchecked")
        if include_root:
            if not checkbox.getState().contains(pyatspi.STATE_FOCUSED):
                if not checkbox.queryComponent().grabFocus():
                    raise RuntimeError("Could not focus the diagnostics root checkbox")
            desktop.key(XK.XK_space)
            desktop.x.flush()
            wait_for(lambda: checkbox.getState().contains(pyatspi.STATE_CHECKED), "root opt-in", process)
        activate(locate(app, "진단 JSON 저장", "push button"))
        dialog = locate(app, "진단 보고서 저장")
        entry = locate(dialog, "Name:", "text")
        output = Path(work, "with-root.json" if include_root else "redacted.json")
        if not entry.queryEditableText().setTextContents(str(output)):
            raise RuntimeError("Could not enter the report path in the save dialog")
        activate(locate(dialog, "Save", "push button"))
        success = locate(app, "진단 보고서 저장 완료", "label")
        reports.append(json.loads(output.read_text(encoding="utf-8")))
        message = success
        while message.getRoleName() != "dialog":
            message = message.parent
        activate(locate(message, "Close", "push button"))
        wait_for(lambda: not any(node.name == "진단 보고서 저장 완료" for node in walk_accessibles(app)),
                 "the completion dialog to close", process)

    desktop.click(*center(menu))
    if locate(app, "스캔 루트 경로 포함", "check box").getState().contains(pyatspi.STATE_CHECKED):
        raise RuntimeError("The prior report's root opt-in was still enabled on reopening")

    redacted, disclosed = reports
    if (redacted.get("report") != "diskscope-diagnostics" or redacted.pop("paths_redacted", None) is not True or
            "root" in redacted["scan"] or root in json.dumps(redacted)):
        raise RuntimeError("Default diagnostics output did not redact the scan root")
    if (disclosed.pop("paths_redacted", None) is not False or disclosed["scan"].pop("root", None) != root or
            disclosed != redacted):
        raise RuntimeError("Opt-in changed fields other than paths_redacted and scan.root")
    print("PASS GTK diagnostics save dialog · snapshot root redacted by default, disclosed only after opt-in",
          flush=True)


def interact(desktop, frame, process, args):
    tree = next(node for node in walk_accessibles(frame) if node.getRoleName() == "tree table")
    desktop.rows = tree[1]
    top = next(node for node in walk_accessibles(frame) if node.name == "큰 파일" and
               node.getRoleName() == "push button")
    tree_mode = next(node for node in walk_accessibles(frame) if node.name == "폴더 트리" and
                     node.getRoleName() == "push button")
    search = next(node for node in walk_accessibles(frame) if node.getRoleName() == "text")

    def root_loaded():
        row = desktop.row(0)
        return row is not None and any(unit in row.name for unit in ("MiB", "GiB"))

    def root_bounds():
        if root_loaded():
            x, y, width, height = bounds(desktop.row(0))
            if x >= 0 and y >= 0 and width >= 350 and height >= 20:
                return x, y, width, height
        return None

    x, y, width, height = wait_for(root_bounds, "the first allocated snapshot row", process)
    # Sample just the name text or aggregate text. GTK draws the row headers,
    # expander hover, and cursor outside these regions.
    root_pixel_region = x + width - 260, y + height // 2 - 7, 120, 14
    wait_for(lambda: len(set(desktop.pixels(root_pixel_region))) > 8,
             "the first painted snapshot row", process)
    print(f"open.first_root_ms={elapsed_ms(args.launched):.2f}", flush=True)
    root_expander = x + 19, y + height // 2
    root_return_pixels = x + 42, y + height + height // 2 - 7, 290, 14
    first_result_pixels = x + 11, y + height // 2 - 7, 320, 14
    directory = "directory-" + f"{2:022d}"
    first_file = "file-" + f"{args.first_file:027d}"

    def has_directory():
        return first_name(desktop, 1) == directory

    def has_top_file():
        return first_name(desktop, 0) == first_file

    empty_child_pixels = desktop.pixels(root_return_pixels)
    desktop.click(*root_expander)
    child_pixels = reference(desktop, process, has_directory, root_return_pixels, empty_child_pixels)
    expansion = []
    for _ in range(args.samples):
        time.sleep(0.25)
        desktop.click(*root_expander)
        wait_for(lambda: desktop.row(1) is None and desktop.pixels(root_return_pixels) == empty_child_pixels,
                 "the collapsed root", process)
        time.sleep(0.25)
        expansion.append(measure(desktop, process, lambda: desktop.click(*root_expander),
                                 has_directory, root_return_pixels, child_pixels))
    stats("expand_root", expansion)

    before = desktop.pixels(first_result_pixels)
    desktop.click(*center(top))
    top_pixels = reference(desktop, process, has_top_file, first_result_pixels, before)
    top_samples = []
    for _ in range(args.samples):
        desktop.click(*center(tree_mode))
        wait_for(lambda: root_loaded() and desktop.pixels(first_result_pixels) != top_pixels,
                 "the folder tree", process)
        top_samples.append(measure(desktop, process, lambda: desktop.click(*center(top)),
                                   has_top_file, first_result_pixels, top_pixels))
    stats("top_files", top_samples)

    desktop.click(*center(tree_mode))
    wait_for(root_loaded, "the folder tree", process)
    desktop.click(*center(search))
    targets = ["file-" + f"{args.first_file + offset:027d}" for offset in (2, 5)]
    filters = [lambda expected=name: first_name(desktop, 0) == expected for name in targets]
    filter_pixels = []
    for name, matches in zip(targets, filters):
        before = desktop.pixels(first_result_pixels)
        if filter_pixels:
            desktop.replace_last_digit(name[-1])
        else:
            desktop.type_text(name)
        filter_pixels.append(reference(desktop, process, matches, first_result_pixels, before))
    search_samples = []
    for i in range(args.samples):
        index = i % len(targets)
        search_samples.append(measure(desktop, process, lambda: desktop.replace_last_digit(targets[index][-1]),
                                      filters[index], first_result_pixels, filter_pixels[index]))
        print(f"search_progress={i + 1}/{args.samples}", flush=True)
    stats("path_filter", search_samples)


def watch_rss(pid, stop, peak):
    path = Path(f"/proc/{pid}/status")
    while not stop.is_set():
        try:
            for line in path.read_text().splitlines():
                if line.startswith("VmRSS:"):
                    peak[0] = max(peak[0], int(line.split()[1]))
                if line.startswith("VmHWM:"):
                    peak[1] = max(peak[1], int(line.split()[1]))
        except ProcessLookupError:
            return
        except FileNotFoundError:
            return
        stop.wait(RSS_POLL_SECONDS)


def run(args, work):
    runtime = Path(work, "runtime")
    runtime.mkdir(mode=0o700)
    environment = dict(os.environ, XDG_CONFIG_HOME=work + "/config", XDG_CACHE_HOME=work + "/cache",
                       XDG_DATA_HOME=work + "/data", XDG_RUNTIME_DIR=str(runtime), GDK_BACKEND="x11",
                       GDK_SCALE="1", GTK_A11Y="atspi", G_DEBUG="fatal-criticals")
    environment.pop("GSK_RENDERER", None)
    if args.diagnostics_only:
        environment.update(LC_ALL="C.UTF-8", GTK_USE_PORTAL="0")
    command = [str(args.binary)] + (["--cairo"] if args.renderer == "cairo" else []) + [str(args.snapshot)]
    with tempfile.TemporaryFile() as log:
        args.launched = time.monotonic_ns()
        app = subprocess.Popen(command, cwd=work, env=environment, stdout=log, stderr=log)
        stop = threading.Event()
        peak = [0, 0]
        watcher = threading.Thread(target=watch_rss, args=(app.pid, stop, peak), daemon=True)
        watcher.start()
        try:
            desktop = Desktop(app.pid)
            desktop.window = wait_for(desktop.find_window, "the GTK window", app)
            desktop.window.set_input_focus(X.RevertToParent, X.CurrentTime)
            desktop.x.sync()
            frame = wait_for(desktop.find_frame, "the GTK accessibility frame", app)
            if args.diagnostics_only:
                diagnostics_smoke(desktop, frame, app, work)
            else:
                interact(desktop, frame, app, args)
        except Exception:
            log.seek(0)
            print(log.read().decode(errors="replace"), flush=True)
            raise
        finally:
            stop.set()
            watcher.join()
            if app.returncode is None:
                app.terminate()
                _, status, usage = os.wait4(app.pid, 0)
                app.returncode = os.waitstatus_to_exitcode(status)
                if app.returncode != -signal.SIGTERM:
                    raise RuntimeError(f"DiskScope ended unexpectedly with status {app.returncode}")
                print(f"gui.getrusage_maxrss_kib={usage.ru_maxrss}", flush=True)
            print(f"gui.sampled_peak_rss_kib={peak[0]} gui.proc_high_water_kib={peak[1]}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--renderer", choices=("cairo", "default"), default="cairo")
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--small", action="store_true", help="use a benchmark --small fixture")
    parser.add_argument("--diagnostics-only", action="store_true", help="smoke-test GTK diagnostic saves")
    args = parser.parse_args()
    args.binary = args.binary.resolve(strict=True)
    args.snapshot = args.snapshot.resolve(strict=True)
    if args.samples < 1:
        parser.error("--samples must be positive")
    if "DISPLAY" not in os.environ or "DBUS_SESSION_BUS_ADDRESS" not in os.environ:
        parser.error("Run under dbus-run-session and xvfb-run on a private X11 display")
    args.first_file = 1002 if args.small else 100002
    with tempfile.TemporaryDirectory(prefix="diskscope-desktop-") as work:
        run(args, work)


if __name__ == "__main__":
    main()
