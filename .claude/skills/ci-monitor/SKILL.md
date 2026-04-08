---
name: ci-monitor
description: Automate git commit, push, GitHub Actions build monitoring, and auto-fix common build errors. Use /ci-monitor to commit current changes and monitor the build, or /ci-monitor "message" to specify commit message. This skill launches a subagent to save context.
allowed-tools: Read, Grep, Glob, Bash, Task, Edit, Write
---

# CI-Monitor Skill

This skill automates the complete build workflow for the DisplayXR Unity plugin native code: commit → push → monitor → diagnose/fix errors.

## Architecture

```
Local: commit → push
GitHub Actions: build-native.yml → CMake (Windows x64 + macOS Universal) → Upload artifacts
                      ↓ (on failure)
Local: analyze logs → apply fix → commit fix → push → re-monitor (up to 3 attempts)
```

**Build Configuration:**
- **Platforms:** Windows (windows-latest) and macOS (macos-latest)
- **Build System:** CMake (MSVC on Windows, Apple Clang on macOS)
- **Source:** `native~/` directory (C11/C++17 shared library)
- **Dependencies:** OpenXR headers fetched via CMake FetchContent
- **Artifacts:** `displayxr_unity-windows-x64` (DLL), `displayxr_unity-macos` (bundle), `displayxr-unity-plugin` (combined)

**Important:** The workflow only triggers on changes to `native~/` or the workflow file itself. If you only changed C#/Editor files, the workflow won't run — report this to the user and STOP.

## CRITICAL: Launch Subagent to Save Context

**You MUST use the Task tool with `subagent_type="general-purpose"` to execute this workflow.**

The subagent handles all heavy work (git, build monitoring, log parsing, fixes) in its own context.

### How to Invoke

When this skill is triggered, immediately call:

```
Task(
  subagent_type="general-purpose",
  description="Build monitor workflow",
  prompt="[Full workflow prompt below, with USER_MESSAGE replaced]"
)
```

### Gathering Files to Commit

Before launching the subagent, you MUST determine which files to include in the commit:

1. **Review your conversation history** for every file you modified via Edit or Write tools during this session. Collect these paths into a list.
2. **Cross-reference with `git status --short`** to confirm each file is actually dirty (modified/untracked). Drop any that are clean.
3. **Build the `[FILES_TO_COMMIT]` value:**
   - If you found session-modified files: use a newline-separated list of paths (e.g., `native~/displayxr_hooks.cpp\nnative~/displayxr_kooima.h`)
   - If you have no tracked session files (e.g., user invoked `/ci-monitor` directly without prior edits): use the literal string `AUTO`
4. **Substitute `[FILES_TO_COMMIT]`** in the subagent prompt template below.

### Determining the Issue Reference

Before launching the subagent, determine the related GitHub issue number:

1. **Check conversation context** — the user may have mentioned an issue number (e.g., "working on #93", or a commit referencing an issue).
2. **Check recent commits** — run `git log --oneline -5` to see if recent commits reference an issue.
3. **Build the `[ISSUE_REF]` value:**
   - If an issue number is known: use e.g. `#93`
   - If no issue is identifiable: use `NONE`
4. **Substitute `[ISSUE_REF]`** in the subagent prompt template below.

---

## Subagent Prompt Template

