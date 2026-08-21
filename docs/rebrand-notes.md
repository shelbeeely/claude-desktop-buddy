# Rebrand notes: "Free Ink Claude Buddy"

Text/branding-only unit. Renamed the project from "claude-desktop-buddy" /
"Claude Desktop Buddy" to **Free Ink Claude Buddy** throughout in-repo docs
and the flasher page, and reframed the README's opening as a FreeInk-SDK-
first project that happens to speak Claude's BLE desk-pet protocol (per the
user's framing: "the free ink sdk is core to this"). No source files,
`platformio.ini`, or the `freeink-sdk` submodule were touched, per this
unit's scope.

## Files changed

- `README.md` — title (`# claude-desktop-buddy` → `# Free Ink Claude Buddy`)
  and the intro paragraph, reframed to lead with FreeInk SDK as the
  foundation and Claude's BLE protocol as what it happens to speak. Kept
  the `REFERENCE.md` pointer and the desk-pet example paragraph as-is.
  No other name-drops of the old project name existed elsewhere in this
  file (verified by grep after editing — see below).
- `site/index.html` — `<title>` and `<h1>` (`Claude Desktop Buddy — Xteink`
  → `Free Ink Claude Buddy — Xteink`). Left the two
  `github.com/shelbeeely/claude-desktop-buddy` links untouched — that's
  the real repo URL/slug, not a display name, and renaming the GitHub repo
  itself is out of scope for a text-only unit.
- `site/manifest.template.json` — ESP Web Tools manifest `"name"` field
  (`Claude Desktop Buddy — Xteink` → `Free Ink Claude Buddy — Xteink`).
  This flows into the generated `site/manifest.json` on the next CI build.

## Files checked, no change needed

- `REFERENCE.md` — titled "Hardware Buddy BLE Protocol"; never names the
  project itself, only the protocol. No change.
- `CONTRIBUTING.md` — refers to "this repo" / "this firmware" generically,
  never by name. No change.
- `LICENSE` — copyright line and bufo-asset attribution; doesn't name the
  project. No change.
- `characters/bufo/README.md` — doesn't name the project. No change.
- `.github/workflows/firmware.yml` — workflow `name:` is "Build & publish
  Xteink firmware" and doesn't reference the project's display name
  anywhere (checked comments too). No change.
- `package.json` — doesn't exist in this repo (PlatformIO/C++ firmware
  project, confirmed via `find`). N/A.
- `docs/freeinkapp-migration.md`, `docs/board-notes/*.md` — don't exist
  yet at the time this unit ran (checked via file listing before writing
  README's intro, per the task's instruction not to invent references to
  files that aren't there). Nothing to cross-reference.

## Left alone deliberately: `src/main.cpp` (do not touch, per scope)

Two other parallel units are actively editing `src/main.cpp`; per this
unit's instructions it was not touched. Project-name strings found there,
for whoever does the final consolidation pass:

- **Line 1** (top-of-file comment):
  `// claude-desktop-buddy — Xteink firmware (X4, X3, X4 Pro).`
  → should become something like
  `// Free Ink Claude Buddy — Xteink firmware (X4, X3, X4 Pro).`

- **Lines 771–772** (credits/info screen, drawn on-device — "info" page 6,
  "credits" per README's "Menu system"):
  ```cpp
  ln(Color::Black, "github.com/anthropics");
  ln(Color::Black, "/claude-desktop-buddy");
  ```
  This renders the repo path on the device's own credits screen. The repo
  slug itself (`shelbeeely/claude-desktop-buddy`) is the real GitHub path
  and per this unit's scope should not be changed just for branding — but
  note the owner shown here is `anthropics`, not `shelbeeely` (the actual
  repo owner per `site/index.html`'s links). Whoever touches `main.cpp`
  next should reconcile this against the real repo URL, and decide
  whether the credits line should also show the "Free Ink Claude Buddy"
  display name alongside/instead of the raw path.

Two other lines mention "desktop-buddy" but are **not** rebrand targets —
they're historical references to a *different, earlier* M5StickCPlus-based
project also called "desktop-buddy" that this firmware's design borrows
patterns from (matches README's own "the earlier M5StickCPlus-based
desktop-buddy" phrasing throughout):

- `src/main.cpp:194` — `// State machine — identical derivation to the
  earlier desktop-buddy...`
- `src/main.cpp:514` — `// Display modes, menu, settings, reset — mirrors
  the earlier desktop-buddy...`
- `src/stats.h:222` — `// Trimmed from the earlier desktop-buddy
  generation's Settings struct: sound/...`

These describe lineage from a prior, separate codebase, not this repo's
own name, so they were left as-is and should stay that way even after the
`main.cpp` consolidation above.

## Verification

```
grep -rn "claude-desktop-buddy" --include="*.md" --include="*.html" \
  --include="*.yml" --include="*.json" . | grep -v '^\./freeink-sdk\|^\./\.git'
```

Only remaining hits after this unit's changes are the two real GitHub URLs
in `site/index.html` (`github.com/shelbeeely/claude-desktop-buddy`), which
are the actual repo slug and intentionally unchanged.
