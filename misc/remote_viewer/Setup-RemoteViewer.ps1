<#
.SYNOPSIS
    Prepares a Windows desktop as a remote Chrome / Firefox viewer for the WebRTC test harnesses
    (misc/traffic_sim/limits.py --remote-viewer / --remote-firefox, misc/webrtc_impair/smoke_test.sh).

.DESCRIPTION
    1. Starts Chrome and/or Firefox on dedicated test profiles (under -Root) with remote debugging on
       127.0.0.1 and with background throttling turned off. Your own browser profiles are not touched.
    2. Exposes the debug ports on the LAN with netsh portproxy and opens them in the firewall for the
       controller only (the machine that runs the harness). These ports give full control of the test
       browsers, so never allow more than the controller. This step needs admin rights; the script asks
       for elevation for that step only, so the browsers run as you.
    3. Prints what the harness cares about (GPU, display mode, LAN address, the harness arguments).
    4. Keeps the machine and display awake while it runs and reports when a debug port stops answering.
       Ctrl+C ends that; the browsers keep running.
    5. While it runs, a control service on 127.0.0.1:9220 (LAN 19220, controller only) lets the harness
       restart a test browser itself, e.g. a Firefox whose one remote session a killed client left taken:
         GET  /status            which test browsers answer, with their process ids
         POST /restart/chrome    stop the test Chrome (only processes on its test profile) and start it again
         POST /restart/firefox   likewise for the test Firefox
       It can do nothing the debug ports can't already (they give full control of the test browsers), and it
       never touches a browser that isn't on a test profile. -NoControl leaves it out.

    -Remove deletes the port proxies and the firewall rule again. It leaves the browsers and profiles.

    Never close a test browser's last tab while a harness is connected: that ends the remote session.
    The harness only opens and closes its own windows.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Setup-RemoteViewer.ps1 -ControllerIP <lan-ip>

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Setup-RemoteViewer.ps1 -ControllerIP <lan-ip> -Browser Firefox

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\Setup-RemoteViewer.ps1 -Remove
#>
[CmdletBinding(DefaultParameterSetName = 'Setup')]
param(
    # IPv4 address of the machine that runs the harness; the only address let through the firewall.
    [Parameter(ParameterSetName = 'Setup', Mandatory = $true, Position = 0)]
    [ValidateScript({ $a = $null; [System.Net.IPAddress]::TryParse($_, [ref]$a) -and $a.AddressFamily -eq 'InterNetwork' })]
    [string]$ControllerIP,

    [Parameter(ParameterSetName = 'Remove', Mandatory = $true)]
    [switch]$Remove,

    [ValidateSet('Chrome', 'Firefox')]
    [string[]]$Browser = @('Chrome', 'Firefox'),

    # Test profiles live here (chrome\, firefox\).
    [string]$Root = 'C:\ometest',

    # Local debug ports (127.0.0.1) and the LAN ports the harness connects to.
    [int]$ChromePort = 9222,
    [int]$FirefoxPort = 9224,
    [int]$ChromeLanPort = 19222,
    [int]$FirefoxLanPort = 19224,
    [int]$ControlPort = 9220,
    [int]$ControlLanPort = 19220,

    # Set up and exit instead of keeping the machine awake, watching the ports and serving control requests.
    [Parameter(ParameterSetName = 'Setup')]
    [switch]$NoKeepAwake,

    # No control service (the harness then can't restart a test browser).
    [Parameter(ParameterSetName = 'Setup')]
    [switch]$NoControl,

    # Internal: the elevated network step. 'Add' or 'Remove', with "lan:local,..." port pairs.
    [Parameter(ParameterSetName = 'Elevated', Mandatory = $true)]
    [ValidateSet('Add', 'Remove')]
    [string]$ElevatedNetwork,
    [Parameter(ParameterSetName = 'Elevated')]
    [string]$ElevatedPorts,
    [Parameter(ParameterSetName = 'Elevated')]
    [string]$ElevatedIP
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$FirewallRuleName = 'ometest remote viewer'

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-Port([int]$Port, [string]$HostName = '127.0.0.1') {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $task = $client.ConnectAsync($HostName, $Port)
        return ($task.Wait(500) -and $client.Connected)
    } catch {
        return $false
    } finally {
        $client.Dispose()
    }
}

function Find-Exe([string]$Name, [string[]]$Candidates) {
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    foreach ($hive in 'HKLM:', 'HKCU:') {
        $key = "$hive\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\$Name"
        if (Test-Path $key) {
            $p = (Get-Item $key).GetValue('')
            if ($p -and (Test-Path -LiteralPath $p)) { return $p }
        }
    }
    return $null
}

