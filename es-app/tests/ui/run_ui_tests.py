#!/usr/bin/env python3
#  SPDX-License-Identifier: MIT
#
#  ES-DE Frontend
#  run_ui_tests.py
#
#  Runs the native UI scenarios against the real frontend binary and checks the persisted
#  results. See README.md in this directory.

import argparse
import json
import os
import re
import select
import shutil
import stat
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

CAPTURE_RE = re.compile(
    r"^\[ui-test\] capture (\S+) frame \d+ \d+x\d+ "
    r"full=([0-9a-f]+) top=([0-9a-f]+) bottom=([0-9a-f]+)$"
)
TEXT_RE = re.compile(r"^\[ui-test\] capture (\S+) text (.*)$")


# The run directory of the run currently being checked, so fail() can print the frontend log.
CURRENT_RUN_DIR = None


def fail(message):
    print(f"[ui-runner] FAIL {message}")
    if CURRENT_RUN_DIR:
        dump_frontend_log(CURRENT_RUN_DIR)
    sys.exit(1)


def first_dir(candidates, what):
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


def base_environment(home, appdata, run_dir):
    env = dict(os.environ)
    env.update({
        "HOME": home,
        "ESDE_APPDATA_DIR": appdata,
        "SDL_VIDEODRIVER": "offscreen",
        "SDL_VIDEO_DRIVER": "offscreen",
        "SDL_VIDEO_EGL_DRIVER": os.path.join(libglvnd_dir(), "libEGL.so.1"),
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "GALLIUM_DRIVER": "llvmpipe",
        "LIBGL_DRIVERS_PATH": mesa_driver_dir(),
        "__EGL_VENDOR_LIBRARY_DIRS": egl_vendor_dir(),
        "LD_LIBRARY_PATH": libglvnd_dir(),
    })
    return env


def remove_tree(path):
    """Removes a run directory after making every entry writable.

    The fixtures are copied from the Nix store, and scenarios such as profileflushfail make parts
    of the staged tree read-only on purpose. A plain rmtree cannot descend into those directories,
    and ignoring the error would leave the next run of the scenario with a half-removed directory.
    """
    if not os.path.exists(path):
        return

    for root, directories, files in os.walk(path):
        try:
            os.chmod(root, 0o755)
        except OSError:
            pass
        for name in directories + files:
            try:
                os.chmod(os.path.join(root, name), 0o755)
            except OSError:
                pass

    shutil.rmtree(path)


def stage_profile_state(fixture_root, appdata):
    """Copies the fixture's personal state into the staged appdata, when it has one.

    The profile's state directory is created by the launcher in production; the UI fixture
    provides it, so the frontend never has to create it.
    """
    source = os.path.join(fixture_root, "profile-state")
    if not os.path.isdir(source):
        return
    target = os.path.join(appdata, "profile-state")
    shutil.copytree(source, target, dirs_exist_ok=True)
    # The staging directory inherits the read-only modes of the store, and the frontend has to be
    # able to write the state file there.
    os.chmod(target, 0o755)
    for name in os.listdir(target):
        os.chmod(os.path.join(target, name), 0o644)


def stage_gamelist_data(fixture_root, home, appdata):
    roms_root = os.path.join(home, "ROMs")
    for directory in (roms_root, os.path.join(appdata, "custom_systems"),
                      os.path.join(appdata, "gamelists"),
                      os.path.join(appdata, "settings")):
        os.makedirs(directory, exist_ok=True)

    shutil.copyfile(os.path.join(fixture_root, "custom_systems", "es_systems.xml"),
                    os.path.join(appdata, "custom_systems", "es_systems.xml"))
    shutil.copyfile(os.path.join(fixture_root, "settings", "es_settings.xml"),
                    os.path.join(appdata, "settings", "es_settings.xml"))
    # A fixture may ship more than one system. Every ROM tree keeps its structure and every
    # gamelist is copied and made writable, because the frontend has to be able to rewrite them
    # in legacy mode.
    for system in sorted(os.listdir(os.path.join(fixture_root, "roms"))):
        shutil.copytree(os.path.join(fixture_root, "roms", system),
                        os.path.join(roms_root, system), dirs_exist_ok=True)
    for system in sorted(os.listdir(os.path.join(fixture_root, "gamelists"))):
        source = os.path.join(fixture_root, "gamelists", system)
        target = os.path.join(appdata, "gamelists", system)
        shutil.copytree(source, target, dirs_exist_ok=True)
        for name in os.listdir(target):
            os.chmod(os.path.join(target, name), 0o644)

    stage_profile_state(fixture_root, appdata)


