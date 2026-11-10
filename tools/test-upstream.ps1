# Tests S01/S02/S03 through the real upstream CLI using disposable fixture data.
# Inputs: executable path; NEXTCLOUD_TEST_URL, NEXTCLOUD_TEST_USER and NEXTCLOUD_TEST_PASSWORD.
# Side effects: creates and removes a unique remote collection, local profile and CLI cache.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable, [switch]$EngineAdapter)
$ErrorActionPreference = 'Stop'
if (-not [IO.Path]::IsPathFullyQualified($Executable) -or -not (Test-Path $Executable -PathType Leaf)) {
    throw 'Provide an existing absolute nextcloudcmd executable path.'
}
$server = [Uri]$env:NEXTCLOUD_TEST_URL
if (-not $server.IsAbsoluteUri -or $server.Scheme -ne 'https' -or $server.UserInfo -or $server.Query -or $server.Fragment) {
    throw 'An HTTPS test base URL without userinfo, query or fragment is required.'
}
if (-not $env:NEXTCLOUD_TEST_USER -or -not $env:NEXTCLOUD_TEST_PASSWORD) { throw 'Test credentials are required in the environment.' }
$base = $server.AbsoluteUri.TrimEnd('/') + '/'
$name = 'nextcloud-mygo-m0-' + [Guid]::NewGuid().ToString('N')
$local = Join-Path ([IO.Path]::GetTempPath()) $name
$cache = Join-Path $env:LOCALAPPDATA $name
$data = Join-Path $local 'files'
$profile = Join-Path $local 'profile'
$handler = [Net.Http.HttpClientHandler]::new()
$handler.AllowAutoRedirect = $false
$client = [Net.Http.HttpClient]::new($handler)
$client.Timeout = [TimeSpan]::FromSeconds(30)
$auth = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($env:NEXTCLOUD_TEST_USER + ':' + $env:NEXTCLOUD_TEST_PASSWORD))
$client.DefaultRequestHeaders.Authorization = [Net.Http.Headers.AuthenticationHeaderValue]::new('Basic', $auth)
$client.DefaultRequestHeaders.Add('OCS-APIRequest', 'true')
$created = $false
$cleanupFailed = $false
$failure = $false
$stage = 'user lookup'

# Invoke-TestDav performs one fixture request without redirects or raw response logging.
function Invoke-TestDav([string]$Method, [string]$Relative, [byte[]]$Content = @(), [int[]]$Expected = @(200)) {
    $request = [Net.Http.HttpRequestMessage]::new([Net.Http.HttpMethod]::new($Method), [Uri]::new($base + $Relative))
    $response = $null
    try {
        if ($Method -eq 'PUT') { $request.Content = [Net.Http.ByteArrayContent]::new($Content) }
        $response = $client.SendAsync($request).GetAwaiter().GetResult()
        if ([int]$response.StatusCode -notin $Expected) { throw "Unexpected fixture HTTP status: $([int]$response.StatusCode)." }
        return ,$response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
    } finally {
        if ($response) { $response.Dispose() }
        $request.Dispose()
    }
}

