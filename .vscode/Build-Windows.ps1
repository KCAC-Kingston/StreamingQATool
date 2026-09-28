[CmdletBinding()]
param(
    [string] $ObsPath = '',
    [switch] $DeployOnly,
    [switch] $SkipDeployPrompt
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$DllPath = Join-Path $ProjectRoot 'build_x64\RelWithDebInfo\StreamingQATool.dll'

try {
    Set-Location -LiteralPath $ProjectRoot

    if (-not $DeployOnly) {
        & cmake --build --preset windows-x64 --parallel
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        if ($SkipDeployPrompt) { exit 0 }

        Add-Type -AssemblyName System.Windows.Forms
        $Choice = [System.Windows.Forms.MessageBox]::Show(
            'Build succeeded. Deploy StreamingQATool? OBS will be stopped and restarted.',
            'Deploy StreamingQATool',
            [System.Windows.Forms.MessageBoxButtons]::YesNo,
            [System.Windows.Forms.MessageBoxIcon]::Question,
            [System.Windows.Forms.MessageBoxDefaultButton]::Button2
        )
        if ($Choice -ne [System.Windows.Forms.DialogResult]::Yes) {
            Write-Host 'Build complete. Deployment skipped.'
            exit 0
        }
    }

    if (-not $ObsPath) {
        $RunningObs = Get-Process obs64 -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($RunningObs -and $RunningObs.Path) {
            $ObsPath = Split-Path (Split-Path (Split-Path $RunningObs.Path -Parent) -Parent) -Parent
        } else {
            foreach ($Key in @('HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio')) {
                if (Test-Path $Key) {
                    $ObsPath = (Get-Item $Key).GetValue('')
                    if ($ObsPath) { break }
                }
            }
        }
        if (-not $ObsPath) { $ObsPath = Join-Path $env:ProgramFiles 'obs-studio' }
    }

    $ObsExe = Join-Path $ObsPath 'bin\64bit\obs64.exe'
    $PluginDirectory = Join-Path $ObsPath 'obs-plugins\64bit'
    if (-not (Test-Path -LiteralPath $DllPath -PathType Leaf)) { throw "Built DLL not found: $DllPath" }
    if (-not (Test-Path -LiteralPath $ObsExe -PathType Leaf)) { throw "OBS executable not found: $ObsExe" }
    if (-not (Test-Path -LiteralPath $PluginDirectory -PathType Container)) { throw "OBS plugin folder not found: $PluginDirectory" }

    $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $Principal = New-Object Security.Principal.WindowsPrincipal($Identity)
    $IsAdmin = $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

    if (-not $IsAdmin) {
        # Elevate only the stop/copy step; launch OBS from this original process.
        $Arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -DeployOnly -ObsPath "{1}"' -f $PSCommandPath, $ObsPath
        $Deployment = Start-Process -FilePath "$PSHOME\powershell.exe" -ArgumentList $Arguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        if ($Deployment.ExitCode -ne 0) { throw "Deployment failed (exit code $($Deployment.ExitCode)). OBS was not restarted." }
    } else {
        if (Get-Process obs64 -ErrorAction SilentlyContinue) {
            & taskkill.exe /IM obs64.exe /F
            if ($LASTEXITCODE -ne 0) { throw 'Could not stop OBS. DLL was not copied.' }
            Get-Process obs64 -ErrorAction SilentlyContinue | Wait-Process -Timeout 15
        }
        Copy-Item -LiteralPath $DllPath -Destination (Join-Path $PluginDirectory 'StreamingQATool.dll') -Force
    }

    if (-not $DeployOnly) {
        Start-Process -FilePath $ObsExe -WorkingDirectory (Split-Path $ObsExe -Parent)
        Write-Host "Deployed StreamingQATool.dll to $PluginDirectory and started OBS."
    }
    exit 0
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
