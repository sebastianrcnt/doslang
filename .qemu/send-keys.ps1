param(
    [Parameter(Mandatory)]
    [string] $Text,

    [ValidateRange(0, 1000)]
    [int] $DelayMilliseconds = 25,

    [switch] $NoEnter
)

$ErrorActionPreference = 'Stop'

$map = @{
    ' ' = 'spc'; ':' = 'shift-semicolon'; ';' = 'semicolon'
    '\' = 'backslash'; '|' = 'shift-backslash'; '/' = 'slash'; '?' = 'shift-slash'
    '.' = 'dot'; '>' = 'shift-dot'; ',' = 'comma'; '<' = 'shift-comma'
    '-' = 'minus'; '_' = 'shift-minus'; '=' = 'equal'; '+' = 'shift-equal'
    '[' = 'bracket_left'; '{' = 'shift-bracket_left'
    ']' = 'bracket_right'; '}' = 'shift-bracket_right'
    "'" = 'apostrophe'; '"' = 'shift-apostrophe'; '`' = 'grave_accent'; '~' = 'shift-grave_accent'
    '!' = 'shift-1'; '@' = 'shift-2'; '#' = 'shift-3'; '$' = 'shift-4'; '%' = 'shift-5'
    '^' = 'shift-6'; '&' = 'shift-7'; '*' = 'shift-8'; '(' = 'shift-9'; ')' = 'shift-0'
}

function ConvertTo-QemuKey([char] $Character) {
    $text = [string] $Character
    if ($map.ContainsKey($text)) { return $map[$text] }
    if ([char]::IsLetter($Character)) {
        $letter = [char]::ToLowerInvariant($Character)
        if ([char]::IsUpper($Character)) { return "shift-$letter" }
        return [string] $letter
    }
    if ([char]::IsDigit($Character)) { return $text }
    throw "Unsupported QEMU key character: '$Character'"
}

$client = [System.Net.Sockets.TcpClient]::new([System.Net.Sockets.AddressFamily]::InterNetwork)
try {
    $client.Connect([System.Net.IPAddress]::Parse('127.0.0.1'), 4444)
    $stream = $client.GetStream()
    $writer = [System.IO.StreamWriter]::new($stream, [System.Text.Encoding]::ASCII, 1024, $true)
    try {
        $writer.NewLine = "`r`n"
        $writer.AutoFlush = $true
        Start-Sleep -Milliseconds 100

        foreach ($char in $Text.ToCharArray()) {
            $writer.WriteLine("sendkey $(ConvertTo-QemuKey $char)")
            Start-Sleep -Milliseconds $DelayMilliseconds
        }
        if (-not $NoEnter) { $writer.WriteLine('sendkey ret') }
        Start-Sleep -Milliseconds $DelayMilliseconds
    }
    finally {
        $writer.Dispose()
    }
}
finally {
    $client.Dispose()
}
