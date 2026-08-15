param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Command
)

$ErrorActionPreference = 'Stop'
$client = [System.Net.Sockets.TcpClient]::new()

try {
    $client.Connect('127.0.0.1', 4444)
    $stream = $client.GetStream()
    $stream.ReadTimeout = 1000
    $writer = [System.IO.StreamWriter]::new($stream, [System.Text.Encoding]::ASCII, 1024, $true)
    $writer.NewLine = "`n"
    $writer.AutoFlush = $true

    Start-Sleep -Milliseconds 100
    while ($stream.DataAvailable) {
        $buffer = New-Object byte[] 4096
        [void]$stream.Read($buffer, 0, $buffer.Length)
    }

    $writer.WriteLine($Command)
    Start-Sleep -Milliseconds 200

    $result = New-Object System.Text.StringBuilder
    while ($stream.DataAvailable) {
        $buffer = New-Object byte[] 4096
        $count = $stream.Read($buffer, 0, $buffer.Length)
        [void]$result.Append([System.Text.Encoding]::ASCII.GetString($buffer, 0, $count))
    }
    $result.ToString().Trim()
}
finally {
    $client.Dispose()
}