# --- Network step (admin) ---------------------------------------------------------------------------

function Invoke-NetworkStep([string]$Action, [string]$PortPairs, [string]$IP) {
    $pairs = @($PortPairs -split ',' | Where-Object { $_ } | ForEach-Object {
        $lan, $local = $_ -split ':'
        [pscustomobject]@{ Lan = [int]$lan; Local = [int]$local }
    })

    # Windows PowerShell turns a native command's stderr into terminating errors under 'Stop'.
    $ErrorActionPreference = 'Continue'
    foreach ($p in $pairs) {
        # Delete only our own listeners: 'portproxy reset' would also wipe proxies that aren't ours.
        & netsh interface portproxy delete v4tov4 listenport=$($p.Lan) listenaddress=0.0.0.0 2>&1 | Out-Null
    }
    Get-NetFirewallRule -DisplayName $FirewallRuleName -ErrorAction SilentlyContinue | Remove-NetFirewallRule

    if ($Action -eq 'Remove') {
        Write-Host "Removed the port proxies ($(($pairs | ForEach-Object { $_.Lan }) -join ', ')) and the firewall rule '$FirewallRuleName'."
        return
    }

    # portproxy runs in the IP Helper service.
    $svc = Get-Service iphlpsvc
    if ($svc.StartType -eq 'Disabled') { Set-Service iphlpsvc -StartupType Manual }
    if ($svc.Status -ne 'Running') { Start-Service iphlpsvc }

    foreach ($p in $pairs) {
        $out = & netsh interface portproxy add v4tov4 listenport=$($p.Lan) listenaddress=0.0.0.0 connectport=$($p.Local) connectaddress=127.0.0.1 2>&1
        if ($LASTEXITCODE -ne 0) { throw "netsh portproxy add $($p.Lan) failed: $out" }
    }
    $ErrorActionPreference = 'Stop'
    New-NetFirewallRule -DisplayName $FirewallRuleName -Direction Inbound -Action Allow -Protocol TCP `
        -LocalPort ($pairs | ForEach-Object { $_.Lan }) -RemoteAddress $IP -Profile Any | Out-Null
    Write-Host "Port proxies $(($pairs | ForEach-Object { "$($_.Lan)->127.0.0.1:$($_.Local)" }) -join ', '); firewall lets only $IP in."
}

function Invoke-Elevated([string]$Action, [string]$PortPairs, [string]$IP) {
    if (Test-IsAdmin) {
        Invoke-NetworkStep $Action $PortPairs $IP
        return
    }
    Write-Host 'Asking for admin rights for the port proxy and firewall step...'
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"",
                 '-ElevatedNetwork', $Action, '-ElevatedPorts', $PortPairs)
    if ($IP) { $argList += @('-ElevatedIP', $IP) }
    try {
        $proc = Start-Process powershell.exe -Verb RunAs -Wait -PassThru -ArgumentList $argList
    } catch {
        throw 'The admin step was cancelled. Rerun and accept the UAC prompt, or run the script from an admin prompt.'
    }
    if ($proc.ExitCode -ne 0) { throw "The admin step failed (exit code $($proc.ExitCode))." }
}

if ($PSCmdlet.ParameterSetName -eq 'Elevated') {
    try {
        Invoke-NetworkStep $ElevatedNetwork $ElevatedPorts $ElevatedIP
        exit 0
    } catch {
        Write-Host "ERROR: $_" -ForegroundColor Red
        Read-Host 'Press Enter to close'
        exit 1
    }
}

$portPairs = @()
if ($Browser -contains 'Chrome') { $portPairs += "${ChromeLanPort}:${ChromePort}" }
if ($Browser -contains 'Firefox') { $portPairs += "${FirefoxLanPort}:${FirefoxPort}" }
# -Remove always removes the control port's proxy too (it may have been set up by an earlier run)
if ($Remove -or -not ($NoControl -or $NoKeepAwake)) { $portPairs += "${ControlLanPort}:${ControlPort}" }
$portPairs = $portPairs -join ','

if ($Remove) {
    Invoke-Elevated 'Remove' $portPairs $null
    Write-Host "The test browsers and their profiles under $Root are left as they are; close them yourself when no harness is connected."
    exit 0
}

# --- Browsers -----------------------------------------------------------------------------------------

function Test-ProfileInUse([string]$ExeName, [string]$ProfileDir) {
    $procs = Get-CimInstance Win32_Process -Filter "Name = '$ExeName'" -ErrorAction SilentlyContinue
    foreach ($p in @($procs)) {
        if ($p.CommandLine -and $p.CommandLine.IndexOf($ProfileDir, [StringComparison]::OrdinalIgnoreCase) -ge 0) { return $true }
    }
    return $false
}

function Start-TestChrome {
    $exe = Find-Exe 'chrome.exe' @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe")
    if (-not $exe) { Write-Warning 'Chrome not found; skipping it.'; return $false }

    if (Test-Port $ChromePort) { Write-Host "Chrome: already answering on 127.0.0.1:$ChromePort."; return $true }
    $profileDir = Join-Path $Root 'chrome'
    if (Test-ProfileInUse 'chrome.exe' $profileDir) {
        Write-Warning "Chrome: a Chrome on $profileDir is running without the debug port. Close that window and rerun."
        return $false
    }
    New-Item -ItemType Directory -Force -Path $profileDir | Out-Null

    # Remote debugging needs a non-default --user-data-dir. The last four flags stop Chrome from
    # throttling timers, rendering and video in a window that is unfocused or covered.
    $chromeArgs = @(
        "--remote-debugging-port=$ChromePort",
        "--user-data-dir=`"$profileDir`"",
        '--no-first-run',
        '--no-default-browser-check',
        '--autoplay-policy=no-user-gesture-required',
        '--disable-backgrounding-occluded-windows',
        '--disable-renderer-backgrounding',
        '--disable-background-timer-throttling',
        '--disable-features=CalculateNativeWinOcclusion',
        '--hide-crash-restore-bubble')  # after a restart by the control service
    Start-Process -FilePath $exe -ArgumentList $chromeArgs | Out-Null
    return (Wait-Port 'Chrome' $ChromePort)
}

