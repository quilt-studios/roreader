param([switch]$NoBrowser)
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot 'platforms\installer'
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$files = @{'/' = @('index.html','text/html; charset=utf-8'); '/style.css' = @('style.css','text/css'); '/app.js' = @('app.js','text/javascript'); '/zip.js' = @('zip.js','text/javascript')}
$files['/locale.js'] = @('locale.js', 'text/javascript')
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'assets/translations') -Filter '*.json' | ForEach-Object {
    $files['/locales/' + $_.Name] = @($_.Name, 'application/json; charset=utf-8', $_.DirectoryName)
}
$files['/package.zip'] = @('ROREADER-qt6.zip', 'application/zip', $PSScriptRoot)
$files['/package.sha256'] = @('ROREADER-qt6.zip.sha256', 'text/plain', $PSScriptRoot)
$files['/logo.png'] = @('roreader-icon.png', 'image/png', (Join-Path $PSScriptRoot 'assets'))
Write-Host 'ROREADER Installer - dieses Fenster offen lassen. Strg+C beendet den Server.'
Write-Host "ROREADER Installer Port: $port"
if (-not $NoBrowser) {
Write-Host 'Google Chrome wird geoeffnet.'
$chromePaths = @(${env:ProgramFiles}, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA) |
    Where-Object { $_ } | ForEach-Object { Join-Path $_ 'Google\Chrome\Application\chrome.exe' }
$chrome = $chromePaths | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $chrome) {
    $listener.Stop()
    throw 'Google Chrome wurde nicht gefunden. Bitte Chrome installieren und die BAT erneut starten.'
}
Start-Process -FilePath $chrome -ArgumentList "http://127.0.0.1:$port/"
}
try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $client.ReceiveTimeout = 3000
            $client.SendTimeout = 3000
            $stream = $client.GetStream()
            $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::ASCII, $false, 4096, $true)
            $line = $reader.ReadLine()
            $request = if ($line) { $line.Split(' ') } else { @() }
            $path = if ($request.Length -eq 3) { $request[1] } else { '' }
            $headerCount = 0
            do { $header = $reader.ReadLine(); $headerCount++ } while ($header -and $header.Length -lt 8192 -and $headerCount -lt 64)
            if ($request.Length -eq 3 -and $request[0] -eq 'GET' -and $files.ContainsKey($path)) {
                $spec = $files[$path]
                $base = if ($spec.Length -gt 2) { $spec[2] } else { $root }
                $file = Join-Path $base $spec[0]
                if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
                    $body = [Text.Encoding]::UTF8.GetBytes('Not found')
                    $status = '404 Not Found'; $type = 'text/plain'
                } else {
                    $body = [IO.File]::ReadAllBytes($file)
                    $status = '200 OK'; $type = $spec[1]
                }
            } else {
                $body = [Text.Encoding]::UTF8.GetBytes('Not found')
                $status = '404 Not Found'; $type = 'text/plain'
            }
            $head = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 $status`r`nContent-Type: $type`r`nContent-Length: $($body.Length)`r`nConnection: close`r`nCache-Control: no-store`r`nX-Content-Type-Options: nosniff`r`nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'`r`n`r`n")
            $stream.Write($head, 0, $head.Length)
            $stream.Write($body, 0, $body.Length)
        } catch { Write-Warning $_.Exception.Message }
        finally { $client.Dispose() }
    }
} finally { $listener.Stop() }
