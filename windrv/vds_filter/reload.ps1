# SPDX-License-Identifier: MIT

$ErrorActionPreference = "Stop"

# Bluetooth Classic HID service UUID 0x1124 in Bluetooth base UUID form.
# Windows Bluetooth HID instance IDs embed this UUID before VID/PID fields.
$BtHidServiceGuid = "00001124-0000-1000-8000-00805F9B34FB"

Get-PnpDevice |
    Where-Object {
        $_.InstanceId -like "HID\{$BtHidServiceGuid}_VID&0002054C_PID&0CE6*" -or
        $_.InstanceId -like "HID\{$BtHidServiceGuid}_VID&0002054C_PID&0DF2*"
    } |
    ForEach-Object {
        Write-Output "Restarting Bluetooth HID device $($_.InstanceId)"
        pnputil /restart-device $_.InstanceId | Out-Host
        if ($LASTEXITCODE -ne 0) {
            $RestartExitCode = $LASTEXITCODE
            Write-Warning "pnputil restart-device failed with exit code $RestartExitCode"
            $global:LASTEXITCODE = 0
            Write-Warning "Removing stale Bluetooth HID device $($_.InstanceId) so it can re-enumerate with the current vds_filter package."
            pnputil /remove-device $_.InstanceId | Out-Host
            if ($LASTEXITCODE -eq 3010) {
                Write-Warning "pnputil requested a reboot while removing stale Bluetooth HID device $($_.InstanceId)."
                $global:LASTEXITCODE = 0
            } elseif ($LASTEXITCODE -ne 0) {
                Write-Warning "pnputil remove-device failed with exit code $LASTEXITCODE"
                $global:LASTEXITCODE = 0
            }
            pnputil /scan-devices | Out-Host
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "pnputil scan-devices failed with exit code $LASTEXITCODE"
                $global:LASTEXITCODE = 0
            }
        }
    }
