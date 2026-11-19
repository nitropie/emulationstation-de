# Native UI test scenarios

Runs the real `es-de` binary headless, drives it with injected SDL input, and asserts the
rendered scene and the persisted data. It runs in the es-de derivation check as the
`es-ui-tests` ctest and can be run by hand.

## How it runs

The frontend executes normally, with `ES_DE_UI_TEST_SCRIPT` pointing at a scenario file. A small
driver compiled into the binary then:

- pushes SDL keyboard, text and controller events into the normal event queue, so
  `InputManager::parseEvent` handles them exactly as it handles a physical device;
- reads the scene from the GUI component tree after each rendered frame;
- captures the frame with `glReadPixels` for hashes;
- fails the scenario, prints the reason and quits.

Without `ES_DE_UI_TEST_SCRIPT` the driver does nothing. The scenario language has no shell,
file-write or network primitive; it can only inject input, inspect the scene and quit.

The runner (`run_ui_tests.py`) stages a fresh `HOME` and `ESDE_APPDATA_DIR` from a fixture for
every run, starts the binary with the SDL `offscreen` video driver on Mesa `llvmpipe`, checks the
scenario result and the persisted gamelist, and runs each scenario twice to compare the asserted
scene text and persisted values.

## Running

Inside the es-de build directory:

```
ctest -R es-ui-tests --output-on-failure
```

Inside a nix build of the package, this is part of `checkPhase` together with `es-core-tests` and
`es-app-tests`.

To run a single scenario by hand, point the binary at a fixture and a scenario:

```
export ES_DE_UI_EGL_VENDOR_DIR=/run/opengl-driver/share/glvnd/egl_vendor.d
export ES_DE_UI_MESA_DRIVERS=/run/opengl-driver/lib/dri
export ES_DE_UI_LIBGLVND_DIR=/run/opengl-driver/lib
python3 es-app/tests/ui/run_ui_tests.py \
  --binary ./es-de \
  --fixtures es-app/tests/ui/fixtures \
  --scenarios es-app/tests/ui/scenarios \
  --work-dir /tmp/es-ui-tests \
  --scenario favourite
```

The runner fails, and does not skip, when no EGL vendor file or no software Mesa driver is found.

## Scenario language

One step per line, `#` starts a comment. Arguments that contain spaces are quoted; `\"` and `\\`
are the only escapes.

| Step | Meaning |
|---|---|
| `wait-frames N` | Wait `N` rendered frames. |
| `wait-view system-view\|gamelist N` | Wait up to `N` frames for that view. |
| `wait-text "TEXT" N` | Wait up to `N` frames until the scene shows `TEXT`. |
| `assert-text "TEXT"` | Fail unless the scene shows `TEXT`. |
| `assert-absent "TEXT"` | Fail if the scene shows `TEXT`. |
| `capture NAME` | Print the frame hashes and the scene text under `NAME`. |
| `key NAME down\|up` | Push an SDL keyboard event. Names: `escape`, `return`, `backspace`, `insert`, `delete`, `up`, `down`, `left`, `right`, `pageup`, `pagedown`, `home`, `end`, or any SDL key name. |
| `controller NAME down\|up` | Push an SDL controller event from an attached virtual controller. Names: `a`, `b`, `x`, `y`, `start`, `back`, `up`, `down`, `left`, `right`, `leftshoulder`, `rightshoulder`. |
| `text "TEXT"` | Push an SDL text input event. |
| `expect-exit` | Declare that the application is expected to exit on its own, for example through a dialog that quits. The runner then accepts a clean exit instead of a `PASS` line. |
| `quit` | Quit through an SDL quit event. |

Frame budgets are bounded (1 to 100000). The parser rejects unknown verbs, missing arguments, bad
frame budgets, an unterminated quote and an empty script; the parser unit tests are in
`es-app/tests/UiTestScriptTests.cpp`.

## Scene assertions

`capture` prints every non-empty `GuiComponent::getValue()` of the visible component tree with its
logical position and size:

```
[ui-test] capture menu-open text [674,376 687x90] MAIN MENU
```

`assert-text` and `assert-absent` match a substring of those lines, so a scenario asserts the
exact strings the application rendered, not pixels and not OCR. Scenario fixtures assert named,
non-empty data with exact values.

Positions are logical component positions and do not include the theme scaling transform, so use
them for context, not for pixel math. Use the frame hashes for a pixel-level statement and only on
scenes that do not animate.

## Fixtures

A fixture owns one scenario and is staged into the run directory:

```
fixtures/<scenario>/
  custom_systems/es_systems.xml
  gamelists/<system>/gamelist.xml
  roms/<system>/*.sfc
  settings/es_settings.xml
```

The fixture sets `SaveGamelistsMode` to `on exit` so the persisted result is asserted after a real
quit. The runner copies with `copyfile` and makes the gamelist writable, because fixture files in
the nix store are read-only.

`scenarios/<scenario>.json` declares the number of runs and the persisted values to check:

```json
{
  "runs": 2,
  "persisted": {
    "./F-Zero (USA).sfc": {"name": "F-Zero", "favorite": "false"},
    "./Super Mario World (USA).sfc": {"name": "Super Mario World", "favorite": "true"}
  }
}
```

