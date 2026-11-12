# Starts or removes one disposable local Nextcloud server for the server test tier.
# Inputs: -Start (optionally -Version, -Runtime wslc|docker) or -Stop -State <state.json>.
# Side effects: -Start pulls the image if absent and creates one labelled container with an anonymous
#   volume, a loopback-only HTTPS port and a short-lived fixture root in build/test-server-<id>/ (ignored).
#   -Stop removes that container, its volume, the image if -Start pulled it, and the fixture's files.
# Notes: The root key exists only in memory; nothing is added to any Windows trust store and TLS checks
#   stay on. The C++ fixtures accept this root only in their own Qt process (SYNCD_TEST_CA_*). Qt never
#   proxies loopback, so clients address the server as primary.syncd.test / alternate.syncd.test through
#   the fixture relay, and cleanup uses the direct loopback origin.
[CmdletBinding(DefaultParameterSetName = 'Start')]
param(
    [Parameter(ParameterSetName = 'Start')][switch]$Start,
    [Parameter(ParameterSetName = 'Start')][string]$Version = '35.0.2',
    [ValidateSet('wslc', 'docker')][string]$Runtime = 'wslc',
    [Parameter(ParameterSetName = 'Stop', Mandatory)][switch]$Stop,
    [Parameter(ParameterSetName = 'Stop', Mandatory)][string]$State
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

# Runtime runs one container-runtime command, keeping its raw output in memory only.
function Runtime([string[]]$Arguments, [string]$InputText = $null, [int]$TimeoutMs = 300000) {
    $info = [Diagnostics.ProcessStartInfo]::new((Get-Command "$Runtime.exe").Source)
    foreach ($a in $Arguments) { $info.ArgumentList.Add($a) }
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($info)
    try {
        $out = $p.StandardOutput.ReadToEndAsync(); $err = $p.StandardError.ReadToEndAsync()
        if ($InputText) { $p.StandardInput.Write($InputText) }
        $p.StandardInput.Close()
        if (-not $p.WaitForExit($TimeoutMs)) { $p.Kill($true); throw "Container runtime step '$($Arguments[0])' exceeded its deadline." }
        $null = $err.GetAwaiter().GetResult()
        if ($p.ExitCode -ne 0) { throw "Container runtime step '$($Arguments[0])' failed (exit $($p.ExitCode)); raw output withheld." }
        return $out.GetAwaiter().GetResult()
    } finally { $p.Dispose() }
}

if ($Stop) {
    $path = (Resolve-Path -LiteralPath $State).Path
    $dir = Split-Path $path -Parent
    $s = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ((Split-Path $dir -Leaf) -ne $s.Owner -or $s.Owner -notmatch '^test-server-[a-f0-9]{32}$' -or (Split-Path $dir -Parent) -ne (Join-Path $root 'build')) {
        throw 'Refusing cleanup: state ownership validation failed.'
    }
    $Runtime = $s.Runtime
    $ok = $true
    if ($s.ContainerCreated) {
        $remove = if ($Runtime -eq 'wslc') { @('remove', '--force', '--volumes', $s.Owner) } else { @('rm', '--force', '--volumes', $s.Owner) }
        try { $null = Runtime $remove -TimeoutMs 60000 } catch { $ok = $false; Write-Warning $_.Exception.Message }
    }
    if ($ok -and -not $s.ImageAlreadyPresent) {
        try { $null = Runtime @('rmi', $s.Image) -TimeoutMs 60000 } catch { $ok = $false; Write-Warning $_.Exception.Message }
    }
    if (-not $ok) { throw 'Disposable server cleanup needs attention; state kept.' }
    Remove-Item -LiteralPath $dir -Recurse -Force
    return
}

$id = 'test-server-' + [Guid]::NewGuid().ToString('N')
$dir = Join-Path $root "build/$id"
$null = New-Item -ItemType Directory -Path $dir
$image = "nextcloud:$Version-apache"
$s = [ordered]@{ Owner = $id; Runtime = $Runtime; Image = $image; ImageAlreadyPresent = $false; ContainerCreated = $false; Port = 0 }
$statePath = Join-Path $dir 'state.json'
function Save { $s | ConvertTo-Json | Set-Content -LiteralPath $statePath -Encoding utf8NoBOM }
Save
# A failed start removes what it created before reporting the failure.
trap { try { & $PSCommandPath -Stop -State $statePath } catch { Write-Warning $_.Exception.Message }; break }
$s.ImageAlreadyPresent = -not [string]::IsNullOrWhiteSpace((Runtime @('images', '--filter', "reference=$image", '--quiet')))
Save
if (-not $s.ImageAlreadyPresent) { $null = Runtime @('pull', '--quiet', $image) -TimeoutMs 600000 }
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0); $listener.Start()
$s.Port = $listener.LocalEndpoint.Port; $listener.Stop()
Save

