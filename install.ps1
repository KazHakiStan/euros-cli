# Euros terminal client installer for Windows (PowerShell 5.1+).
#
#   irm https://raw.githubusercontent.com/KazHakiStan/euros-cli/master/install.ps1 | iex
#   & ([scriptblock]::Create((irm .../install.ps1))) -Uninstall
#
# $env:EUROS_VERSION = 'v0.2.0'   a specific release instead of the latest
# $env:EUROS_INSTALL_DIR = '...'   where to put it (default %LOCALAPPDATA%\Programs\euros)
# $env:EUROS_REPO = 'owner/name'   another GitHub repository
# $env:EUROS_DOWNLOAD_BASE = 'url'  where the zip and SHA256SUMS are (a mirror)
# $env:EUROS_NO_MODIFY_PATH = '1'   don't touch PATH
param(
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$Repo = 'KazHakiStan/euros-cli'
if ($env:EUROS_REPO) { $Repo = $env:EUROS_REPO }
$Version = $env:EUROS_VERSION
$Dir = Join-Path $env:LOCALAPPDATA 'Programs\euros'
if ($env:EUROS_INSTALL_DIR) { $Dir = $env:EUROS_INSTALL_DIR }
$Asset = 'euros-windows-x86_64.zip'

# throw, not exit: under `irm | iex` exit would close the user's PowerShell window.
function Fail($message) {
    throw "euros install: $message"
}

if ($Repo -notmatch '^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$') { Fail "bad EUROS_REPO: $Repo" }
if ($Version -and $Version -notmatch '^v[0-9]+(\.[0-9]+){0,2}(-[A-Za-z0-9.]+)?$') { Fail "bad EUROS_VERSION: $Version" }

# User PATH lives in the registry as REG_EXPAND_SZ; keep %VARS% in it unexpanded.
function Get-UserPath {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    $value = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $key.Close()
    return $value
}

function Set-UserPath($value) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    $key.SetValue('Path', $value, [Microsoft.Win32.RegistryValueKind]::ExpandString)
    $key.Close()
    # Tell Explorer (and so new terminals) that the environment changed.
    if (-not ('Euros.Env' -as [type])) {
        Add-Type -Namespace Euros -Name Env -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
    uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
    }
    $result = [UIntPtr]::Zero
    [Euros.Env]::SendMessageTimeout([IntPtr]0xffff, 0x1A, [UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result) | Out-Null
}

function Test-SameDir($a, $b) {
    $x = [Environment]::ExpandEnvironmentVariables($a).TrimEnd('\')
    $y = [Environment]::ExpandEnvironmentVariables($b).TrimEnd('\')
    return [string]::Equals($x, $y, [StringComparison]::OrdinalIgnoreCase)
}

function Add-ToPath {
    $parts = @((Get-UserPath) -split ';' | Where-Object { $_ })
    if ($parts | Where-Object { Test-SameDir $_ $Dir }) { return $false }
    Set-UserPath ((@($parts) + $Dir) -join ';')
    $env:Path = "$env:Path;$Dir"
    return $true
}

function Remove-FromPath {
    $parts = @((Get-UserPath) -split ';' | Where-Object { $_ })
    $kept = @($parts | Where-Object { -not (Test-SameDir $_ $Dir) })
    if ($kept.Count -ne $parts.Count) {
        Set-UserPath ($kept -join ';')
        Write-Host "Removed $Dir from PATH"
    }
}

function Invoke-Uninstall {
    Remove-FromPath
    if (Test-Path $Dir) {
        # A running euros.exe can't be deleted, but it can be renamed out of the way.
        Get-ChildItem $Dir -File | ForEach-Object {
            try { Remove-Item $_.FullName -Force } catch { Rename-Item $_.FullName "$($_.Name).old" -Force -ErrorAction SilentlyContinue }
        }
        Remove-Item $Dir -Recurse -Force -ErrorAction SilentlyContinue
    }
    Write-Host "euros is uninstalled. Your session stays in %APPDATA%\euros (run 'euros logout' first to remove it)."
}

function Invoke-Install {
    if (-not [Environment]::Is64BitOperatingSystem) { Fail 'only 64-bit Windows is supported' }
    if ($env:EUROS_DOWNLOAD_BASE) {
        $base = $env:EUROS_DOWNLOAD_BASE
    } elseif ($Version) {
        $base = "https://github.com/$Repo/releases/download/$Version"
    } else {
        $base = "https://github.com/$Repo/releases/latest/download"
    }
    $label = if ($Version) { $Version } else { '(latest)' }

    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("euros-" + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory $tmp | Out-Null
    try {
        Write-Host "Downloading $Asset $label..."
        $zip = Join-Path $tmp $Asset
        $sums = Join-Path $tmp 'SHA256SUMS'
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "$base/$Asset" -OutFile $zip
            Invoke-WebRequest -UseBasicParsing -Uri "$base/SHA256SUMS" -OutFile $sums
        } catch {
            Fail "download failed from $base ($($_.Exception.Message))"
        }

        $expected = $null
        foreach ($line in Get-Content $sums) {
            $fields = $line -split '\s+'
            if ($fields.Count -ge 2 -and $fields[1].TrimStart('*') -eq $Asset) { $expected = $fields[0] }
        }
        if (-not $expected) { Fail "$Asset is not listed in SHA256SUMS" }
        $actual = (Get-FileHash -Algorithm SHA256 $zip).Hash
        if ($actual -ne $expected.ToUpperInvariant()) {
            Fail "checksum mismatch for ${Asset}: the download is damaged or was tampered with"
        }

        $unpacked = Join-Path $tmp 'unpacked'
        Expand-Archive -Path $zip -DestinationPath $unpacked -Force
        if (-not (Test-Path (Join-Path $unpacked 'euros.exe'))) { Fail 'the archive has no euros.exe' }

        New-Item -ItemType Directory -Force $Dir | Out-Null
        Get-ChildItem $Dir -Filter '*.old' -File | Remove-Item -Force -ErrorAction SilentlyContinue
        foreach ($file in Get-ChildItem $unpacked -File) {
            $target = Join-Path $Dir $file.Name
            if (Test-Path $target) {
                # euros.exe and its DLLs may be in use (euros update runs this): move them aside first.
                try { Remove-Item $target -Force } catch { Rename-Item $target "$($file.Name).old" -Force }
            }
            Copy-Item $file.FullName $target
        }
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    $added = $false
    if ($env:EUROS_NO_MODIFY_PATH) {
        Write-Host "Add $Dir to your PATH to run euros."
    } else {
        $added = Add-ToPath
    }
    $exe = Join-Path $Dir 'euros.exe'
    $installed = & $exe --version 2>$null
    if (-not $installed) { $installed = 'euros' }
    Write-Host ''
    Write-Host "$installed is installed in $Dir" -ForegroundColor Green
    if ($added) {
        Write-Host "Added $Dir to your PATH. Open a new terminal, then: euros login"
    } else {
        Write-Host 'Next: euros login'
    }
}

if ($Uninstall -or $env:EUROS_UNINSTALL) {
    Invoke-Uninstall
} else {
    Invoke-Install
}
