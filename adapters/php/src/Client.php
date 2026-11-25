<?php

namespace SRR;

class Client {
    private $sock = null;
    private $socket_path;
    private $next_trans_id = 1;

    public function __construct(string $service_name, string $socket_dir = "/tmp") {
        $this->socket_path = (strpos($service_name, '/') === 0)
            ? "unix://{$service_name}"
            : "unix://{$socket_dir}/srr_{$service_name}.sock";
    }

    public function connect(float $timeout = 2.0): self {
        if ($this->sock) return $this;
        $this->sock = @stream_socket_client($this->socket_path, $errno, $errstr, $timeout);
        if (!$this->sock) {
            throw new \RuntimeException("Could not connect to {$this->socket_path}: [$errno] $errstr");
        }
        stream_set_timeout($this->sock, (int)$timeout);
        return $this;
    }

    public function close(): void {
        if ($this->sock) {
            @fwrite($this->sock, Protocol::encodeIpcGoodbye());
            @fclose($this->sock);
            $this->sock = null;
        }
    }

    public function call(string $payload, float $timeout = 2.0): string {
        if (!$this->sock) $this->connect($timeout);

        $tid = $this->next_trans_id++;
        $frame = Protocol::encodeIpcRequest($tid, $payload);
        fwrite($this->sock, $frame);

        $raw_hdr = fread($this->sock, Protocol::IPC_HEADER_SIZE);
        if (!$raw_hdr || strlen($raw_hdr) < Protocol::IPC_HEADER_SIZE) {
            throw new \RuntimeException("Truncated header or socket closed");
        }

        $hdr = Protocol::decodeIpcHeader($raw_hdr);
        $reply = "";
        $rem = $hdr['payload_len'];
        while ($rem > 0) {
            $chunk = fread($this->sock, $rem);
            if ($chunk === false || strlen($chunk) === 0) break;
            $reply .= $chunk;
            $rem -= strlen($chunk);
        }

        if ($hdr['msg_type'] === Protocol::MSG_ERROR) {
            throw new \RuntimeException("Service returned error: $reply");
        }

        return $reply;
    }

    public function pulse(int $code, int $value): void {
        if (!$this->sock) $this->connect();
        $frame = Protocol::encodeIpcPulse($code, $value);
        fwrite($this->sock, $frame);
    }
}