# The fixture account is the instance's admin; its random password lives only in this ignored directory.
$password = [Convert]::ToBase64String([Security.Cryptography.RandomNumberGenerator]::GetBytes(30))
$envFile = Join-Path $dir 'bootstrap.env'
@('NEXTCLOUD_ADMIN_USER=fixture', "NEXTCLOUD_ADMIN_PASSWORD=$password", 'SQLITE_DATABASE=fixture',
  'NEXTCLOUD_TRUSTED_DOMAINS=localhost 127.0.0.1 primary.syncd.test alternate.syncd.test') | Set-Content -LiteralPath $envFile -Encoding utf8NoBOM
$detach = if ($Runtime -eq 'wslc') { '--detach' } else { '-d' }
$null = Runtime @('run', $detach, '--name', $id, '--label', "nextcloud-mygo.test=$id", '--publish', "127.0.0.1:$($s.Port):80", '--env-file', $envFile, $image)
$s.ContainerCreated = $true
Save

# A fixture root and a leaf for the loopback and reserved .test names, validated offline.
$hash = [Security.Cryptography.HashAlgorithmName]::SHA256
$padding = [Security.Cryptography.RSASignaturePadding]::Pkcs1
$rootKey = [Security.Cryptography.RSA]::Create(2048); $leafKey = [Security.Cryptography.RSA]::Create(2048)
$X = 'System.Security.Cryptography.X509Certificates'
$rootRequest = New-Object "$X.CertificateRequest" ("CN=Nextcloud-MyGo test server $($id.Substring(12, 8)) root", $rootKey, $hash, $padding)
$rootRequest.CertificateExtensions.Add((New-Object "$X.X509BasicConstraintsExtension" ($true, $true, 0, $true)))
$rootRequest.CertificateExtensions.Add((New-Object "$X.X509KeyUsageExtension" ([Security.Cryptography.X509Certificates.X509KeyUsageFlags]'KeyCertSign, CrlSign', $true)))
$rootRequest.CertificateExtensions.Add((New-Object "$X.X509SubjectKeyIdentifierExtension" ($rootRequest.PublicKey, $false)))
$from = [DateTimeOffset]::UtcNow.AddMinutes(-5); $until = [DateTimeOffset]::UtcNow.AddDays(1)
$rootCert = $rootRequest.CreateSelfSigned($from, $until)
$leafRequest = New-Object "$X.CertificateRequest" ('CN=localhost', $leafKey, $hash, $padding)
$leafRequest.CertificateExtensions.Add((New-Object "$X.X509BasicConstraintsExtension" ($false, $false, 0, $true)))
$leafRequest.CertificateExtensions.Add((New-Object "$X.X509KeyUsageExtension" ([Security.Cryptography.X509Certificates.X509KeyUsageFlags]'DigitalSignature, KeyEncipherment', $true)))
$eku = [Security.Cryptography.OidCollection]::new(); $null = $eku.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1'))
$leafRequest.CertificateExtensions.Add((New-Object "$X.X509EnhancedKeyUsageExtension" ($eku, $true)))
$san = New-Object "$X.SubjectAlternativeNameBuilder"
foreach ($name in 'localhost', 'primary.syncd.test', 'alternate.syncd.test') { $san.AddDnsName($name) }
$san.AddIpAddress([Net.IPAddress]::Loopback)
$leafRequest.CertificateExtensions.Add($san.Build())
$serial = [Security.Cryptography.RandomNumberGenerator]::GetBytes(16); $serial[0] = $serial[0] -band 0x7f
$leafCert = $leafRequest.Create($rootCert, $from, $until.AddMinutes(-1), $serial)
$chain = New-Object "$X.X509Chain"
$chain.ChainPolicy.TrustMode = 'CustomRootTrust'; $null = $chain.ChainPolicy.CustomTrustStore.Add($rootCert)
$chain.ChainPolicy.RevocationMode = 'NoCheck'
if (-not $chain.Build($leafCert)) { throw 'Fixture certificate chain failed offline validation.' }
[IO.File]::WriteAllText((Join-Path $dir 'root.pem'), $rootCert.ExportCertificatePem())
$s.RootThumbprint = $rootCert.Thumbprint
Save

