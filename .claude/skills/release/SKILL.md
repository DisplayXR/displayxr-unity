---
name: release
description: Create a tagged release of the DisplayXR Unity plugin, monitor CI build, and verify the GitHub release and UPM branch are updated. Use /release v1.0.0 for explicit version, or /release patch|minor|major for auto-bump.
allowed-tools: Read, Grep, Glob, Bash, Agent, Edit, Write
---

# Release Skill — DisplayXR Unity Plugin

Creates a tagged release of the Unity plugin, monitors `build-native.yml`, and verifies the GitHub Release, `upm` branch, and `upm/{version}` tag are all in place.

## Architecture

```
/release v0.8.0
  │
  ├─ Pre-flight checks (clean tree, on main, version valid)
  ├─ Bump version in package.json
  ├─ Add CHANGELOG.md entry
  ├─ Commit + create tag + push
  ├─ Monitor build-native.yml
  │    ├─ build-windows: Windows x64 native plugin
  │    ├─ build-macos:   macOS Universal native plugin
  │    └─ release:       UPM tarball + GitHub Release + upm branch + upm/{version} tag
  ├─ Verify GitHub Release exists with .tgz asset
  ├─ Verify upm branch updated
  └─ Report
```

## CRITICAL: Launch Subagent

**You MUST use the Agent tool with `subagent_type="general-purpose"` to execute this workflow.**

### Parsing the Version Argument

Parse `[ARGUMENTS]` to determine version:

1. If argument matches `vN.N.N` → use as explicit version
2. If argument is `patch` → read current version from `package.json`, bump patch (0.7.0 → 0.7.1)
3. If argument is `minor` → bump minor (0.7.0 → 0.8.0)
4. If argument is `major` → bump major (0.7.0 → 1.0.0)
5. If no argument → ask user what version

The git tag uses the `v` prefix (`v0.8.0`); the `package.json` version field does NOT (`0.8.0`).

### Subagent Prompt Template

Replace `[VERSION]` with the resolved version (e.g., `v0.8.0`) and `[VERSION_NUMBER]` with the version without the `v` prefix (e.g., `0.8.0`):

```
Execute the DisplayXR Unity plugin release workflow for version [VERSION].

## Configuration
- Repo: DisplayXR/displayxr-unity
- Workflow: build-native.yml
- Version source of truth: package.json "version" field
- Git tag format: v{major}.{minor}.{patch}
- UPM branch: upm (orphan, force-pushed each release with native binaries)
- UPM tag format: upm/v{major}.{minor}.{patch}

---

## PHASE 1: PRE-FLIGHT CHECKS

### Step 1.1: Verify clean state
Run: `git status --short`
- If dirty, report and STOP: "Working tree is not clean. Commit or stash changes first."

### Step 1.2: Verify on main branch
Run: `git branch --show-current`
- If not `main`, report and STOP: "Must be on main branch to release."

### Step 1.3: Verify version doesn't already exist
Run: `git tag -l "[VERSION]"`
- If tag exists, report and STOP: "Tag [VERSION] already exists."

### Step 1.4: Verify package.json current version
Read `package.json`. Extract `"version": "X.Y.Z"`.
- Confirm the resolved [VERSION_NUMBER] is greater than current.

### Step 1.5: Get previous tag for release notes
Run: `git tag --sort=-v:refname | grep '^v' | head -1`
Store as PREV_TAG.

---

## PHASE 2: UPDATE VERSION AND CHANGELOG

### Step 2.1: Bump package.json version
Use Edit tool to change `"version": "X.Y.Z"` → `"version": "[VERSION_NUMBER]"`.

### Step 2.2: Add CHANGELOG.md entry
Read CHANGELOG.md. Find the top header (after the file title).
Generate commit summary since PREV_TAG:
```bash
git log PREV_TAG..HEAD --oneline --no-merges
```
Group commits by prefix (feat/fix/docs/refactor/ci) and prepend a new section to CHANGELOG.md:
```
## [[VERSION_NUMBER]] - YYYY-MM-DD