# Invoke-TestCycle runs a bounded engine cycle; credentials stay in its environment.
# Returns: request counts parsed in memory, with raw process output never emitted.
function Invoke-TestCycle {
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $local "$name.exe"))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $arguments = if ($EngineAdapter) {
        @($data, "/$name", (Join-Path $profile 'journal.sqlite'), (Join-Path $PSScriptRoot '../third_party/nextcloud/sync-exclude.lst'))
    } else { @('--non-interactive', '--max-sync-retries', '0', '--confdir', $profile, '--path', "/$name", $data, $base) }
    foreach ($argument in $arguments) {
        $start.ArgumentList.Add($argument)
    }
    $start.Environment['NC_USER'] = $env:NEXTCLOUD_TEST_USER
    $start.Environment['NC_PASSWORD'] = $env:NEXTCLOUD_TEST_PASSWORD
    $start.Environment['QT_LOGGING_RULES'] = '*.debug=false;*.info=false;*.warning=false;*.critical=false;nextcloud.sync.accessmanager.info=true'
    $start.Environment['QT_FORCE_STDERR_LOGGING'] = '1'
    $start.Environment['PATH'] = (Split-Path $Executable -Parent) + ';' + $env:PATH
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $started = $false
    try {
        $started = $process.Start()
        if (-not $started) { throw 'Engine did not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(120000)) {
            Write-Information 'Engine fixture exceeded its process deadline.' -InformationAction Continue
            $process.Kill($true)
            $process.WaitForExit()
        }
        $output = $stdout.GetAwaiter().GetResult() + "`n" + $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            Write-Information "Engine fixture exit code: $($process.ExitCode)." -InformationAction Continue
            if ($EngineAdapter) {
                $output -split "`n" | Where-Object { $_ -match '^SYNCD_FIXTURE (success=[01] reason=[A-Z_]*|idle=[01] active=[0-9]+ alive=[0-9]+ managers=[0-9]+ accounts=[0-9]+|polls=[0-9]+ uploads=[0-9]+ downloads=[0-9]+)\r?$' } | ForEach-Object {
                    Write-Information $_ -InformationAction Continue
                }
            }
            throw "Engine exit code: $($process.ExitCode)."
        }
        if ($EngineAdapter) {
            $line = @($output -split "`n" | Where-Object { $_.StartsWith('{"') })[-1]
            return ($line | ConvertFrom-Json)
        }
        $counts = @{ Requests = 0; Discovery = 0; Downloads = 0; Writes = 0 }
        foreach ($line in ($output -split "`n")) {
            if (-not $line.Contains('nextcloud.sync.accessmanager') -or
                $line -notmatch '(?<operation>HEAD|GET|PUT|POST|DELETE|CUSTOM|UNKNOWN)"?\s+"?(?<verb>[A-Z]*)"?\s+"?(?<url>https://[^"\s]+)"?\s+has X-Request-ID') { continue }
            $counts.Requests++
            $method = if ($Matches.operation -eq 'CUSTOM') { $Matches.verb } else { $Matches.operation }
            $url = [Uri]$Matches.url
            if ($method -eq 'PROPFIND') { $counts.Discovery++ }
            if ($method -eq 'GET' -and $url.AbsolutePath.Contains('/remote.php/dav/')) { $counts.Downloads++ }
            if ($method -notin @('GET', 'HEAD', 'PROPFIND', 'REPORT', 'OPTIONS')) { $counts.Writes++ }
        }
        return [pscustomobject]$counts
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill($true); $process.WaitForExit() }
        $process.Dispose()
    }
}

# Assert-TestContent checks exact fixture bytes without reporting their contents.
function Assert-TestContent([byte[]]$Actual, [string]$Expected) {
    if ([Text.Encoding]::UTF8.GetString($Actual) -cne $Expected) { throw 'Fixture content mismatch.' }
}

