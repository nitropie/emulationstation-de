#!/usr/bin/env python3
#  SPDX-License-Identifier: MIT
#
#  ES-DE Frontend
#  run_ui_tests.py
#
#  Runs the native UI scenarios against the real frontend binary and checks the persisted
#  results. See README.md in this directory.

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET

CAPTURE_RE = re.compile(
    r"^\[ui-test\] capture (\S+) frame \d+ \d+x\d+ "
    r"full=([0-9a-f]+) top=([0-9a-f]+) bottom=([0-9a-f]+)$"
)
TEXT_RE = re.compile(r"^\[ui-test\] capture (\S+) text (.*)$")


def fail(message):
    print(f"[ui-runner] FAIL {message}")
    sys.exit(1)


def first_existing(candidates, what):
    for candidate in candidates:
        if candidate and os.path.isdir(candidate) and os.listdir(candidate):
            return candidate
    fail(f"no {what} found; looked at {candidates}")


def egl_vendor_dir():
    candidates = [
        os.environ.get("ES_DE_UI_EGL_VENDOR_DIR"),
        "/run/opengl-driver/share/glvnd/egl_vendor.d",
        "/usr/share/glvnd/egl_vendor.d",
    ]
    for candidate in candidates:
        if candidate and os.path.isdir(candidate):
            if any(name.endswith(".json") for name in os.listdir(candidate)):
                return candidate
    fail("no EGL vendor json file found; the UI test cannot create an OpenGL ES context")


def mesa_driver_dir():
    candidates = [
        os.environ.get("ES_DE_UI_MESA_DRIVERS"),
        "/run/opengl-driver/lib/dri",
        "/usr/lib/dri",
        "/usr/lib/x86_64-linux-gnu/dri",
    ]
    for candidate in candidates:
        if candidate and os.path.isdir(candidate):
            if any(name.startswith("swrast") or name.startswith("kms_swrast")
                   for name in os.listdir(candidate)):
                return candidate
    fail("no software Mesa driver (swrast) found; the UI test needs llvmpipe")


def libglvnd_dir():
    candidates = [os.environ.get("ES_DE_UI_LIBGLVND_DIR")]
    for root in ("/run/opengl-driver/lib", "/usr/lib", "/usr/lib/x86_64-linux-gnu"):
        candidates.append(root)
    for candidate in candidates:
        if candidate and os.path.isfile(os.path.join(candidate, "libEGL.so.1")):
            return candidate
    fail("libEGL.so.1 not found; the UI test needs libglvnd")


def stage_fixture(fixture_root, run_dir):
    home = os.path.join(run_dir, "home")
    appdata = os.path.join(run_dir, "appdata")
    roms = os.path.join(home, "ROMs", "snes")
    for directory in (roms, os.path.join(appdata, "custom_systems"),
                      os.path.join(appdata, "gamelists", "snes"),
                      os.path.join(appdata, "settings")):
        os.makedirs(directory)

    # copyfile, not copy: the fixture files in the store are read-only and the frontend has to
    # rewrite the gamelist.
    shutil.copyfile(os.path.join(fixture_root, "custom_systems", "es_systems.xml"),
                    os.path.join(appdata, "custom_systems", "es_systems.xml"))
    shutil.copyfile(os.path.join(fixture_root, "settings", "es_settings.xml"),
                    os.path.join(appdata, "settings", "es_settings.xml"))
    gamelist = os.path.join(appdata, "gamelists", "snes", "gamelist.xml")
    shutil.copyfile(os.path.join(fixture_root, "gamelists", "snes", "gamelist.xml"), gamelist)
    os.chmod(gamelist, 0o644)
    for rom in glob.glob(os.path.join(fixture_root, "roms", "snes", "*")):
        shutil.copyfile(rom, os.path.join(roms, os.path.basename(rom)))

    return home, appdata


