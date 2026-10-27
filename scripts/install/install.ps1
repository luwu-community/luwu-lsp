# Installs luwu-lsp and sets up your editors on Windows:
#
#     irm https://raw.githubusercontent.com/luwu-community/luwu-lsp/luwu/scripts/install/install.ps1 | iex
#
# Everything the installer does is in install.luau, a seal script. This only finds a seal to run it with: the one on
# your PATH when it's newer than 0.8.0, or else the latest release, downloaded into a temporary directory that's deleted
# afterwards. To pass install.luau arguments (`--help` lists them; `--keep-seal` keeps the downloaded seal), run this as
# a script block:
#
#     & ([scriptblock]::Create((irm https://raw.githubusercontent.com/luwu-community/luwu-lsp/luwu/scripts/install/install.ps1))) --help

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$raw = "https://raw.githubusercontent.com/luwu-community/luwu-lsp/luwu/scripts/install"
$installerArgs = @($args)

$platform = switch ($env:PROCESSOR_ARCHITECTURE) {
    "AMD64" { "windows-x64" }
    "ARM64" { "windows-arm64" }
    default { throw "luwu-lsp installer: no luwu-lsp build for Windows on $env:PROCESSOR_ARCHITECTURE" }
}

# Whether `seal --version` at this path is newer than 0.8.0
function Test-SealVersion([string] $path) {
    $output = & $path --version 2>$null
    if (-not ($output -match '(\d+)\.(\d+)\.(\d+)')) {
        return $false
    }
    $major = [int] $Matches[1]
    $minor = [int] $Matches[2]
    $patch = [int] $Matches[3]
    return $major -gt 0 -or $minor -gt 8 -or ($minor -eq 8 -and $patch -gt 0)
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("luwu-lsp-install-" + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null

try {
    $seal = $null
    $onPath = Get-Command seal -ErrorAction SilentlyContinue
    if ($onPath -and (Test-SealVersion $onPath.Source)) {
        $seal = $onPath.Source
    }
    else {
        Write-Host "Downloading seal to run the installer..."
        $release = Invoke-RestMethod "https://api.github.com/repos/seal-runtime/seal/releases/latest"
        $asset = $release.assets | Where-Object { $_.name -like "*-$platform.zip" } | Select-Object -First 1
        if (-not $asset) {
            throw "luwu-lsp installer: couldn't find a seal release for $platform"
        }
        $zip = Join-Path $tmp "seal.zip"
        Invoke-WebRequest $asset.browser_download_url -OutFile $zip
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $archive = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            $entry = $archive.Entries | Where-Object { $_.FullName -eq "seal.exe" } | Select-Object -First 1
            $seal = Join-Path $tmp "seal.exe"
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $seal)
        }
        finally {
            $archive.Dispose()
        }
        # Lets the installer offer to keep it (`--keep-seal`)
        $env:LUWU_INSTALL_TEMPORARY_SEAL = "1"
    }

    $script = Join-Path $tmp "install.luau"
    Invoke-WebRequest "$raw/install.luau" -OutFile $script

    & $seal $script @installerArgs
    $status = $LASTEXITCODE
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

exit $status