### Added
- ...

### Fixed
- ...

### Changed
- ...
```
Use today's date.

If unsure about grouping, just list all commits under "### Changed".

### Step 2.3: Commit version bump
```bash
git add package.json CHANGELOG.md
git commit -m "$(cat <<'EOF'
Release [VERSION]
EOF
)"
```
Store the commit SHA: `git rev-parse HEAD`

### Step 2.4: Create tag and push
```bash
git tag [VERSION]
git push origin main
git push origin [VERSION]
```

---

## PHASE 3: MONITOR BUILD

### Step 3.1: Wait for build to register
Run: `sleep 15`

### Step 3.2: Find the build run
```bash
gh run list --workflow build-native.yml --limit 10 --json databaseId,status,headSha,displayTitle,event
```
Find the run matching your commit SHA (from Step 2.3) with event=push.
Retry up to 6 times with 10s waits.

### Step 3.3: Watch build
Run: `gh run watch RUN_ID --interval 15` (timeout 600000ms)

### Step 3.4: Check result
Run: `gh run view RUN_ID --json status,conclusion`
- If success: continue to Phase 3.5
- If failure: Go to PHASE 5 (Rollback)

---

## PHASE 3.5: CODE-SIGN THE WINDOWS NATIVE PLUGIN (capability-gated)

`displayxr_unity.dll` is the native plugin that loads into the Unity
player, so Smart App Control blocks it when unsigned. CI builds it on a
GitHub-hosted runner (UNSIGNED — the EV signing key lives on a self-hosted
build box, never in cloud CI, and this public repo names no signer). So a
signed release is produced by sending the CI-built DLL to a **remote
signing provider** and re-injecting the signed DLL into both distribution
channels.

The provider is named by the **`DXR_SIGN_REPO` local env var** (unset here →
unsigned; the public repo hardcodes no provider path). On the dev/release box
it's typically sourced from the runtime repo's `.env.local`
(`export DXR_SIGN_REPO=<owner/repo>`). The provider exposes a **`sign-artifact`**
folder hook: zip a folder of PEs → it signs every `.dll`/`.exe` on its
self-hosted EV box → returns a `signed` artifact. This flow is **OS-agnostic**
(runs from macOS / Linux / any Windows — no local cert, no `SIGN_CMD`), the same
primitive `/dxr-release` and `/installer-release` use. Contract: the runtime
repo's `docs/specs/runtime/release-signing.md`.

Signing does **not** rebuild — the CI-built DLL is signed in place inside the
`.tgz` release asset AND the `upm` orphan branch (git-URL installs read the DLL
from there). It **never gates publishing**: any failure leaves the unsigned CI
release and is flagged in the report. Re-running this against an already-released
version signs it in place — safe and idempotent — so a version cut without a
signer can be signed later by simply re-running `/release [VERSION]`.

### Step 3.5.1: Capability check
```bash
SIGN_REPO="${DXR_SIGN_REPO}"   # local env only; unset -> unsigned (public repo names no provider)
if [ -n "$SIGN_REPO" ] && gh workflow view sign-artifact -R "$SIGN_REPO" >/dev/null 2>&1; then
  SIGNED=yes
else
  echo "⚠  SIGNING SKIPPED — DXR_SIGN_REPO unset in the env, or the provider is unreachable."
  echo "   The release DLL stays UNSIGNED. Set DXR_SIGN_REPO to a repo implementing"
  echo "   'sign-artifact' (e.g. source the runtime repo's .env.local), then re-run"
  echo "   /release [VERSION] — it detects the existing release and signs it in place."
  SIGNED=no   # continue — do not fail
