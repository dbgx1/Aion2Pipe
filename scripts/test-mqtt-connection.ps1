param([string]$Config = "$PSScriptRoot/../private/emqx-connection.json")
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$settings = Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
$secretBytes = [System.Security.Cryptography.ProtectedData]::Unprotect([Convert]::FromBase64String($settings.passwordDpapi), $null, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
$tcp = [Net.Sockets.TcpClient]::new()
$tls = $null
$packet = [IO.MemoryStream]::new()
function Write-MqttString([IO.Stream]$Stream, [byte[]]$Value) {
    if ($Value.Length -gt 65535) { throw 'MQTT string exceeds limit' }
    $Stream.WriteByte([byte]($Value.Length -shr 8))
    $Stream.WriteByte([byte]($Value.Length -band 255))
    $Stream.Write($Value, 0, $Value.Length)
}
try {
    $connect = $tcp.ConnectAsync([string]$settings.host, [int]$settings.port)
    if (!$connect.Wait(10000)) { throw 'TCP connection timed out' }
    $null = $connect.GetAwaiter().GetResult()
    # Default validation verifies both the certificate chain and the broker hostname.
    $tls = [Net.Security.SslStream]::new($tcp.GetStream(), $false)
    $tls.ReadTimeout = 10000
    $tls.WriteTimeout = 10000
    $tls.AuthenticateAsClient([string]$settings.host)
    Write-MqttString $packet ([Text.Encoding]::UTF8.GetBytes('MQTT'))
    $packet.WriteByte(4)
    $packet.WriteByte(0xC2) # MQTT 3.1.1: clean session, username and password; no will.
    $packet.WriteByte(0)
    $packet.WriteByte(30)
    $probeId = 'aion2-connection-probe-' + [Guid]::NewGuid().ToString('N')
    Write-MqttString $packet ([Text.Encoding]::UTF8.GetBytes($probeId))
    Write-MqttString $packet ([Text.Encoding]::UTF8.GetBytes([string]$settings.username))
    Write-MqttString $packet $secretBytes
    $tls.WriteByte(0x10)
    $remaining = [int]$packet.Length
    do {
        $digit = $remaining % 128
        $remaining = [int][Math]::Floor($remaining / 128)
        if ($remaining -gt 0) { $digit = $digit -bor 128 }
        $tls.WriteByte([byte]$digit)
    } while ($remaining -gt 0)
    $payload = $packet.ToArray()
    $tls.Write($payload, 0, $payload.Length)
    $tls.Flush()
    $reply = [byte[]]::new(4)
    $offset = 0
    while ($offset -lt 4) {
        $read = $tls.Read($reply, $offset, 4 - $offset)
        if ($read -eq 0) { throw 'Broker closed before CONNACK' }
        $offset += $read
    }
    if ($reply[0] -ne 0x20 -or $reply[1] -ne 2 -or $reply[2] -ne 0) { throw 'Invalid CONNACK' }
    if ($reply[3] -ne 0) { throw "Broker rejected authentication: CONNACK $($reply[3])" }
    $tls.WriteByte(0xE0)
    $tls.WriteByte(0)
    $tls.Flush()
    Write-Output 'TLS certificate validated; MQTT authentication accepted. No subscriptions or application messages sent.'
} finally {
    [Array]::Clear($secretBytes, 0, $secretBytes.Length)
    if ($payload) { [Array]::Clear($payload, 0, $payload.Length) }
    $packet.Dispose()
    if ($tls) { $tls.Dispose() }
    $tcp.Dispose()
}