function Start-TestFirefox {
    $exe = Find-Exe 'firefox.exe' @(
        "$env:ProgramFiles\Mozilla Firefox\firefox.exe",
        "${env:ProgramFiles(x86)}\Mozilla Firefox\firefox.exe")
    if (-not $exe) { Write-Warning 'Firefox not found; skipping it.'; return $false }

    if (Test-Port $FirefoxPort) { Write-Host "Firefox: already answering on 127.0.0.1:$FirefoxPort."; return $true }
    $profileDir = Join-Path $Root 'firefox'
    if (Test-ProfileInUse 'firefox.exe' $profileDir) {
        Write-Warning "Firefox: a Firefox on $profileDir is running without the debug port. Close that window and rerun."
        return $false
    }
    New-Item -ItemType Directory -Force -Path $profileDir | Out-Null

    # Autoplay without a click; no pausing of video decoding or slowed timers for a background tab;
    # no first-run and default-browser prompts in the way.
    $prefs = @(
        'user_pref("media.autoplay.default", 0);',
        'user_pref("media.suspend-bkgnd-video.enabled", false);',
        'user_pref("dom.timeout.enable_budget_timer_throttling", false);',
        'user_pref("dom.min_background_timeout_value", 4);',
        'user_pref("browser.tabs.remote.autostart", true);',
        'user_pref("browser.shell.checkDefaultBrowser", false);',
        'user_pref("browser.aboutwelcome.enabled", false);',
        'user_pref("datareporting.policy.dataSubmissionPolicyBypassNotification", true);',
        # A restart by the control service is a forced stop: start fresh, without a restore-session page
        'user_pref("browser.sessionstore.resume_from_crash", false);')
    Set-Content -LiteralPath (Join-Path $profileDir 'user.js') -Value $prefs -Encoding ASCII

    # WebDriver BiDi. The harness sends Host: 127.0.0.1:<port>, which Firefox accepts, so no
    # --remote-allow-hosts is needed.
    $ffArgs = @('-no-remote', '-profile', "`"$profileDir`"", "--remote-debugging-port=$FirefoxPort")
    Start-Process -FilePath $exe -ArgumentList $ffArgs | Out-Null
    return (Wait-Port 'Firefox' $FirefoxPort)
}

function Wait-Port([string]$Name, [int]$Port) {
    for ($i = 0; $i -lt 30; $i++) {
        if (Test-Port $Port) { Write-Host "${Name}: answering on 127.0.0.1:$Port."; return $true }
        Start-Sleep -Milliseconds 500
    }
    Write-Warning "${Name}: started, but nothing answers on 127.0.0.1:$Port after 15 s."
    return $false
}

function Get-TestBrowserProcesses([string]$Name) {
    $exe, $profileDir = if ($Name -eq 'Chrome') { 'chrome.exe', (Join-Path $Root 'chrome') } else { 'firefox.exe', (Join-Path $Root 'firefox') }
    return @(Get-CimInstance Win32_Process -Filter "Name = '$exe'" -ErrorAction SilentlyContinue | Where-Object {
        $_.CommandLine -and $_.CommandLine.IndexOf($profileDir, [StringComparison]::OrdinalIgnoreCase) -ge 0 })
}

function Restart-TestBrowser([string]$Name) {
    # Only processes on the test profile: the user's own Chrome / Firefox are never touched
    foreach ($p in Get-TestBrowserProcesses $Name) { Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue }
    $port = if ($Name -eq 'Chrome') { $ChromePort } else { $FirefoxPort }
    # @(): a function's one-element array comes back as the element itself, which has no Count
    for ($i = 0; $i -lt 40 -and (@(Get-TestBrowserProcesses $Name).Count -gt 0 -or (Test-Port $port)); $i++) { Start-Sleep -Milliseconds 250 }
    Write-Host "$(Get-Date -Format 'HH:mm:ss') control: restarting the test $Name"
    if ($Name -eq 'Chrome') { return (Start-TestChrome) } else { return (Start-TestFirefox) }
}

$ok = @{}
if ($Browser -contains 'Chrome') { $ok['Chrome'] = Start-TestChrome }
if ($Browser -contains 'Firefox') { $ok['Firefox'] = Start-TestFirefox }

Invoke-Elevated 'Add' $portPairs $ControllerIP

# --- Report -------------------------------------------------------------------------------------------

$lanIPs = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and $_.AddressState -eq 'Preferred' } |
    ForEach-Object { $_.IPAddress })
$lanIP = if ($lanIPs.Count -gt 0) { $lanIPs[0] } else { '<this-machine-ip>' }

Write-Host ''
Write-Host '=== Viewer machine ==='
foreach ($gpu in @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)) {
    $mode = if ($gpu.CurrentHorizontalResolution) {
        "$($gpu.CurrentHorizontalResolution)x$($gpu.CurrentVerticalResolution) @ $($gpu.CurrentRefreshRate) Hz"
    } else { 'no display' }
    Write-Host "GPU: $($gpu.Name) (driver $($gpu.DriverVersion)), $mode"
}
$battery = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
if ($battery -and $battery.BatteryStatus -eq 1) { Write-Warning 'Running on battery: power saving skews frame timing.' }
Write-Host "LAN address(es): $($lanIPs -join ', ')"
if ($ok.ContainsKey('Chrome') -and $ok['Chrome']) {
    try {
        $v = Invoke-RestMethod "http://127.0.0.1:$ChromePort/json/version" -TimeoutSec 3
        Write-Host "Chrome: $($v.Browser)"
    } catch { }
}

Write-Host ''
Write-Host '=== On the controller ==='
Write-Host '(Use the IP address: Chrome refuses a hostname in the Host header.)'
if ($ok.ContainsKey('Chrome')) {
    Write-Host "  limits.py --remote-viewer http://${lanIP}:$ChromeLanPort"
    Write-Host "  SMOKE_VIEWER_CDP=http://${lanIP}:$ChromeLanPort ./smoke_test.sh harness"
}
if ($ok.ContainsKey('Firefox')) {
    Write-Host "  limits.py --remote-firefox ws://${lanIP}:$FirefoxLanPort"
    Write-Host "  SMOKE_VIEWER_BIDI=ws://${lanIP}:$FirefoxLanPort ./smoke_test.sh harness"
}
if (-not ($NoControl -or $NoKeepAwake)) {
    Write-Host "  control: curl http://${lanIP}:$ControlLanPort/status ; curl -X POST http://${lanIP}:$ControlLanPort/restart/firefox"
}
Write-Host 'This machine must reach the controller''s test OME (signalling TCP, media UDP); on one LAN that just works.'

Write-Host ''
Write-Host '=== By hand, once ==='
if ($ok.ContainsKey('Chrome')) {
    Write-Host '  Chrome (test window): chrome://settings/performance -> turn off Energy saver and Memory saver.'
    Write-Host '  Check chrome://gpu: "Video Decode: Hardware accelerated" is what real viewers get.'
}
Write-Host '  Keep the test browsers on a monitor that stays on; do not minimise them.'
Write-Host '  Never close a test browser''s last tab while a harness is connected.'

if ($NoKeepAwake) { exit 0 }

# --- Keep awake and watch -----------------------------------------------------------------------------

Add-Type -Namespace OmeTest -Name Power -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern uint SetThreadExecutionState(uint esFlags);
'@
# ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED: no sleep, no display off, until this exits.
[OmeTest.Power]::SetThreadExecutionState([uint32]2147483651) | Out-Null

Write-Host ''
Write-Host 'Keeping this machine and its display awake. Ctrl+C stops that (the browsers keep running).'
$watch = @()
if ($ok.ContainsKey('Chrome')) { $watch += [pscustomobject]@{ Name = 'Chrome'; Port = $ChromePort; Up = [bool]$ok['Chrome'] } }
if ($ok.ContainsKey('Firefox')) { $watch += [pscustomobject]@{ Name = 'Firefox'; Port = $FirefoxPort; Up = [bool]$ok['Firefox'] } }

# --- Control service (a minimal HTTP/1.1 server; one request at a time is plenty) ----------------------

function Send-Json($Client, [int]$Code, $Obj) {
    $body = [Text.Encoding]::UTF8.GetBytes(($Obj | ConvertTo-Json -Compress -Depth 4))
    $reason = @{ 200 = 'OK'; 404 = 'Not Found'; 405 = 'Method Not Allowed'; 500 = 'Internal Server Error' }[$Code]
    $head = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 $Code $reason`r`nContent-Type: application/json`r`n" +
                                            "Content-Length: $($body.Length)`r`nConnection: close`r`n`r`n")
    $s = $Client.GetStream()
    $s.Write($head, 0, $head.Length)
    $s.Write($body, 0, $body.Length)
    $s.Flush()
}

function Get-Status {
    $out = [ordered]@{ host = $env:COMPUTERNAME; time = (Get-Date).ToString('o') }
    foreach ($w in $watch) {
        $out[$w.Name.ToLower()] = [ordered]@{ answering = (Test-Port $w.Port); port = $w.Port
                                             pids = @(Get-TestBrowserProcesses $w.Name | ForEach-Object { $_.ProcessId }) }
    }
    return $out
}

function Invoke-ControlRequest($Client) {
    $Client.ReceiveTimeout = 3000
    $reader = New-Object IO.StreamReader($Client.GetStream(), [Text.Encoding]::ASCII)
    $requestLine = $reader.ReadLine()
    while ($true) { $h = $reader.ReadLine(); if ($null -eq $h -or $h -eq '') { break } }
    if (-not $requestLine) { return }
    $method, $path = ($requestLine -split ' ')[0, 1]
    Write-Host "$(Get-Date -Format 'HH:mm:ss') control: $method $path"
    if ($path -eq '/status') {
        Send-Json $Client 200 (Get-Status)
        return
    }
    if ($path -match '^/restart/(chrome|firefox)$') {
        $name = (Get-Culture).TextInfo.ToTitleCase($Matches[1])
        if ($method -ne 'POST') { Send-Json $Client 405 @{ error = 'POST only' }; return }
        $w = $watch | Where-Object { $_.Name -eq $name }
        if (-not $w) { Send-Json $Client 404 @{ error = "$name is not set up on this machine" }; return }
        $up = Restart-TestBrowser $name
        $w.Up = $up
        Send-Json $Client 200 ([ordered]@{ restarted = $name; answering = $up; status = (Get-Status) })
        return
    }
    Send-Json $Client 404 @{ error = 'GET /status, POST /restart/chrome, POST /restart/firefox' }
}

$listener = $null
if (-not $NoControl) {
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $ControlPort)
    $listener.Start()
    Write-Host "Control service on 127.0.0.1:$ControlPort (LAN ${lanIP}:$ControlLanPort): GET /status, POST /restart/chrome|firefox"
}
$nextWatch = (Get-Date).AddSeconds(30)
try {
    while ($true) {
        if ($listener -and $listener.Pending()) {
            $client = $listener.AcceptTcpClient()
            try { Invoke-ControlRequest $client }
            catch {
                Write-Host "$(Get-Date -Format 'HH:mm:ss') control: $_" -ForegroundColor Red
                try { Send-Json $client 500 @{ error = "$_" } } catch { }
            }
            finally { $client.Close() }
            continue
        }
        Start-Sleep -Milliseconds 200
        if ((Get-Date) -lt $nextWatch) { continue }
        $nextWatch = (Get-Date).AddSeconds(30)
        foreach ($w in $watch) {
            $up = Test-Port $w.Port
            if ($up -ne $w.Up) {
                $state = if ($up) { 'answering again' } else { 'NOT answering any more (browser closed or crashed?)' }
                Write-Host "$(Get-Date -Format 'HH:mm:ss') $($w.Name) on 127.0.0.1:$($w.Port) $state"
                $w.Up = $up
            }
        }
    }
} finally {
    if ($listener) { $listener.Stop() }
}