def stage_gamelist(fixture_root, run_dir):
    home = os.path.join(run_dir, "home")
    appdata = os.path.join(run_dir, "appdata")
    stage_gamelist_data(fixture_root, home, appdata)
    return home, appdata, {}


def stage_profile(fixture_root, run_dir, profile):
    home = os.path.join(run_dir, "home")
    appdata = os.path.join(run_dir, "appdata")
    profile_dir = os.path.join(run_dir, "profile")
    avatars_dir = os.path.join(run_dir, "avatars")
    for directory in (home, appdata, profile_dir, avatars_dir,
                      os.path.join(appdata, "settings")):
        os.makedirs(directory)

    stage_gamelist_data(fixture_root, home, appdata)

    for name in os.listdir(os.path.join(fixture_root, "avatars")):
        src = os.path.join(fixture_root, "avatars", name)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(avatars_dir, name))
    shutil.copyfile(os.path.join(fixture_root, "avatars.json"),
                    os.path.join(avatars_dir, "avatars.json"))

    metadata = profile.get("metadata", "profile.json")
    metadata_path = os.path.join(profile_dir, "profile.json")
    shutil.copyfile(os.path.join(fixture_root, metadata), metadata_path)
    os.chmod(metadata_path, 0o644)

    with open(os.path.join(fixture_root, profile.get("catalog", "catalog.json.in")),
              encoding="utf-8") as handle:
        catalog = handle.read().replace("@RUN@", run_dir)
    catalog_path = os.path.join(profile_dir, "catalog.json")
    with open(catalog_path, "w", encoding="utf-8") as handle:
        handle.write(catalog)

    env = {
        "NITROPIE_PROFILE_CATALOG": catalog_path,
        "NITROPIE_PROFILE_ID": profile["id"],
    }
    return home, appdata, env


