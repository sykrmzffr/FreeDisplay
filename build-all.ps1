# Builds everything and puts the single distributable Tandem.exe in dist\.
#   1. the tablet app (Gradle, needs JDK 17+ - Android Studio's bundled JBR works)
#   2. the Windows app with the tablet app and adb embedded (needs VS 2022 C++ tools)
$ErrorActionPreference = "Continue" # native tools write warnings to stderr; exit codes are checked
$root = $PSScriptRoot

if (-not $env:JAVA_HOME) {
    $jbr = "$env:ProgramFiles\Android\Android Studio\jbr"
    if (Test-Path $jbr) { $env:JAVA_HOME = $jbr }
}
$gradle = Get-Command gradle -ErrorAction SilentlyContinue
$gradle = if ($gradle) { $gradle.Source } else {
    # Fall back to a Gradle distribution already downloaded by Android Studio.
    Get-ChildItem "$env:USERPROFILE\.gradle\wrapper\dists\gradle-*-bin\*\gradle-*\bin\gradle.bat" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $gradle) { throw "Gradle not found. Install Android Studio (and open android\ once) or put gradle on PATH." }

Write-Host "== Building tablet app"
Push-Location "$root\android"
try {
    & $gradle --offline -q assembleRelease
    if ($LASTEXITCODE) { & $gradle -q assembleRelease } # first build may need to download dependencies
    if ($LASTEXITCODE) { throw "Gradle build failed" }
} finally { Pop-Location }

Write-Host "== Building Windows app"
& "$root\host\build.bat"
if ($LASTEXITCODE) { throw "Host build failed" }

# The APK goes in a subfolder: next to the exe it would make the exe run in place
# instead of offering to install. It is only needed for installing by hand.
New-Item -ItemType Directory -Force "$root\dist\tablet" | Out-Null
Copy-Item "$root\host\out\Tandem.exe" "$root\dist\Tandem.exe" -Force
Copy-Item "$root\host\out\payload\Tandem.apk" "$root\dist\tablet\Tandem.apk" -Force
$exe = Get-Item "$root\dist\Tandem.exe"
Write-Host ("== Done: {0} ({1:N1} MB) and dist\tablet\Tandem.apk" -f $exe.FullName, ($exe.Length / 1MB))
