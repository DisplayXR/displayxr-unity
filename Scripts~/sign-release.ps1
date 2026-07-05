<#
.SYNOPSIS
  Sign all DisplayXR-produced binaries in a release folder, then verify the
  whole tree is signed.

.DESCRIPTION
  Signing is driven entirely by the command passed in -SignCmd (sourced from
  the SIGN_CMD environment variable by the build scripts). This script holds
  NO certificate and NO secret. On machines without signing capability,
  SIGN_CMD is unset and the build scripts skip signing — the build is
  unsigned, with no behavior change.

    1. Walk the folder for *.dll / *.exe.
    2. Any binary WITHOUT a Valid Authenticode signature -> sign it via the
       configured signer. Binaries that already carry a valid signature
       (e.g. bundled third-party DLLs) are left untouched.
    3. Re-verify the ENTIRE tree. If anything is still unsigned, FAIL (exit 1).

  -SignCmd receives the file path as a trailing argument. Callers pass a full
  signing command (including their own timestamp authority) via SIGN_CMD; the
  default below is a minimal fallback for ad-hoc use.

.EXAMPLE
  # sign a staged binary tree before packaging
  .\sign-release.ps1 -Path .\_package\bin -SignCmd "$env:SIGN_CMD"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    # Signing command; receives the file path as a trailing argument.
    # Production callers pass a full command (with a timestamp authority)
    # via the SIGN_CMD environment variable. This default is a bare fallback.
    [string]$SignCmd = 'signtool sign /fd sha256 /a',

    # Skip signing files already validly signed by anyone (default). Use
    # -Force to re-sign every binary regardless (overwrites existing sigs).
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Test-Signed($file) {
    (Get-AuthenticodeSignature $file).Status -eq 'Valid'
}

function Invoke-Sign($file) {
    $parts = $SignCmd -split '\s+'
    $exe   = $parts[0]
    $args  = @($parts[1..($parts.Length - 1)]) + $file
    & $exe @args
    if ($LASTEXITCODE -ne 0) { throw "Signing failed for $file (exit $LASTEXITCODE)" }
}

if (-not (Test-Path $Path)) { throw "Path not found: $Path" }

$binaries = Get-ChildItem -Path $Path -Recurse -Include *.dll, *.exe -File
Write-Host "Found $($binaries.Count) binaries under $Path"

$signed = 0; $skipped = 0
foreach ($b in $binaries) {
    if (-not $Force -and (Test-Signed $b.FullName)) {
        $who = (Get-AuthenticodeSignature $b.FullName).SignerCertificate.Subject
        Write-Host "  skip (already signed) : $($b.Name)  <$who>"
        $skipped++
        continue
    }
    Write-Host "  signing               : $($b.Name)"
    Invoke-Sign $b.FullName
    $signed++
}

Write-Host "Signed $signed, skipped $skipped already-signed."

# ---- Verification gate ----
$unsigned = $binaries | Where-Object { -not (Test-Signed $_.FullName) }
if ($unsigned) {
    Write-Host "`nFAIL: the following binaries are still unsigned:" -ForegroundColor Red
    $unsigned | ForEach-Object { Write-Host "  $($_.FullName)" -ForegroundColor Red }
    exit 1
}

Write-Host "`nOK: all $($binaries.Count) binaries under $Path are signed." -ForegroundColor Green