A run fails if the process does not exit 0, does not print `[ui-test] PASS`, prints
`[ui-test] FAIL`, writes a different gamelist than declared, or if the two runs differ in scene
text or persisted values.

### Masking a value that cannot be deterministic

A few persisted values come from the clock rather than from input, for example `lastplayed`,
which the game-end path stamps with the wall clock. A declared file may carry a `mask` list: each
entry is a `pattern` (a regular expression) and a `replace` (the token that stands in for every
match). The rules are applied to the actual file and to the golden, and the masked text is what
the runner compares between the two runs, so both runs must agree after masking.

```json
{
  "files": [
    {
      "path": "appdata/profile-state/snes.json",
      "golden": "state-after-launch.json",
      "mask": [
        {
          "pattern": "\"lastplayed\": \"[0-9]{8}T[0-9]{6}\"",
          "replace": "\"lastplayed\": \"<TS>\""
        }
      ]
    }
  ]
}
```

A mask is bounded to the file that declares it. Its pattern may not be longer than 200
characters and may not be able to match the empty string; it must match at least once in both the
actual file and the golden; and it must not match again after the replacement. Any of those
failures stops the run with the reason, so a mask cannot quietly become a no-op. Masking is only
for values that cannot be produced deterministically; every other file stays a byte-for-byte
comparison.

## Profile scenarios

`fixtures/profiled/` is the profile-mode fixture: a three-avatar manifest with its images, a
catalog whose entries point into the run directory, and metadata files. The runner substitutes
the run directory for `@RUN@` in `catalog.json.in`, exports `NITROPIE_PROFILE_CATALOG` and
`NITROPIE_PROFILE_ID`, and declares the persisted metadata file and its backup as `files`.
Both are compared byte for byte against the goldens, so an unknown key in the input has to
survive and the atomic publisher has to leave the previous bytes in `profile.json.bak`.
`fixtures/noprofile/` runs without those variables and asserts that the `PROFILE` entry is
absent.

A fixture may also ship `profile-state/<system>.json`; the runner stages it into the profile's
appdata and the declared `files` goldens prove that loading the library leaves the personal state
and the shared gamelist byte-identical. Profile-mode scenarios assert what the overlay does: a
stored favourite sorts first and appears in the counters, a stored hidden game is absent, and a
shared gamelist whose stale personal tags or preference elements disagree with the state changes
nothing on screen.

When a store cannot be resolved (a corrupt or too-new state file), the frontend shows the reason
before it offers the library and holds off the start view. `profilerecover` presses the first
dialog button to recover from `.bak`; `profileunresolved` has no backup, so its only button quits
and the scenario uses `expect-exit`. `noprofile-state-ignored` plants a state file without the
profile environment and asserts that legacy mode ignores it.

A scenario can launch the frontend more than once over the same staged data. The expectations
JSON is the scenario; its scripts are named in `launch`, so `profilerestart-part1.txt` and
`profilerestart-part2.txt` belong to the `profilerestart` scenario:

```json
{
  "launch": ["profilerestart-part1.txt", "profilerestart-part2.txt"],
  "files": [
    { "path": "profile/profile.json", "golden": "profile-restart.json" },
    { "path": "profile/profile.json.bak", "golden": "profile-renamed.json" }
  ]
}
```

The profile is renamed through the popup's own key grid, driven by the controller, because the
language has no text-append primitive. The popup opens with the name field focused, so the
first Down selects the first key. The grid is a hardcoded unshifted US layout, and the
scenario paths depend on it:

| Key | Grid cells |
|---|---|
| `1` | row 0, column 0 (the selected key after the first Down) |
| `2` | row 0, column 1 |
| `3` | row 0, column 2 |
| SPACE | row 4, columns 2-9 |
| CLEAR | row 4, columns 10-11 |
| OK | row 1, column 12 |

The scenarios reach SPACE with two Right and four Down from `1`, reach CLEAR with one more
Right because SPACE spans columns 2-9, and type `2` after moving to row 0. Up from row 0
leaves the grid for the name field, where A accepts the edit. The scenarios assert the field
text with `assert-text` or `wait-text` before accepting, so a missed key fails on a named text
check with the scene dump instead of in the persisted file. The captures print the popup's
field text, which is how the grid path was established: the key that was focused is the one
whose character appears in the field after A.

## Known limits

- Mesa `llvmpipe` output is not pixel-identical to the Raspberry Pi GPU. Pixel assertions compare
  frames within a run or on static scenes; text assertions are portable.
- Animated views (system view, transitions) change every frame, so no whole-frame golden hashes
  are used there.
- Views change on wall-clock animations while the driver waits in frames. `wait-view` and
  `wait-text` are used instead of fixed frame counts where the view matters.
- Textlist entries, including game names in a textlist theme, are drawn by the list component and
  are not children of the GUI tree. Scenario assertions use the metadata panel, the theme's game
  and favourite counters, and the persisted gamelist instead.
- The driver is compiled into the shipped binary but is inert unless `ES_DE_UI_TEST_SCRIPT` is set;
  it prints a warning line when active.
- The harness needs an OpenGL ES context. It runs on the SDL offscreen driver with Mesa
  `llvmpipe` and EGL; a missing driver or EGL vendor file is a failure.
