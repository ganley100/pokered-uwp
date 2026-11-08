<#
.SYNOPSIS
  Build signed, sideloadable UWP packages of Pokémon Red and/or Blue for Xbox
  Developer Mode (they also install on Windows 10/11 desktops).

.DESCRIPTION
  Needs Visual Studio 2022 with the "Universal Windows Platform development"
  workload (C++ UWP tools + a Windows 10/11 SDK), and the ROMs built first with
  `make red blue` in the repository root (pokered.gbc / pokeblue.gbc).

  The packages are signed with a self-signed certificate whose subject must
  match the Publisher in each Package.appxmanifest (CN=PokeredUWP). On first
  run one is created in uwp\cert\ and reused afterwards, so new builds install
  as updates over old ones. In CI, a fixed certificate can be supplied through
  the SIGNING_CERT_PFX_BASE64 and SIGNING_CERT_PASSWORD environment variables.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File uwp\build.ps1
  powershell -ExecutionPolicy Bypass -File uwp\build.ps1 -Game Blue -Version 1.0.3.0
#>
[CmdletBinding()]
param(
    [ValidateSet('Red', 'Blue', 'Both')]
    [string] $Game = 'Both',

    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',

    # Package version, e.g. 1.0.42.0. Defaults to the version in the manifest.
    [string] $Version,

    # Where the finished packages are copied. Defaults to uwp\dist.
    [string] $OutDir,

    # Password for the locally generated certificate in uwp\cert.
    [string] $CertPassword = 'pokered-uwp'
)

$ErrorActionPreference = 'Stop'
$uwp = $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $uwp 'dist' }
$publisher = 'CN=PokeredUWP'

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild `
            -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
        if ($found) { return $found }
    }
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'MSBuild was not found. Install Visual Studio 2022 with the Universal Windows Platform development workload.'
}

function Get-SigningCertificate {
    if ($env:SIGNING_CERT_PFX_BASE64) {
        $pfx = Join-Path ([IO.Path]::GetTempPath()) 'pokered-uwp-signing.pfx'
        [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:SIGNING_CERT_PFX_BASE64))
        return @{ Pfx = $pfx; Password = $env:SIGNING_CERT_PASSWORD }
    }

    $certDir = Join-Path $uwp 'cert'
    $pfx = Join-Path $certDir 'PokeredUWP.pfx'
    if (-not (Test-Path $pfx)) {
        Write-Host "Creating self-signed certificate $publisher in $certDir"
        New-Item -ItemType Directory -Force -Path $certDir | Out-Null
        $cert = New-SelfSignedCertificate -Type Custom -Subject $publisher `
            -KeyUsage DigitalSignature -FriendlyName 'pokered-uwp sideloading' `
            -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(10) `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
        $secure = ConvertTo-SecureString -String $CertPassword -Force -AsPlainText
        Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $secure | Out-Null
        Export-Certificate -Cert $cert -FilePath (Join-Path $certDir 'PokeredUWP.cer') | Out-Null
        Remove-Item -Path "Cert:\CurrentUser\My\$($cert.Thumbprint)"
    }
    return @{ Pfx = $pfx; Password = $CertPassword }
}

$msbuild = Find-MSBuild
$cert = Get-SigningCertificate
$games = if ($Game -eq 'Both') { @('Red', 'Blue') } else { @($Game) }

foreach ($g in $games) {
    $name = "Pokemon$g"
    $projectDir = Join-Path $uwp $name
    $project = Join-Path $projectDir "$name.vcxproj"
    $manifest = Join-Path $projectDir 'Package.appxmanifest'
    $packageDir = Join-Path $uwp "AppPackages\$name"
    $originalManifest = [IO.File]::ReadAllText($manifest)

    try {
        if ($Version) {
            if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Version must look like 1.2.3.0, not '$Version'." }
            $patched = $originalManifest -replace '(<Identity\b[^>]*\bVersion=")[^"]*(")', "`${1}$Version`${2}"
            [IO.File]::WriteAllText($manifest, $patched, (New-Object Text.UTF8Encoding $false))
        }

        if (Test-Path $packageDir) { Remove-Item -Recurse -Force $packageDir }

        Write-Host "Building $name ($Configuration|x64)"
        & $msbuild $project /nologo /m /verbosity:minimal `
            "/p:Configuration=$Configuration" '/p:Platform=x64' `
            '/p:GenerateAppxPackageOnBuild=true' '/p:AppxBundle=Never' `
            '/p:AppxPackageSigningEnabled=true' `
            "/p:PackageCertificateKeyFile=$($cert.Pfx)" `
            "/p:PackageCertificatePassword=$($cert.Password)"
        if ($LASTEXITCODE -ne 0) { throw "MSBuild failed for $name (exit code $LASTEXITCODE)." }
    }
    finally {
        if ($Version) { [IO.File]::WriteAllText($manifest, $originalManifest, (New-Object Text.UTF8Encoding $false)) }
    }

    $package = Get-ChildItem -Path $packageDir -Recurse -Include '*.msix', '*.appx' |
        Where-Object { $_.FullName -notmatch '\\Dependencies\\' } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $package) { throw "No package was produced under $packageDir." }

    $dest = Join-Path $OutDir $name
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -Recurse -Path (Join-Path $package.DirectoryName '*') -Destination $dest
    Write-Host "$name -> $dest"
}