fi
```

### Step 3.5.2: Sign the DLL via `sign-artifact`, re-inject into BOTH channels (if SIGNED=yes)
Fold **only** the DLL into a sign folder, hand it to the provider, then re-inject
the signed copy into the `.tgz` asset and the `upm` branch. Cleans up the temp
signing release on the provider regardless of outcome.
```bash
if [ "$SIGNED" = yes ]; then
  TGZ="com.displayxr.unity-[VERSION_NUMBER].tgz"
  DLL_REL="Runtime/Plugins/Windows/x64/displayxr_unity.dll"
  D=$(mktemp -d)

  # Pull the just-released .tgz and extract the CI-built (unsigned) DLL.
  gh release download [VERSION] -R DisplayXR/displayxr-unity -p "$TGZ" -D "$D"
  mkdir -p "$D/x"; tar xzf "$D/$TGZ" -C "$D/x"
  PKGDIR=$(ls -d "$D"/x/com.displayxr.unity-* 2>/dev/null | head -1); [ -z "$PKGDIR" ] && PKGDIR="$D/x/package"
  DLL="$PKGDIR/$DLL_REL"

  SIGNED_DLL=""
  if [ ! -f "$DLL" ]; then
    echo "⚠ $DLL_REL not found in $TGZ — ships unsigned."; SIGNED=no
  else
    mkdir -p "$D/in"; cp "$DLL" "$D/in/"                       # fold ONLY the DLL into a sign folder
    # portable zip: git-bash on Windows has no `zip` — fall back to PowerShell.
    if command -v zip >/dev/null; then ( cd "$D/in" && zip -qr "$D/unsigned.zip" . )
    else powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$D/in")\*' -DestinationPath '$(cygpath -w "$D/unsigned.zip")' -Force"; fi

    # NOTE: name this SIGN_TAG, never TMP. `TMP` is an exported env var on
    # git-bash/Windows (Go's os.TempDir() reads %TMP%), so assigning to it
    # repoints every child process's temp dir at a relative path — and
    # `gh run download` below then dies with "error initializing temporary
    # file: open <cwd>\sign-unity-...\gh-artifact.zip: The system cannot find
    # the path specified", leaving the release silently unsigned (and littering
    # an empty sign-unity-* dir in the cwd). Windows-only: macOS/Linux Go reads
    # TMPDIR, so this never repros on the mac box.
    SIGN_TAG="sign-unity-$(date +%s)-$$"
    gh release create "$SIGN_TAG" -R "$SIGN_REPO" --prerelease --title "$SIGN_TAG" \
       --notes "temp unity-signing payload (auto-deleted)" "$D/unsigned.zip"
    SINCE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    gh workflow run sign-artifact -R "$SIGN_REPO" -f release_tag="$SIGN_TAG"
    RID=""
    for _ in $(seq 1 20); do
      RID=$(gh run list -R "$SIGN_REPO" --workflow sign-artifact --event workflow_dispatch \
              --limit 8 --json databaseId,createdAt \
              --jq "[.[]|select(.createdAt>=\"$SINCE\")]|sort_by(.createdAt)|last|.databaseId // empty")
      [ -n "$RID" ] && break; sleep 4
    done
    if [ -n "$RID" ] && gh run watch "$RID" -R "$SIGN_REPO" --interval 15 --exit-status; then
      gh run download "$RID" -R "$SIGN_REPO" -n signed -D "$D/out"
      # portable unzip (git-bash on Windows has no `unzip`).
      if command -v unzip >/dev/null; then ( cd "$D/out" && unzip -qo signed.zip -d "$D/signed" 2>/dev/null || true )
      else powershell -NoProfile -Command "Expand-Archive -Path '$(cygpath -w "$D/out/signed.zip")' -DestinationPath '$(cygpath -w "$D/signed")' -Force"; fi
      SIGNED_DLL=$(ls "$D/signed/displayxr_unity.dll" 2>/dev/null | head -1)
    fi
    gh release delete "$SIGN_TAG" -R "$SIGN_REPO" --yes --cleanup-tag >/dev/null 2>&1 || true

    if [ -z "$SIGNED_DLL" ]; then
      echo "⚠ sign-artifact did not return a signed DLL — ships unsigned."; SIGNED=no
    else
      # Channel 1 — repack the .tgz with the signed DLL, re-upload over the asset.
      cp "$SIGNED_DLL" "$DLL"
      ( cd "$D/x" && tar czf "$D/$TGZ" "$(basename "$PKGDIR")" )
      gh release upload [VERSION] "$D/$TGZ" --clobber -R DisplayXR/displayxr-unity

      # Channel 2 — put the signed DLL on the `upm` branch + move its version tag.
      # CI already force-pushed `upm` with the unsigned DLL; layer the signed one on top.
      git fetch origin upm --quiet && git checkout -B upm origin/upm
      cp "$SIGNED_DLL" "$DLL_REL"; git add -f "$DLL_REL"
      git commit -q -m "Sign displayxr_unity.dll for [VERSION] (Leia EV)" || echo "(upm already signed)"
      git push -f origin upm
      git tag -f "upm/[VERSION]" && git push -f origin "upm/[VERSION]"
      git checkout main
      echo "✅ signed displayxr_unity.dll re-injected into the .tgz asset + upm branch (Valid/Leia EV)."
    fi
  fi
  rm -rf "$D"
