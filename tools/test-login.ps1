# Exercises real Login Flow v2 consent, persistence and credential revocation over Go/C++ IPC.
# Inputs: compiled daemon and approved NEXTCLOUD_TEST_* environment; matching Qt DLL/plugin paths.
# Side effects: one isolated profile, one or two issued app passwords and one disposable web session; cleans owned resources.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable, [switch]$SyncFolders, [switch]$Reauthenticate)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not [IO.Path]::IsPathFullyQualified($Executable) -or -not (Test-Path -LiteralPath $Executable)) { throw 'Existing absolute daemon path required.' }
if (-not $env:NEXTCLOUD_TEST_USER -or -not $env:NEXTCLOUD_TEST_PASSWORD) { throw 'Approved test credentials required in the environment.' }
$run = [Guid]::NewGuid().ToString('N')
$probe = Join-Path $root "build/login-probe-$run.exe"
$savedExecutable = $env:SYNCD_TEST_EXE
$savedState = $env:SYNCD_LOGIN_FIXTURE_STATE
$savedFolders = $env:SYNCD_TEST_FOLDERS
$savedReauth = $env:SYNCD_TEST_REAUTH
if ($Reauthenticate -and -not $SyncFolders) { throw 'Renewal acceptance requires the owned folder fixture.' }
& go build -o $probe ./tools/test-login
if ($LASTEXITCODE -ne 0) { throw 'Login fixture build failed.' }
$env:SYNCD_TEST_EXE = $Executable
$env:SYNCD_LOGIN_FIXTURE_STATE = Join-Path $root "build/login-probe-$run.json"
$env:SYNCD_TEST_FOLDERS = if ($SyncFolders) { '1' } else { '' }
$env:SYNCD_TEST_REAUTH = if ($Reauthenticate) { '1' } else { '' }
$origin = [Uri]$env:NEXTCLOUD_TEST_URL
$handler = [Net.Http.HttpClientHandler]::new()
$handler.AllowAutoRedirect = $false
$handler.CookieContainer = [Net.CookieContainer]::new()
$client = [Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(30)
$client.DefaultRequestHeaders.Add('Origin', $origin.GetLeftPart([UriPartial]::Authority))
$client.DefaultRequestHeaders.Add('Accept', 'text/html')
$stage = 'start'
$loggedIn = $false
$page = $null

# Send keeps the disposable consent session and all CSRF values in memory, with same-origin redirects only.
function Send([Uri]$Uri, [string]$Method = 'GET', [hashtable]$Form = @{}) {
    for ($i=0; $i -lt 10; $i++) {
        if ($Uri.Scheme -ne 'https' -or $Uri.Authority -ne $origin.Authority) { throw 'Consent redirected outside the approved origin.' }
        $req = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method),$Uri)
        $res = $null
        try {
            if ($Method -eq 'POST') {
                $pairs = [Collections.Generic.Dictionary[string,string]]::new()
                foreach ($key in $Form.Keys) { $pairs.Add($key,[string]$Form[$key]) }
                $req.Content = [Net.Http.FormUrlEncodedContent]::new($pairs)
            }
            $res = $client.SendAsync($req).GetAwaiter().GetResult()
            $code = [int]$res.StatusCode
            if ($code -ge 300 -and $code -lt 400) {
                $Uri = [Uri]::new($Uri,$res.Headers.Location)
                if ($code -in 301,302,303) { $Method = 'GET'; $Form = @{} }
                continue
            }
            if ($code -ne 200) { throw "Consent request failed at $stage (HTTP $code)." }
            return @{ Content = $res.Content.ReadAsStringAsync().GetAwaiter().GetResult(); Uri = $Uri }
        } finally { if ($res) { $res.Dispose() }; $req.Dispose() }
    }
    throw 'Too many consent redirects.'
}

# Initial decodes one server-rendered initial-state value without logging its contents.
function Initial([string]$HTML,[string]$Name) {
    $tag = [regex]::Match($HTML,'<input\b[^>]*\bid="initial-state-core-' + [regex]::Escape($Name) + '"[^>]*>')
    if (-not $tag.Success) { throw "Expected consent state missing at $stage." }
    $value = [regex]::Match($tag.Value,'\bvalue="([^"]+)"').Groups[1].Value
    return [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([Net.WebUtility]::HtmlDecode($value))) | ConvertFrom-Json
}

# CSRF reads the session's ordinary request token, keeping it out of output and files.
function CSRF([string]$HTML) {
    $token = [regex]::Match($HTML,'data-requesttoken="([^"]+)"').Groups[1].Value
    if (-not $token) { throw "CSRF token missing at $stage." }
    return [Net.WebUtility]::HtmlDecode($token)
}

