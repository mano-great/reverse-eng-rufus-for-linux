# Build the test payload as a gzipped newc cpio archive.
#
# It is appended to Alpine's netboot initramfs, so the kernel unpacks the
# Alpine files first and then overwrites /init with ours.
param(
    [Parameter(Mandatory = $true)][string]$Init,
    [Parameter(Mandatory = $true)][string]$Binary,
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Output
)

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$script:ino = 1

function Write-CpioEntry {
    param(
        [System.IO.Stream]$Stream,
        [string]$Name,
        [int]$Mode,
        [byte[]]$Data,
        [int]$RdevMajor = 0,
        [int]$RdevMinor = 0,
        [int]$Nlink = 1
    )

    if ($null -eq $Data) { $Data = New-Object byte[] 0 }
    $nameBytes = [Text.Encoding]::ASCII.GetBytes($Name + [char]0)
    $header = "070701" +
        ("{0:X8}" -f $script:ino) +
        ("{0:X8}" -f $Mode) +
        ("{0:X8}" -f 0) +
        ("{0:X8}" -f 0) +
        ("{0:X8}" -f $Nlink) +
        ("{0:X8}" -f 0) +
        ("{0:X8}" -f $Data.Length) +
        ("{0:X8}" -f 0) +
        ("{0:X8}" -f 0) +
        ("{0:X8}" -f $RdevMajor) +
        ("{0:X8}" -f $RdevMinor) +
        ("{0:X8}" -f $nameBytes.Length) +
        ("{0:X8}" -f 0)
    $script:ino++

    $hb = [Text.Encoding]::ASCII.GetBytes($header)
    $Stream.Write($hb, 0, $hb.Length)
    $Stream.Write($nameBytes, 0, $nameBytes.Length)

    $pad = (4 - (($hb.Length + $nameBytes.Length) % 4)) % 4
    if ($pad -gt 0) {
        $z = New-Object byte[] $pad
        $Stream.Write($z, 0, $pad)
    }
    if ($Data.Length -gt 0) {
        $Stream.Write($Data, 0, $Data.Length)
    }
    $pad2 = (4 - ($Data.Length % 4)) % 4
    if ($pad2 -gt 0) {
        $z = New-Object byte[] $pad2
        $Stream.Write($z, 0, $pad2)
    }
}

$cpioPath = "$Output.cpio"
$fs = [System.IO.File]::Create($cpioPath)

# Directories must precede their children.
Write-CpioEntry -Stream $fs -Name "." -Mode 0x41ED -Data $null -Nlink 2
Write-CpioEntry -Stream $fs -Name "dev" -Mode 0x41ED -Data $null -Nlink 2

# Console/null, in case the base initramfs lacks them (stdio for PID 1).
Write-CpioEntry -Stream $fs -Name "dev/console" -Mode 0x21A4 -Data $null -RdevMajor 5 -RdevMinor 1
Write-CpioEntry -Stream $fs -Name "dev/null" -Mode 0x21B6 -Data $null -RdevMajor 1 -RdevMinor 3

Write-CpioEntry -Stream $fs -Name "rufus-linux" -Mode 0x81ED `
    -Data ([System.IO.File]::ReadAllBytes($Binary))
Write-CpioEntry -Stream $fs -Name "test.iso" -Mode 0x81A4 `
    -Data ([System.IO.File]::ReadAllBytes($Image))
Write-CpioEntry -Stream $fs -Name "test-init" -Mode 0x81ED `
    -Data ([System.IO.File]::ReadAllBytes($Init))

# Trailer
Write-CpioEntry -Stream $fs -Name "TRAILER!!!" -Mode 0 -Data $null
$fs.Close()

$in = [System.IO.File]::OpenRead($cpioPath)
$out = [System.IO.File]::Create($Output)
$gz = New-Object System.IO.Compression.GZipStream($out, [System.IO.Compression.CompressionLevel]::Fastest, $true)
$in.CopyTo($gz)
$gz.Dispose()
$out.Dispose()
$in.Dispose()
Remove-Item -Force $cpioPath

"payload: $Output ($((Get-Item $Output).Length) bytes)"