fi
```

Verify (optional): `gh run download` the `.tgz`, extract, and
`Get-AuthenticodeSignature` the DLL should report `Valid` / signer `Leia, Inc.`.

Note: the macOS `displayxr_unity.bundle` is signed with an Apple
Developer ID cert + notarization (a separate track from the Windows EV
cert) — out of scope here; flag it as macOS-unsigned in the report.
Carry `SIGNED` into the final report.

---

## PHASE 4: VERIFY AND REPORT

### Step 4.1: Verify GitHub Release exists
```bash
gh release view [VERSION] --repo DisplayXR/displayxr-unity --json tagName,name,assets
```
- Verify tag exists, release was created, `.tgz` asset is attached (`com.displayxr.unity-[VERSION_NUMBER].tgz`)

### Step 4.2: Verify upm branch and tag
```bash
git ls-remote origin refs/heads/upm
git ls-remote origin refs/tags/upm/[VERSION]
```
- Verify both exist

### Step 4.3: Report
```
DisplayXR Unity [VERSION] published successfully!

Build:
  - Windows x64:  PASSED
  - macOS Universal: PASSED
  - Release job: PASSED (run #RUN_ID)

Signing:  [SIGNED=yes → "displayxr_unity.dll signed (Leia EV) on the provider runner and re-injected into the .tgz asset + upm branch"] | [SIGNED=no → "⚠ Windows DLL UNSIGNED — DXR_SIGN_REPO unset/unreachable; set it and re-run /release [VERSION] to sign in place"]  (macOS .bundle: unsigned — separate Apple Developer ID track)

Published to:
  - GitHub Release: https://github.com/DisplayXR/displayxr-unity/releases/tag/[VERSION]
  - UPM branch: https://github.com/DisplayXR/displayxr-unity/tree/upm
  - UPM tag: https://github.com/DisplayXR/displayxr-unity/tree/upm/[VERSION]

Install in Unity (Package Manager → Add from git URL):
  https://github.com/DisplayXR/displayxr-unity.git#upm/[VERSION]

Changelog:
  [generated changelog summary]
```

STOP.

---

## PHASE 5: ROLLBACK (on build failure)

### Step 5.1: Delete tag
```bash
git tag -d [VERSION]
git push --delete origin [VERSION]
```

### Step 5.2: Revert version bump
```bash
git revert HEAD --no-edit
git push origin main
```

### Step 5.3: Get error logs
Run: `gh run view RUN_ID --log-failed | tail -200`

### Step 5.4: Report
```
DisplayXR Unity [VERSION] release FAILED — rolled back.

Error from build:
[error summary]

Tag, version bump, and CHANGELOG entry have been reverted.
Fix the issue and try again with /release [VERSION]
```

STOP.
```