Pass this complete prompt to the subagent (replace `[USER_MESSAGE]` with the user's commit message or "auto-generate", `[FILES_TO_COMMIT]` with the file list or "AUTO", and `[ISSUE_REF]` with the issue number or "NONE"):

```
Execute the displayxr-unity ci-monitor workflow. You have access to Edit and Write tools to fix build errors.

Working directory: /Users/david.fattal/Documents/GitHub/displayxr-unity

## Configuration
- Repository: DisplayXR/displayxr-unity (DisplayXR Unity plugin)
- Workflow: build-native.yml
- Jobs: build-windows (Windows x64, MSVC), build-macos (macOS Universal, Apple Clang)
- Artifact Names: displayxr_unity-windows-x64, displayxr_unity-macos, displayxr-unity-plugin
- Build: CMake (native~/ directory)
- Max Fix Attempts: 3

## Commit Message
[USER_MESSAGE]

## Issue Reference
[ISSUE_REF]

## Files to Commit
[FILES_TO_COMMIT]

If the above is a list of file paths, stage ONLY those files in Phase 1.
If the above is "AUTO" or empty/missing, snapshot the dirty files at invocation time (see Phase 1 instructions).
NEVER use `git add -A` or `git add .` under any circumstances.

---

## PHASE 1: COMMIT AND PUSH

### Step 1.1: Pre-flight Check
Run: `cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git status`
- If no changes to commit, report "Nothing to commit" and STOP.
- Otherwise, continue to Step 1.2.

### Step 1.2: Stage Changes (Selective)

**If the "Files to Commit" section above contains a file list (not "AUTO"):**
- Stage ONLY those specific files:
  ```bash
  cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git add path/to/file1 path/to/file2 ...
  ```

**If "Files to Commit" is "AUTO" or empty/missing (fallback):**
- Snapshot the currently dirty files RIGHT NOW to prevent drift during build monitoring:
  ```bash
  cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git status --short | awk '{print $NF}'
  ```
- Store this list, then stage ONLY those files:
  ```bash
  git add <each file from snapshot>
  ```

**NEVER use `git add -A` or `git add .`** — this prevents unrelated dirty files from being swept into the commit.

### Step 1.3: Generate Commit Message (if needed)
If commit message is "auto-generate":
- Run: `git diff --cached --stat`
- Examine the changes and create a descriptive message summarizing what changed

### Step 1.4: Create Commit

**Issue reference:** If the "Issue Reference" section above contains an issue number (e.g., `#93`),
append it to the first line of the commit message in parentheses — e.g., `Fix linker error (#93)`.
If the issue reference is "NONE", omit it.

Run:
```bash
cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git commit -m "$(cat <<'EOF'
[YOUR COMMIT MESSAGE HERE (with issue ref if applicable)]

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```
**CRITICAL:** Extract and store the commit hash from the output (e.g., `[branch abc1234]`).
Run: `git rev-parse HEAD`
Store this as COMMIT_SHA - you will need it to verify the correct build is monitored.

### Step 1.5: Check if Native Files Changed
The workflow only triggers on `native~/` or workflow file changes. Check:
```bash
git diff HEAD~1 --name-only | grep -E '^(native~/|\.github/workflows/build-native\.yml)'
```
If no native files changed, report to the user:
"Pushed successfully, but no native code changed — the build-native.yml workflow won't trigger. Only C# and editor changes were committed."
Then STOP (no build to monitor).

### Step 1.6: Push
Run: `cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git push origin HEAD`
- Note the branch name from output
- If push fails, report the error and STOP

---

## PHASE 2: MONITOR BUILD

### Step 2.1: Wait for Workflow to Register
Run: `sleep 10`
(GitHub Actions needs time to register the new push)

### Step 2.2: Get Run ID for OUR Commit (CRITICAL)
**You MUST verify the run is for the exact commit you pushed, not a previous run.**

Run this command to find runs and check their commit SHA:
```bash
gh run list -R DisplayXR/displayxr-unity --limit 5 --json databaseId,status,headBranch,headSha,displayTitle
```

**Verification loop:**
1. Look for a run where `headSha` starts with your COMMIT_SHA (first 7+ chars match)
2. If no matching run found, wait 10 seconds and retry (up to 6 retries = 1 minute)
3. If still no matching run after retries, the workflow may not have triggered (path filter). Report this and STOP.

Once you find the matching run, store its `databaseId` as RUN_ID.

### Step 2.3: Watch Build
Run: `gh run watch -R DisplayXR/displayxr-unity RUN_ID --interval 15` (use timeout 600000ms = 10 min)

### Step 2.4: Check Result
Run: `gh run view -R DisplayXR/displayxr-unity RUN_ID --json status,conclusion`
- If conclusion is "success": Go to PHASE 4 (Report Success)
- If conclusion is "failure": Go to PHASE 3 (Diagnose and Fix)
- If status is still "in_progress": The watch command may have timed out, check again

---

## PHASE 3: DIAGNOSE AND FIX (Loop up to 3 times)

Track: fix_attempt = 1
Track: fix_files_modified = [] (append every file path you modify with Edit/Write during this phase)

### Step 3.1: Get Error Logs
The build has two jobs. Check which failed:
```bash
gh run view -R DisplayXR/displayxr-unity RUN_ID --json jobs --jq '.jobs[] | select(.conclusion == "failure") | .name'
```

Then get the failed logs:
```bash
gh run view -R DisplayXR/displayxr-unity RUN_ID --log-failed | tail -200
```
Save the output for analysis.

### Step 3.2: Identify Error Type
Parse the logs and look for these patterns (in order of priority):

**Pattern A: Missing include file**
MSVC:
```
fatal error C1083: Cannot open include file: 'XYZ.h'
```
Clang:
```
fatal error: 'XYZ.h' file not found
```
→ Go to Fix A

**Pattern B: Undeclared identifier / use of undeclared identifier**
MSVC:
```
error C2065: 'XYZ': undeclared identifier
```
Clang:
```
error: use of undeclared identifier 'XYZ'
```
→ Go to Fix B

**Pattern C: Not a member of struct/class**
MSVC:
```
error C2039: 'member_name': is not a member of 'StructName'
```
Clang:
```
error: no member named 'member_name' in 'StructName'
```
→ Go to Fix C

**Pattern D: Unresolved external symbol (linker)**
MSVC:
```
error LNK2019: unresolved external symbol "function_name"
```
macOS:
```
Undefined symbols for architecture x86_64: "_function_name"
```
→ Go to Fix D

**Pattern E: Redefinition error**
```
error C2084: function 'X' already has a body
error C2011: 'X': 'struct' type redefinition
error: redefinition of 'X'
```
→ Go to Fix E

**Pattern F: CMake configuration error**
```
CMake Error at
Could not find a package configuration file provided by
```
→ Go to Fix F

**No match found**: Report error with logs and STOP (manual intervention needed)

---

### FIX A: Missing Include File

1. Extract the missing filename from error
2. Search for the file in the native~/ source:
   ```bash
   find /Users/david.fattal/Documents/GitHub/displayxr-unity/native~ -name "FILENAME" 2>/dev/null
   ```
3. If file exists:
   - Check the include path in the failing file
   - Use Edit tool to fix the include path
4. If file doesn't exist:
   - Check if it's an OpenXR header (should be fetched by FetchContent)
   - Check if it's a typo or was renamed
5. Go to Step 3.5

### FIX B: Undeclared Identifier

1. Extract the identifier name from error
2. Search for where it's defined:
   ```bash
   grep -rn "IDENTIFIER" /Users/david.fattal/Documents/GitHub/displayxr-unity/native~/ --include="*.h"
   ```
3. If found in a header:
   - Use Edit tool to add the missing #include to the failing file
4. If it's a new identifier that should be defined:
   - Check surrounding code for context
   - Add the definition or declaration where appropriate
5. Go to Step 3.5

### FIX C: Not a Member of Struct

1. Extract struct/class name and member name from error
2. Find the struct definition:
   ```bash
   grep -rn "struct StructName" /Users/david.fattal/Documents/GitHub/displayxr-unity/native~/ --include="*.h" -A 50
   ```
3. Check what members actually exist
4. Use Edit tool to:
   - Fix the member name to the correct one, OR
   - Add the missing member to the struct
5. Go to Step 3.5

### FIX D: Unresolved External Symbol (Linker Error)

1. Extract the function name from error
2. Check if function is declared but not implemented:
   ```bash
   grep -rn "function_name" /Users/david.fattal/Documents/GitHub/displayxr-unity/native~/ --include="*.c" --include="*.cpp" --include="*.h"
   ```
3. If implementation is missing:
   - Find where it should be implemented (look at similar functions)
   - Use Write or Edit tool to add the implementation
4. If it's a library linking issue:
   - Check native~/CMakeLists.txt for missing target_link_libraries
   - Add the missing library
5. Go to Step 3.5

### FIX E: Redefinition Error

1. Find all definitions:
   ```bash
   grep -rn "SYMBOL_NAME" /Users/david.fattal/Documents/GitHub/displayxr-unity/native~/ --include="*.h" --include="*.c" --include="*.cpp"
   ```
2. Identify the issue type:

   **For "different basic types":** This usually means a function is called before it's declared.
   In C, undeclared functions are assumed to return `int`, causing a conflict when the actual
   definition is found. Fix by adding a forward declaration before the first call.

   **For true duplicates:**
   - Remove the duplicate definition, OR
   - Add include guards if missing, OR
   - Use #pragma once

3. Go to Step 3.5

### FIX F: CMake Configuration Error

1. Read the CMake error message carefully
2. Common issues:
   - OpenXR FetchContent failed → check network/git tag in CMakeLists.txt
   - Generator mismatch → check the cmake command in the workflow
3. Fix the CMakeLists.txt or workflow file as needed
4. Go to Step 3.5

---

### Step 3.5: Commit the Fix (Selective Staging)

Stage ONLY the files you modified during this fix attempt (from your `fix_files_modified` list):
```bash
cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git add path/to/fixed_file1 path/to/fixed_file2 ...
```
**NEVER use `git add -A` or `git add .`** — only stage files you directly edited with Edit/Write tools.

Then commit:
```bash
cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git commit -m "$(cat <<'EOF'
Fix: [brief description of what was fixed]

Auto-fix attempt fix_attempt/3

Co-Authored-By: Claude Opus 4.6 <noreply@anthropic.com>
EOF
)"
```

### Step 3.6: Push the Fix
Run: `cd /Users/david.fattal/Documents/GitHub/displayxr-unity && git push origin HEAD`

### Step 3.7: Re-monitor
- Increment fix_attempt
- If fix_attempt > 3: Go to PHASE 5 (Report Fix Failure)
- Otherwise:
  1. Get the new COMMIT_SHA: `git rev-parse HEAD`
  2. Return to Step 2.1 (wait for new workflow)
  3. **CRITICAL:** Use the NEW commit SHA to verify the correct run in Step 2.2

---

## PHASE 4: DOWNLOAD ARTIFACTS AND REPORT SUCCESS

### Step 4.1: Download Windows DLL from CI
The CI builds the Windows DLL with MSVC — this is the canonical binary for the plugin.
Download it and place it in the correct location:

```bash
cd /Users/david.fattal/Documents/GitHub/displayxr-unity
rm -rf /tmp/displayxr-ci-artifacts
gh run download -R DisplayXR/displayxr-unity RUN_ID -n displayxr_unity-windows-x64 -D /tmp/displayxr-ci-artifacts
```

Then copy the DLL to the plugin directory:
```bash
cp /tmp/displayxr-ci-artifacts/displayxr_unity.dll Runtime/Plugins/Windows/x64/displayxr_unity.dll
```

Verify it was placed correctly:
```bash
ls -la Runtime/Plugins/Windows/x64/displayxr_unity.dll
```

Clean up:
```bash
rm -rf /tmp/displayxr-ci-artifacts
```

**IMPORTANT:** Do NOT commit the downloaded DLL — it's a local-only artifact for the user's Unity editor to build Windows apps. The CI workflow handles binary distribution via the `upm` branch and releases.

### Step 4.2: Report

Run: `gh run view -R DisplayXR/displayxr-unity RUN_ID --json conclusion,databaseId,url,updatedAt`

Report:
```
Build completed successfully!
- Committed: '[message]' ([N] files changed)
- Pushed to: [branch]
- Build: SUCCEEDED (run #RUN_ID)
- URL: [workflow URL]
- Windows DLL: Downloaded from CI (MSVC) → Runtime/Plugins/Windows/x64/displayxr_unity.dll
- macOS bundle: Built locally via build-mac.sh (already up to date)
```

If there were fix attempts, add:
```
- Auto-fixes applied: [N] (see commit history)
```

STOP.

---

## PHASE 5: REPORT FIX FAILURE

Report:
```
Build FAILED after [N] fix attempts

Original error: [first error encountered]
Fixes attempted:
1. [description of fix 1]
2. [description of fix 2]
3. [description of fix 3]

Current error: [remaining error from logs]
Build URL: [workflow URL]

Manual intervention required.
```

STOP.

---

## Key Files Reference

When looking for fixes, check these locations:

**Native plugin source (native~/):**
- `displayxr_hooks.cpp/.h` - OpenXR function hook chain (xrLocateViews, xrCreateSession, etc.)
- `displayxr_kooima.cpp/.h` - Kooima asymmetric frustum projection math
- `displayxr_shared_state.cpp/.h` - Thread-safe double-buffered state between C# and native
- `displayxr_readback.cpp/.h` - Compositor output readback for preview
- `displayxr_extensions.h` - OpenXR extension struct definitions (must match runtime)
- `display3d_view.c/.h` - Display geometry and view computation
- `CMakeLists.txt` - Build configuration, FetchContent for OpenXR headers

**C# scripts (Runtime/, Editor/):**
- Not built by CI — these are compiled by Unity at project load time
- Changes here won't trigger the build-native.yml workflow

**Build configuration:**
- `.github/workflows/build-native.yml` - CI workflow
- `native~/CMakeLists.txt` - Native plugin CMake build
```

---

## GitHub Actions Workflow Details

The workflow (`build-native.yml`) does:

### build-windows job:
1. **Checkout** - Clones repo
2. **Configure** - `cmake -S native~ -B native~/build -A x64 -DCMAKE_BUILD_TYPE=Release`
3. **Build** - `cmake --build native~/build --config Release`
4. **Verify** - Checks `Runtime/Plugins/Windows/x64/displayxr_unity.dll` exists
5. **Upload** - Artifact `displayxr_unity-windows-x64`

### build-macos job:
1. **Checkout** - Clones repo
2. **Configure** - `cmake -S native~ -B native~/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"`
3. **Build** - `cmake --build native~/build --config Release`
4. **Verify** - Checks `Runtime/Plugins/macOS/displayxr_unity.bundle` exists
5. **Upload** - Artifact `displayxr_unity-macos`

### package job (main branch push only):
- Downloads both artifacts and uploads combined `displayxr-unity-plugin`

### Triggers
- Push to `main` branch (only `native~/` or workflow changes)
- Pull requests to `main` (only `native~/` or workflow changes)
- Manual `workflow_dispatch`

---

## Usage Examples

### With commit message:
```
/ci-monitor "Fix Kooima projection for off-center eye positions"
```

### Without message (auto-generate):
```
/ci-monitor
```

### Just monitor current build (no commit):
```
/ci-monitor --watch-only
```
For watch-only mode, skip PHASE 1 and start at PHASE 2, using the latest run for the current branch.
