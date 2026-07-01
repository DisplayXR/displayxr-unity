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
GitHub-hosted runner (unsigned; the code-signing key is held on a build
machine and isn't available to cloud CI, and no secret lives in this
public repo). So a signed release is produced by signing the shipped DLL
on a signing-capable machine and re-publishing.

Signing does **not** require rebuilding — we sign the CI-built DLL in
place inside both distribution channels (the `.tgz` asset and the `upm`
orphan branch). No-ops cleanly without signing capability.

### Step 3.5.1: Capability check
Two methods; neither names a signing endpoint. Unity signs a **prebuilt** DLL
(no build step), so the **remote** method needs no Windows host — it works from
anywhere with `gh`.
- **Remote** — `$DXR_SIGN_HOOK` (a local executable that signs a folder in place).
- **Local** — `$SIGN_CMD` (per-file signer; needs a Windows host for signtool).

```bash
if [ -n "$DXR_SIGN_HOOK" ] && [ -x "$DXR_SIGN_HOOK" ]; then
  SIGN_METHOD=remote; SIGNED=yes
elif [ -n "$SIGN_CMD" ] && uname -s | grep -qiE 'mingw|msys|cygwin|windows'; then
  SIGN_METHOD=local;  SIGNED=yes
else
  echo "⚠  SIGNING SKIPPED — no signing capability; release DLL stays UNSIGNED."
  echo "   Set DXR_SIGN_HOOK (remote) or SIGN_CMD (local, Windows) and re-run."
  SIGN_METHOD=none; SIGNED=no
fi

# Sign a folder of binaries in place, by whichever method is active:
sign_folder() {
  if [ "$SIGN_METHOD" = remote ]; then "$DXR_SIGN_HOOK" "$1"
  else powershell -NoProfile -ExecutionPolicy Bypass -File Scripts\\sign-release.ps1 -Path "$1" -SignCmd "$SIGN_CMD"; fi
}
```

### Step 3.5.2: Sign the DLL in the `.tgz` asset, repack, re-upload (if SIGNED=yes)
```bash
TGZ="com.displayxr.unity-[VERSION_NUMBER].tgz"
gh release download [VERSION] --repo DisplayXR/displayxr-unity --pattern "$TGZ" --dir /tmp/usign
mkdir -p /tmp/usign/x && tar -xzf "/tmp/usign/$TGZ" -C /tmp/usign/x
DLL=$(find /tmp/usign/x -ipath '*Windows/x64/displayxr_unity.dll')
sign_folder "$(dirname "$DLL")"
# repack with the same internal layout (npm packages root at 'package/')
( cd /tmp/usign/x && tar -czf "/tmp/usign/$TGZ" package )
gh release upload [VERSION] "/tmp/usign/$TGZ" --clobber --repo DisplayXR/displayxr-unity
```

### Step 3.5.3: Sign the DLL on the `upm` branch too
Users who install by git URL get the DLL from the `upm` orphan branch,
not the `.tgz` — sign that copy as well.
```bash
git fetch origin upm && git checkout upm
sign_folder "Runtime/Plugins/Windows/x64"
git commit -am "Sign displayxr_unity.dll for [VERSION]"
git push origin upm
# move the upm/[VERSION] tag onto the signed commit so the pinned install is signed too
git tag -f "upm/[VERSION]" && git push -f origin "upm/[VERSION]"
git checkout main
```

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