# Wait for the first-run install over plain loopback HTTP, then switch the same port to HTTPS.
$deadline = [DateTime]::UtcNow.AddMinutes(4)
do {
    $ready = $false
    try { $st = Invoke-RestMethod -Uri "http://127.0.0.1:$($s.Port)/status.php" -TimeoutSec 5; $ready = $st.installed -and -not $st.maintenance } catch {}
    if (-not $ready) { Start-Sleep -Seconds 2 }
} while (-not $ready -and [DateTime]::UtcNow -lt $deadline)
if (-not $ready) { throw 'Disposable server did not finish its install within four minutes.' }
$exec = @('exec', '--interactive', $id, 'sh', '-ec')
if ($Runtime -eq 'docker') { $exec = @('exec', '-i', $id, 'sh', '-ec') }
$null = Runtime ($exec + 'umask 077; mkdir -p /etc/fixture-tls; cat > /etc/fixture-tls/leaf.key') $leafKey.ExportPkcs8PrivateKeyPem() 30000
$null = Runtime ($exec + 'cat > /etc/fixture-tls/leaf.pem') $leafCert.ExportCertificatePem() 30000
$site = "<VirtualHost *:80>`n ServerName localhost`n DocumentRoot /var/www/html`n SSLEngine on`n SSLCertificateFile /etc/fixture-tls/leaf.pem`n SSLCertificateKeyFile /etc/fixture-tls/leaf.key`n <Directory /var/www/html>`n  Require all granted`n  AllowOverride All`n  Options FollowSymLinks MultiViews`n </Directory>`n</VirtualHost>`n"
$null = Runtime ($exec + 'cat > /etc/apache2/sites-available/000-default.conf') $site 30000
$occ = "su -s /bin/sh www-data -c 'php /var/www/html/occ"
$null = Runtime ($exec + ("a2enmod ssl >/dev/null; $occ config:system:set overwriteprotocol --value=https' >/dev/null; " +
    "$occ background:cron' >/dev/null; $occ app:disable firstrunwizard' >/dev/null || true; apache2ctl graceful")) $null 120000
$rootKey.Dispose(); $leafKey.Dispose()

# Confirm HTTPS answers with the fixture chain (custom root, normal hostname checks) before handing over.
$deadline = [DateTime]::UtcNow.AddSeconds(30); $https = $false
do {
    $tcp = [Net.Sockets.TcpClient]::new()
    try {
        $tcp.Connect([Net.IPAddress]::Loopback, $s.Port)
        $validate = [Net.Security.RemoteCertificateValidationCallback]{
            param($sender, $certificate, $presented, $errors)
            $check = New-Object "$X.X509Chain"
            $check.ChainPolicy.TrustMode = 'CustomRootTrust'; $null = $check.ChainPolicy.CustomTrustStore.Add($rootCert)
            $check.ChainPolicy.RevocationMode = 'NoCheck'
            $leaf = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificate)
            return $check.Build($leaf) -and $leaf.MatchesHostname('primary.syncd.test', $false, $false)
        }
        $ssl = [Net.Security.SslStream]::new($tcp.GetStream(), $false, $validate)
        $ssl.AuthenticateAsClient('primary.syncd.test')
        $https = $true
        $ssl.Dispose()
    } catch { Start-Sleep -Milliseconds 500 } finally { $tcp.Dispose() }
} while (-not $https -and [DateTime]::UtcNow -lt $deadline)if (-not $https) { throw 'Disposable server did not answer HTTPS with the fixture chain.' }
$s.Https = $true
Save
$statePath