def run_scenario(binary, scenario, fixture_root, run_dir, timeout):
    home, appdata = stage_fixture(fixture_root, run_dir)
    gamelist = os.path.join(appdata, "gamelists", "snes", "gamelist.xml")

    env = dict(os.environ)
    env.update({
        "HOME": home,
        "ESDE_APPDATA_DIR": appdata,
        "ES_DE_UI_TEST_SCRIPT": scenario,
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_VIDEO_DRIVER": "offscreen",
        "SDL_VIDEO_EGL_DRIVER": os.path.join(libglvnd_dir(), "libEGL.so.1"),
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "GALLIUM_DRIVER": "llvmpipe",
        "LIBGL_DRIVERS_PATH": mesa_driver_dir(),
        "__EGL_VENDOR_LIBRARY_DIRS": egl_vendor_dir(),
        "LD_LIBRARY_PATH": libglvnd_dir(),
    })

    command = [binary, "--no-splash", "--gamelist-only", "--debug", "--resolution", "1280",
               "720"]
    try:
        completed = subprocess.run(command, env=env, cwd=run_dir, capture_output=True,
                                   text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"the frontend did not finish within {timeout} seconds")

    print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)

    if completed.returncode != 0:
        fail(f"the frontend exited with {completed.returncode}")

    if "[ui-test] FAIL" in completed.stdout:
        fail("the scenario reported a failure")
    if "[ui-test] PASS" not in completed.stdout:
        fail("the scenario did not report a pass")

    captures = {}
    for line in completed.stdout.splitlines():
        match = CAPTURE_RE.match(line)
        if match:
            captures[match.group(1)] = {"hashes": match.groups()[1:], "text": []}
            continue
        match = TEXT_RE.match(line)
        if match:
            captures.setdefault(match.group(1), {"hashes": None, "text": []})["text"].append(
                match.group(2))

    if not captures:
        fail("the scenario captured no scene")

    return captures, gamelist


def check_persisted(gamelist, expected):
    tree = ET.parse(gamelist)
    root = tree.getroot()
    games = {}
    for game in root.findall("game"):
        path = game.findtext("path")
        if path is None:
            fail("a persisted game has no path")
        games[path] = game

    for path, fields in expected.items():
        if path not in games:
            fail(f"the persisted gamelist has no entry for {path}")
        game = games[path]
        for field, value in fields.items():
            actual = game.findtext(field)
            if actual != value:
                with open(gamelist, encoding="utf-8") as handle:
                    print(handle.read())
                fail(f"{path}: expected {field}={value}, found {actual}")

    return {path: {field: game.findtext(field) for field in expected[path]}
            for path, game in games.items() if path in expected}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--fixtures", required=True)
    parser.add_argument("--scenarios", required=True)
    parser.add_argument("--work-dir", required=True)
    parser.add_argument("--scenario")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    if not os.access(args.binary, os.X_OK):
        fail(f"the frontend binary {args.binary} is not executable")

    scenarios = ([args.scenario] if args.scenario
                 else sorted(os.path.splitext(name)[0]
                             for name in os.listdir(args.scenarios) if name.endswith(".txt")))
    if not scenarios:
        fail(f"no scenarios found in {args.scenarios}")

    os.makedirs(args.work_dir, exist_ok=True)
    results = {}

    for name in scenarios:
        scenario = os.path.join(args.scenarios, f"{name}.txt")
        expectations_path = os.path.join(args.scenarios, f"{name}.json")
        if not os.path.isfile(scenario):
            fail(f"missing scenario {scenario}")
        if not os.path.isfile(expectations_path):
            fail(f"missing expectations {expectations_path}")

        fixture_root = os.path.join(args.fixtures, name)
        if not os.path.isdir(fixture_root):
            fail(f"missing fixture directory {fixture_root}")

        with open(expectations_path, encoding="utf-8") as handle:
            expectations = json.load(handle)

        runs = expectations.get("runs", 2)
        if runs < 2:
            fail("a scenario has to run at least twice to prove determinism")

        observed = []
        for index in range(runs):
            run_dir = os.path.join(args.work_dir, f"{name}-run-{index}")
            shutil.rmtree(run_dir, ignore_errors=True)
            captures, gamelist = run_scenario(args.binary, scenario, fixture_root, run_dir,
                                              args.timeout)
            persisted = check_persisted(gamelist, expectations.get("persisted", {}))
            observed.append({"captures": captures, "persisted": persisted})
            print(f"[ui-runner] run {index + 1}/{runs} of {name} passed")

        first = {scene: data["text"] for scene, data in observed[0]["captures"].items()}
        for index, run in enumerate(observed[1:], start=2):
            text = {scene: data["text"] for scene, data in run["captures"].items()}
            if text != first:
                fail(f"{name}: run {index} produced different scene text than run 1")
            if run["persisted"] != observed[0]["persisted"]:
                fail(f"{name}: run {index} persisted different values than run 1")

        results[name] = {
            "runs": runs,
            "scenes": sorted(first),
            "text-counts": {scene: len(lines) for scene, lines in first.items()},
            "persisted": observed[0]["persisted"],
        }
        print(f"[ui-runner] PASS {name} ({runs} runs, identical scene text and persisted values)")

    with open(os.path.join(args.work_dir, "results.json"), "w", encoding="utf-8") as handle:
        json.dump(results, handle, indent=2, sort_keys=True)


if __name__ == "__main__":
    main()
