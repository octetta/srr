# ==============================================================================
# SRR PowerShell Client (Windows & Linux PowerShell Core)
# Direct Named Pipe (Windows: \\.\pipe\srr_*) or Unix Domain Socket (Linux)
# communication using native .NET System.IO.Pipes and System.Net.Sockets.
# ==============================================================================

param(
    [string]$ServiceName = "srr_synth",
    [string]$Command = "STATUS"
)

$SRR_MAGIC = [uint32]0x53525231
$SRR_VERSION = [uint16]1
$SRR_MSG_REQUEST = [uint16]2
$SRR_MSG_REPLY = [uint16]3
$SRR_MSG_PULSE = [uint16]5

function Send-SRRRequest {
    param(
        [string]$Service,
        [string]$Payload
    )

    $isWindows = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)

    if ($isWindows) {
        # Windows Named Pipe: \\.\pipe\srr_<Service>
        $pipeName = "srr_$Service"
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream(".", $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
        $pipe.Connect(2000)

        $writer = New-Object System.IO.BinaryWriter($pipe)
        $reader = New-Object System.IO.BinaryReader($pipe)
        $stream = $pipe
    } else {
        # Linux / macOS: Unix Domain Socket
        $sockPath = "/tmp/srr_${Service}.sock"
        $endpoint = New-Object System.Net.Sockets.UnixDomainSocketEndPoint($sockPath)
        $sock = New-Object System.Net.Sockets.Socket([System.Net.Sockets.AddressFamily]::Unix, [System.Net.Sockets.SocketType]::Stream, [System.Net.Sockets.ProtocolType]::Unspecified)
        $sock.Connect($endpoint)

        $stream = New-Object System.Net.Sockets.NetworkStream($sock)
        $writer = New-Object System.IO.BinaryWriter($stream)
        $reader = New-Object System.IO.BinaryReader($stream)
    }

    try {
        $payloadBytes = [System.Text.Encoding]::UTF8.GetBytes($Payload)
        
        # Write 24-byte header: uint32 magic, uint16 ver, uint16 type, uint32 flags, uint64 tid, uint32 len
        $writer.Write($SRR_MAGIC)
        $writer.Write($SRR_VERSION)
        $writer.Write($SRR_MSG_REQUEST)
        $writer.Write([uint32]0)
        $writer.Write([uint64]1)
        $writer.Write([uint32]$payloadBytes.Length)
        $writer.Write($payloadBytes)
        $writer.Flush()

        # Read 24-byte reply header
        $repMagic = $reader.ReadUInt32()
        $repVer   = $reader.ReadUInt16()
        $repType  = $reader.ReadUInt16()
        $repFlags = $reader.ReadUInt32()
        $repTid   = $reader.ReadUInt64()
        $repLen   = $reader.ReadUInt32()

        if ($repMagic -ne $SRR_MAGIC) {
            throw "Invalid SRR protocol magic: $repMagic"
        }

        # Read reply payload
        if ($repLen -gt 0) {
            $replyBytes = $reader.ReadBytes($repLen)
            return [System.Text.Encoding]::UTF8.GetString($replyBytes)
        }
        return ""
    } finally {
        $stream.Close()
    }
}

# --- Invocation ---
Write-Host "=== PowerShell SRR Client ==="
Write-Host "Connecting to service '$ServiceName'..."

try {
    $reply = Send-SRRRequest -Service $ServiceName -Payload $Command
    Write-Host "[REPLY]: $reply" -ForegroundColor Green
} catch {
    Write-Host "[NOTE]: $_ (Ensure synth_server or target daemon is running)" -ForegroundColor Yellow
}