try {
    $user = [Text.Encoding]::UTF8.GetString((Invoke-TestDav 'GET' 'ocs/v2.php/cloud/user?format=json')) | ConvertFrom-Json
    if (-not $user.ocs.data.id) { throw 'Missing DAV user ID.' }
    $remote = 'remote.php/dav/files/' + [Uri]::EscapeDataString($user.ocs.data.id) + '/' + $name
    $stage = 'fixture creation'
    $null = Invoke-TestDav 'MKCOL' $remote -Expected @(201)
    $created = $true
    New-Item -ItemType Directory $data, $profile | Out-Null
    Copy-Item $Executable (Join-Path $local "$name.exe")
    [IO.File]::WriteAllText((Join-Path $data 'upload.txt'), 'upload-v1')
    $null = Invoke-TestDav 'PUT' "$remote/download.txt" ([Text.Encoding]::UTF8.GetBytes('download-v1')) @(201)
    $stage = 'initial cycle'
    $initial = Invoke-TestCycle
    $stage = 'initial request observation positive control'
    if ($initial.Downloads -lt 1 -or $initial.Writes -lt 1) {
        Write-Information "Request counters: total=$($initial.Requests), discovery=$($initial.Discovery), downloads=$($initial.Downloads), writes=$($initial.Writes)." -InformationAction Continue
        throw 'Initial content requests were not observed.'
    }
    Assert-TestContent (Invoke-TestDav 'GET' "$remote/upload.txt") 'upload-v1'
    Assert-TestContent ([IO.File]::ReadAllBytes((Join-Path $data 'download.txt'))) 'download-v1'
    $journalDirectory = if ($EngineAdapter) { $profile } else { $data }
    $journalFilter = if ($EngineAdapter) { 'journal.sqlite' } else { '.sync_*.db' }
    $journals = @(Get-ChildItem $journalDirectory -Force -Filter $journalFilter)
    if ($journals.Count -ne 1) { throw 'Expected one journal.' }
    $journalPath = $journals[0].FullName
    $stage = 'unchanged cycle'
    $unchanged = Invoke-TestCycle
    $stage = 'unchanged request observation'
    if ($unchanged.Discovery -lt 1 -or $unchanged.Downloads -ne 0 -or $unchanged.Writes -ne 0) {
        throw 'Unchanged cycle did not prove discovery without content transfer requests.'
    }
    $journals = @(Get-ChildItem $journalDirectory -Force -Filter $journalFilter)
    if ($journals.Count -ne 1 -or $journals[0].FullName -ne $journalPath) { throw 'Journal location changed (INV-02).' }
    $stage = 'modification cycle'
    [IO.File]::WriteAllText((Join-Path $data 'upload.txt'), 'upload-v2-longer')
    $null = Invoke-TestDav 'PUT' "$remote/download.txt" ([Text.Encoding]::UTF8.GetBytes('download-v2-longer')) @(204)
    $null = Invoke-TestCycle
    Assert-TestContent (Invoke-TestDav 'GET' "$remote/upload.txt") 'upload-v2-longer'
    Assert-TestContent ([IO.File]::ReadAllBytes((Join-Path $data 'download.txt'))) 'download-v2-longer'
    $stage = 'conflict cycle'
    [IO.File]::WriteAllText((Join-Path $data 'upload.txt'), 'local-conflict-version-three')
    $null = Invoke-TestDav 'PUT' "$remote/upload.txt" ([Text.Encoding]::UTF8.GetBytes('remote-conflict-version-four')) @(204)
    $null = Invoke-TestCycle
    $contents = @(Get-ChildItem $data -File -Filter '*.txt' | ForEach-Object { [IO.File]::ReadAllText($_.FullName) })
    if ('local-conflict-version-three' -cnotin $contents -or 'remote-conflict-version-four' -cnotin $contents) { throw 'Conflict lost one fixture version (S03).' }
    Write-Output 'Initial upload/download, repeated cycle with stable journal path, modifications and conflict contents passed.'
    Write-Output 'Unchanged cycle: discovery observed, zero DAV content GET or write requests; initial transfer positive control passed.'
} catch {
    $failure = $true
} finally {
    if ($created) {
        try { $null = Invoke-TestDav 'DELETE' $remote -Expected @(204, 404) }
        catch { $cleanupFailed = $true }
    }
    $client.Dispose()
    $handler.Dispose()
    foreach ($directory in @($local, $cache)) {
        if (Test-Path $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
    }
}
if ($cleanupFailed) { throw "Remote cleanup failed; remove only test collection $name." }
if ($failure) { throw "Real-engine fixture failed at $stage; private exception details suppressed. Owned resources cleaned." }
Write-Output 'Cleanup passed: owned remote collection, local profile and CLI cache removed.'
