# 通过 UART0 把 .nes 文件上传到 ESP32-S31 开发板的 SD 卡。
# 设备端固件在没有 ROM 时会打印 "READY-FOR-UART-ROM" 并进入接收模式。
#
# 用法（在 references/esp32-nesemu 下）：
#   powershell -ExecutionPolicy Bypass -File tools/uart_send_rom.ps1 -Port COM3 roms/English_Word_Blaster.nes
#   powershell -ExecutionPolicy Bypass -File tools/uart_send_rom.ps1 -Port COM3 roms/English_Word_Blaster.nes roms/contra2.nes

param(
    [string]$Port = "COM3",
    [int]$Baud = 115200,
    [int]$WaitReadySec = 60,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

if (-not $Files -or $Files.Count -eq 0) {
    Write-Error "请指定至少一个 .nes 文件路径"
    exit 1
}

$script:sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$script:sp.Encoding = [System.Text.Encoding]::ASCII
$script:sp.ReadTimeout = 1000
$script:sp.WriteTimeout = 10000
$script:sp.DtrEnable = $false
$script:sp.RtsEnable = $false
$script:sp.Open()
Start-Sleep -Milliseconds 200
$script:sp.DiscardInBuffer()

$script:pending = ""

# 读取串口行；找到含 $needle 的行就返回该行，否则一直读到超时返回 $null。
function Wait-Line {
    param([string]$Needle, [int]$TimeoutSec)

    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
        try {
            $s = $script:sp.ReadExisting()
            if ($s.Length -gt 0) { $script:pending += $s }
        } catch {
            # 无数据时忽略
        }

        $idx = $script:pending.IndexOf("`n")
        while ($idx -ge 0) {
            $line = $script:pending.Substring(0, $idx).TrimEnd("`r")
            $script:pending = $script:pending.Substring($idx + 1)
            Write-Host "  | $line"
            if ($Needle -and $line.Contains($Needle)) { return $line }
            $idx = $script:pending.IndexOf("`n")
        }

        if (-not $Needle) { return $null }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

Write-Host "等待设备进入 UART ROM 接收模式（$Port @ $Baud）..."
$ready = Wait-Line "READY-FOR-UART-ROM" $WaitReadySec
if (-not $ready) {
    Write-Host "未在 ${WaitReadySec}s 内收到 READY-FOR-UART-ROM。" -ForegroundColor Yellow
    Write-Host "可能原因：SD 卡上已有 ROM（固件直接进游戏了），或板子未复位/未插入 SD 卡。" -ForegroundColor Yellow
    Write-Host "如需上传，请先取出/清空 SD 卡上的 .nes，按 RESET 后重试。"
    $script:sp.Close()
    exit 2
}

$sentCount = 0
foreach ($f in $Files) {
    if (-not (Test-Path $f)) {
        Write-Warning "跳过（文件不存在）: $f"
        continue
    }
    $full = (Resolve-Path $f).Path
    $name = Split-Path $full -Leaf
    $size = (Get-Item $full).Length

    Write-Host "==> $name ($size 字节)"
    $script:sp.WriteLine("ROM $name $size")

    $r = Wait-Line "READY " 15
    if (-not $r) {
        Write-Error "设备未回复 READY，中止"
        break
    }

    $fs = [System.IO.File]::OpenRead($full)
    $chunk = New-Object byte[] 4096
    $sent = 0
    try {
        while (($n = $fs.Read($chunk, 0, 4096)) -gt 0) {
            $script:sp.Write($chunk, 0, $n)
            $sent += $n
            Write-Progress -Activity "发送 $name" -Status "$sent / $size" -PercentComplete ([int]($sent * 100 / $size))
        }
    } finally {
        $fs.Close()
        Write-Progress -Activity "发送 $name" -Completed
    }

    $ok = Wait-Line "OK $name" 120
    if ($ok) {
        Write-Host "  <- $ok"
        $sentCount++
    } else {
        Write-Error "未收到 $name 的 OK 确认，中止"
        break
    }
}

if ($sentCount -gt 0) {
    Write-Host "发送 DONE，设备将重启并自动加载第一个 English ROM..."
    $script:sp.WriteLine("DONE")
    Wait-Line "REBOOT" 15 | Out-Null
    Write-Host "完成：已上传 $sentCount 个文件。"
} else {
    Write-Host "没有文件成功上传。"
}

$script:sp.Close()