def start_wifi_server(binary, mock, config_path):
    """Starts the fake wpa_supplicant service and waits until it owns the bus name."""
    if not os.access(binary, os.X_OK):
        fail(f"the Wi-Fi test server {binary} is not executable")
    if not os.path.isfile(mock):
        fail(f"missing Wi-Fi mock configuration {mock}")

    process = subprocess.Popen([binary, "--config", mock, "--config-path", config_path],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    deadline = time.time() + 15
    while time.time() < deadline:
        if process.poll() is not None:
            output = process.stdout.read() if process.stdout else ""
            fail(f"the Wi-Fi test server exited before becoming ready: {output.strip()}")
        ready, _, _ = select.select([process.stdout], [], [], 0.2)
        if not ready:
            continue
        line = process.stdout.readline()
        if "es-wifi-test-server ready" in line:
            return process

    process.kill()
    process.wait()
    fail("the Wi-Fi test server did not become ready")


def stop_wifi_server(process):
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def dump_frontend_log(run_dir):
    """Prints the tail of the frontend's own log, which is where launch problems are reported."""
    path = os.path.join(run_dir, "appdata", "logs", "es_log.txt")
    if not os.path.isfile(path):
        return
    print("[ui-runner] frontend log tail:", file=sys.stderr)
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle.readlines()[-40:]:
            print(f"[ui-runner]   {line.rstrip()}", file=sys.stderr)


def run_launch(binary, script, env, run_dir, timeout):
    environment = dict(env)
    environment["ES_DE_UI_TEST_SCRIPT"] = script
    command = [binary, "--no-splash", "--gamelist-only", "--debug", "--resolution", "1280",
               "720"]
    try:
        completed = subprocess.run(command, env=environment, cwd=run_dir, capture_output=True,
                                   text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        fail(f"the frontend did not finish within {timeout} seconds")

    print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)

    if completed.returncode != 0:
        fail(f"the frontend exited with {completed.returncode}")
    if "[ui-test] FAIL" in completed.stdout:
        # The driver's own failure line names the step and the reason; repeat it here so the
        # runner's generic failure is never the only thing in the log.
        for line in completed.stdout.splitlines():
            if line.startswith("[ui-test] FAIL"):
                print(f"[ui-runner] scenario {line}", file=sys.stderr)
        fail("the scenario reported a failure")
    # A scenario may declare that the application exits on its own, for example through a dialog
    # that quits; a clean exit stands in for the PASS line then.
    expected_exit = "[ui-test] expect-exit" in completed.stdout
    if "[ui-test] PASS" not in completed.stdout and not expected_exit:
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

    return captures


def apply_masks(text, masks, what):
    """Applies the masking rules of one declared file to a text.

    A mask is {"pattern": <regex>, "replace": <text>}. It must match at least once and must not
    match again after the replacement, so a mask can never silently become a no-op.
    """
    for mask in masks:
        pattern = mask["pattern"]
        replacement = mask["replace"]
        masked, count = re.subn(pattern, replacement, text)
        if count == 0:
            print(text)
            fail(f"{what}: the mask pattern {pattern!r} matched nothing")
        if re.search(pattern, masked):
            fail(f"{what}: the mask pattern {pattern!r} still matches after masking")
        text = masked
    return text


def parse_masks(entry):
    """Validates the optional `mask` list of a `files` entry and compiles its patterns."""
    if "mask" not in entry:
        return []
    masks = entry["mask"]
    if not isinstance(masks, list) or not masks:
        fail(f"{entry['path']}: mask must be a non-empty list")
    for mask in masks:
        if not isinstance(mask, dict) or set(mask) != {"pattern", "replace"}:
            fail(f"{entry['path']}: every mask needs exactly a pattern and a replace")
        if not isinstance(mask["pattern"], str) or not mask["pattern"]:
            fail(f"{entry['path']}: a mask pattern must be a non-empty string")
        if not isinstance(mask["replace"], str):
            fail(f"{entry['path']}: a mask replace must be a string")
        # Bounded: one regex per named file, of a sane length, and it may not match the empty
        # string, which would let it rewrite arbitrary text.
        if len(mask["pattern"]) > 200:
            fail(f"{entry['path']}: the mask pattern {mask['pattern']!r} is longer than 200 "
                 "characters")
        try:
            compiled = re.compile(mask["pattern"])
        except re.error as error:
            fail(f"{entry['path']}: the mask pattern {mask['pattern']!r} is not valid: {error}")
        if compiled.match(""):
            fail(f"{entry['path']}: the mask pattern {mask['pattern']!r} can match an empty "
                 "string")
    return masks


def check_gamelist(gamelist, expected):
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
    parser.add_argument("--wifi-server")
    parser.add_argument("--scenario")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    if not os.access(args.binary, os.X_OK):
        fail(f"the frontend binary {args.binary} is not executable")

    scenarios = ([args.scenario] if args.scenario
                 else sorted(os.path.splitext(name)[0]
                             for name in os.listdir(args.scenarios) if name.endswith(".json")))
    if not scenarios:
        fail(f"no scenarios found in {args.scenarios}")

    os.makedirs(args.work_dir, exist_ok=True)
    results = {}

    for name in scenarios:
        expectations_path = os.path.join(args.scenarios, f"{name}.json")
        if not os.path.isfile(expectations_path):
            fail(f"missing expectations {expectations_path}")

        with open(expectations_path, encoding="utf-8") as handle:
            expectations = json.load(handle)

        wifi = expectations.get("wifi", {})
        wifi_backend = expectations.get("wifiBackend")
        if wifi_backend == "required" and not args.wifi_server:
            print(f"[ui-runner] SKIP {name} (Wi-Fi backend absent)")
            continue
        if wifi_backend == "absent" and args.wifi_server:
            print(f"[ui-runner] SKIP {name} (Wi-Fi backend present)")
            continue

        profile = expectations.get("profile")
        if profile:
            fixture_root = os.path.join(args.fixtures, profile.get("fixture", name))
        else:
            fixture_root = os.path.join(args.fixtures, expectations.get("fixture", name))
        if not os.path.isdir(fixture_root):
            fail(f"missing fixture directory {fixture_root}")

        script_names = expectations.get("launch", [f"{name}.txt"])
        runs = expectations.get("runs", 2)
        if runs < 2:
            fail("a scenario has to run at least twice to prove determinism")

        observed = []
        for index in range(runs):
            run_dir = os.path.join(args.work_dir, f"{name}-run-{index}")
            remove_tree(run_dir)

            global CURRENT_RUN_DIR
            CURRENT_RUN_DIR = run_dir
            if profile:
                home, appdata, extra_env = stage_profile(fixture_root, run_dir, profile)
            else:
                home, appdata, extra_env = stage_gamelist(fixture_root, run_dir)

            env = base_environment(home, appdata, run_dir)
            env.update(extra_env)

            config_path = os.path.join(run_dir, wifi.get("config", "imperative.conf"))
            wifi_server = None
            if wifi_backend == "required":
                rfkill_path = os.path.join(run_dir, "rfkill")
                with open(rfkill_path, "w", encoding="utf-8"):
                    pass
                env["ES_DE_WIFI_DBUS_BUS"] = "session"
                env["ES_DE_WIFI_RFKILL_PATH"] = rfkill_path
                wifi_server = start_wifi_server(
                    args.wifi_server,
                    os.path.join(fixture_root, wifi.get("mock", "wifi-mock.json")),
                    config_path)

            launch_captures = []
            try:
                for script_name in script_names:
                    script = os.path.join(args.scenarios, script_name)
                    if not os.path.isfile(script):
                        fail(f"missing scenario {script}")
                    launch_captures.append(run_launch(args.binary, script, env, run_dir,
                                                      args.timeout))
            finally:
                stop_wifi_server(wifi_server)

            wifi_state = {}
            if wifi_backend == "required":
                log_path = os.path.join(appdata, "logs", "es_log.txt")
                if not os.path.isfile(log_path):
                    fail("the frontend did not write es_log.txt")
                with open(log_path, encoding="utf-8", errors="replace") as handle:
                    log_text = handle.read()
                for forbidden in wifi.get("logForbid", []):
                    if forbidden in log_text:
                        fail(f"es_log.txt contains {forbidden!r}")

                if os.path.isfile(config_path):
                    mode = stat.S_IMODE(os.stat(config_path).st_mode)
                    if mode != 0o600:
                        fail(f"{wifi.get('config', 'imperative.conf')} has mode {oct(mode)}")
                    with open(config_path, encoding="utf-8", errors="replace") as handle:
                        config_text = handle.read()
                    for forbidden in wifi.get("configForbid", []):
                        if forbidden in config_text:
                            fail(f"the persisted Wi-Fi configuration contains {forbidden!r}")
                    for required in wifi.get("configRequire", []):
                        if required not in config_text:
                            fail(f"the persisted Wi-Fi configuration does not contain {required!r}")
                    wifi_state["config"] = config_text
                elif wifi.get("expectConfig"):
                    fail(f"{wifi.get('config', 'imperative.conf')} was not written")

            settings_state = {}
            settings_expectations = expectations.get("settings", {})
            if settings_expectations:
                settings_path = os.path.join(appdata, "settings", "es_settings.xml")
                if not os.path.isfile(settings_path):
                    fail("the frontend did not write es_settings.xml")
                # es_settings.xml is a flat list of elements without a root, so it cannot go
                # through ElementTree. The named boolean is matched directly instead.
                with open(settings_path, encoding="utf-8", errors="replace") as handle:
                    settings_text = handle.read()
                for setting_name, expected_value in settings_expectations.items():
                    match = re.search(r'<bool\s+name="' + re.escape(setting_name) +
                                      r'"\s+value="([^"]*)"', settings_text)
                    if not match:
                        fail(f"es_settings.xml has no bool {setting_name}")
                    actual_value = match.group(1)
                    if actual_value != str(expected_value):
                        fail(f"es_settings.xml has {setting_name}={actual_value}, "
                             f"expected {expected_value}")
                    settings_state[setting_name] = actual_value

            files = {}
            for entry in expectations.get("files", []):
                path = os.path.join(run_dir, entry["path"])
                golden = os.path.join(fixture_root, entry["golden"])
                if not os.path.isfile(path):
                    fail(f"{entry['path']} was not written")
                with open(path, "rb") as handle:
                    actual = handle.read()
                with open(golden, "rb") as handle:
                    expected = handle.read()

                masks = parse_masks(entry)
                if masks:
                    # A masked file is compared as text, with the same rules applied to the actual
                    # file, the golden and the two runs, so the masked form is what is recorded.
                    actual_text = apply_masks(actual.decode("utf-8", "replace"), masks,
                                              f"{entry['path']} (actual)")
                    expected_text = apply_masks(expected.decode("utf-8", "replace"), masks,
                                                f"{entry['golden']} (golden)")
                    if actual_text != expected_text:
                        print(actual_text)
                        fail(f"{entry['path']} does not match {entry['golden']} after masking")
                    files[entry["path"]] = actual_text
                    continue

                if actual != expected:
                    print(actual.decode("utf-8", "replace"))
                    fail(f"{entry['path']} does not match {entry['golden']}")
                files[entry["path"]] = actual.decode("utf-8", "replace")

            gamelist_checks = {}
            if not profile and expectations.get("persisted"):
                gamelist = os.path.join(appdata, "gamelists", "snes", "gamelist.xml")
                gamelist_checks = check_gamelist(gamelist, expectations["persisted"])

            observed.append({"captures": launch_captures, "files": files,
                             "persisted": gamelist_checks, "wifi": wifi_state,
                             "settings": settings_state})
            print(f"[ui-runner] run {index + 1}/{runs} of {name} passed")

        first = observed[0]
        for index, run in enumerate(observed[1:], start=2):
            if len(run["captures"]) != len(first["captures"]):
                fail(f"{name}: run {index} launched a different number of times")
            for launch, (left, right) in enumerate(zip(first["captures"], run["captures"])):
                left_text = {scene: data["text"] for scene, data in left.items()}
                right_text = {scene: data["text"] for scene, data in right.items()}
                if left_text != right_text:
                    fail(f"{name}: run {index} launch {launch + 1} produced different scene text")
            if run["files"] != first["files"]:
                fail(f"{name}: run {index} persisted different files than run 1")
            if run["persisted"] != first["persisted"]:
                fail(f"{name}: run {index} persisted different values than run 1")
            if run["wifi"] != first["wifi"]:
                fail(f"{name}: run {index} persisted a different Wi-Fi configuration than run 1")
            if run["settings"] != first["settings"]:
                fail(f"{name}: run {index} persisted different settings than run 1")

        results[name] = {
            "runs": runs,
            "launches": len(first["captures"]),
            "scenes": sorted({scene for launch in first["captures"] for scene in launch}),
            "files": sorted(first["files"]),
        }
        print(f"[ui-runner] PASS {name} ({runs} runs, identical scene text and persisted files)")

    with open(os.path.join(args.work_dir, "results.json"), "w", encoding="utf-8") as handle:
        json.dump(results, handle, indent=2, sort_keys=True)


if __name__ == "__main__":
    main()