function Approve([string]$Url) {
    $stage = 'flow landing'; $page = Send ([Uri]$url)
    if ($page.Content -notmatch 'id="initial-state-core-loginFlowGrant"') {
        $auth = Initial $page.Content 'loginFlowAuth'
        $stage = 'login page'; $page = Send ([Uri]::new($origin,('login?redirect_url=' + [Uri]::EscapeDataString(([Uri]$auth.loginRedirectUrl).PathAndQuery))))
        if ($page.Content -notmatch 'id="initial-state-core-loginFlowGrant"') {
            $stage = 'account authentication'
            $page = Send ([Uri]::new($origin,'login')) 'POST' @{
                user=$env:NEXTCLOUD_TEST_USER; password=$env:NEXTCLOUD_TEST_PASSWORD
                requesttoken=(CSRF $page.Content); redirect_url=([Uri]$auth.loginRedirectUrl).PathAndQuery
            }
        }
    }
    $grant = Initial $page.Content 'loginFlowGrant'
    $script:loggedIn = $true
    if ($grant.userId -ne $env:NEXTCLOUD_TEST_USER) { throw 'Consent account does not match the approved fixture account.' }
    $stage = 'app-password grant'
    $page = Send ([Uri]$grant.actionUrl) 'POST' @{stateToken=$grant.stateToken;requesttoken=(CSRF $page.Content)}
    if ((Initial $page.Content 'loginFlowState') -ne 'done') { throw 'Consent did not complete.' }
    return $page
}

try {
    $url = & $probe start
    if ($LASTEXITCODE -ne 0) { throw 'Daemon login start failed.' }
    $page = Approve $url
    $stage = 'daemon completion and cleanup'
    if ($Reauthenticate) {
        $info = [Diagnostics.ProcessStartInfo]::new($probe)
        $info.ArgumentList.Add('finish')
        $info.UseShellExecute = $false
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $child = [Diagnostics.Process]::new()
        $child.StartInfo = $info
        $started = $false
        try {
            if (-not $child.Start()) { throw 'Renewal fixture launch failed.' }
            $started = $true
            $errors = $child.StandardError.ReadToEndAsync()
            while ($null -ne ($line = $child.StandardOutput.ReadLine())) {
                if ($line.StartsWith('https://')) { $page = Approve $line }
                else { Write-Output $line }
            }
            $child.WaitForExit()
            $diagnostic = $errors.GetAwaiter().GetResult()
            if ($child.ExitCode -ne 0) {
                $evidence = [regex]::Match($diagnostic, '(?m)^SYNCD_DIAGNOSTICS (\{[^\r\n]+\})\r?$')
                if ($evidence.Success) {
                    $data = $evidence.Groups[1].Value | ConvertFrom-Json
                    $safe = @{ AllPaused = $data.AllPaused; Folders = @($data.Folders | Select-Object State, ReasonCode, UserPaused, Blockers, ActiveRequests, Requests, ActiveUploads, ActiveDownloads) }
                    Write-Output ('Owned fixture diagnostic counters: ' + ($safe | ConvertTo-Json -Depth 5 -Compress))
                }
                $readError = [regex]::Match($diagnostic, '(?m)^SYNCD_DIAGNOSTICS_ERROR (syncd: [A-Za-z0-9 _-]+|json: [A-Za-z0-9 ,._:-]+)\r?$').Groups[1].Value.Trim()
                if ($readError) { Write-Output ('Owned fixture diagnostics unavailable: ' + $readError) }
                $summary = [regex]::Match($diagnostic, '(?m)^panic: [A-Za-z0-9 _,/():.\[\]-]+\r?$').Value.Trim()
                throw "Daemon renewal acceptance failed (exit $($child.ExitCode)). $summary"
            }
        } finally {
            if ($started -and -not $child.HasExited) { $child.Kill(); $child.WaitForExit() }
            $child.Dispose()
        }
    } else {
        & $probe finish
        if ($LASTEXITCODE -ne 0) { throw 'Daemon account completion or cleanup failed.' }
    }
} finally {
    $cleanupFailed = $false
    if (Test-Path -LiteralPath $env:SYNCD_LOGIN_FIXTURE_STATE) {
        & $probe abort
        if ($LASTEXITCODE -ne 0) { $cleanupFailed = $true }
    }
    if ($loggedIn -and $page) {
        try { $null = Send ([Uri]::new($origin,('logout?requesttoken=' + [Uri]::EscapeDataString((CSRF $page.Content))))) }
        catch { $cleanupFailed = $true; Write-Warning 'Disposable web-session logout was not confirmed.' }
    }
    $client.Dispose(); $handler.Dispose()
    Remove-Item -LiteralPath $probe -Force
    $env:SYNCD_TEST_EXE = $savedExecutable
    $env:SYNCD_LOGIN_FIXTURE_STATE = $savedState
    $env:SYNCD_TEST_FOLDERS = $savedFolders
    $env:SYNCD_TEST_REAUTH = $savedReauth
    if ($cleanupFailed) { throw 'Owned login fixture cleanup needs attention.' }
}
