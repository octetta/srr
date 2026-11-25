<?php
/**
 * SRR PHP Client (Zero PECL Dependencies)
 * Direct Unix Domain Socket communication using PHP's native stream_socket_client and pack().
 */

class SRRClient {
    private $sock;
    private $next_trans_id = 1;

    const MAGIC   = 0x53525231; // "SRR1"
    const VERSION = 1;

    const MSG_REQUEST  = 2;
    const MSG_REPLY    = 3;
    const MSG_PULSE    = 5;
    const MSG_GOODBYE  = 7;

    public function __construct($service_name, $timeout = 2.0) {
        $path = "unix:///tmp/srr_{$service_name}.sock";
        $this->sock = @stream_socket_client($path, $errno, $errstr, $timeout);
        if (!$this->sock) {
            throw new Exception("Could not connect to $path: [$errno] $errstr");
        }
        stream_set_timeout($this->sock, (int)$timeout);
    }

    public function send($payload) {
        $tid = $this->next_trans_id++;
        $len = strlen($payload);

        // Header: uint32 magic (V), uint16 ver (v), uint16 type (v), uint32 flags (V), uint64 tid (P), uint32 len (V)
        $hdr = pack("VvvVPV", self::MAGIC, self::VERSION, self::MSG_REQUEST, 0, $tid, $len);
        fwrite($this->sock, $hdr . $payload);

        // Read 24-byte reply header
        $raw_hdr = fread($this->sock, 24);
        if (strlen($raw_hdr) < 24) {
            throw new Exception("Incomplete header response");
        }
        $fields = unpack("Vmagic/vver/vtype/Vflags/Ptid/Vlen", $raw_hdr);

        if ($fields['magic'] !== self::MAGIC) {
            throw new Exception("Invalid protocol magic");
        }

        // Read payload
        $reply = "";
        $rem = $fields['len'];
        while ($rem > 0) {
            $chunk = fread($this->sock, $rem);
            if ($chunk === false || strlen($chunk) === 0) break;
            $reply .= $chunk;
            $rem -= strlen($chunk);
        }

        return $reply;
    }

    public function pulse($code, $value) {
        $pulse_body = pack("VVP", $code, 0, $value);
        $hdr = pack("VvvVPV", self::MAGIC, self::VERSION, self::MSG_PULSE, 0, 0, strlen($pulse_body));
        fwrite($this->sock, $hdr . $pulse_body);
    }

    public function close() {
        if ($this->sock) {
            $bye = pack("VvvVPV", self::MAGIC, self::VERSION, self::MSG_GOODBYE, 0, 0, 0);
            @fwrite($this->sock, $bye);
            fclose($this->sock);
            $this->sock = null;
        }
    }
}

// --- Example CLI invocation ---
if (php_sapi_name() === 'cli') {
    $svc = $argv[1] ?? "srr_synth";
    echo "Connecting PHP client to '$svc'...\n";
    try {
        $client = new SRRClient($svc);
        $t0 = microtime(true);
        $reply = $client->send("STATUS");
        $ms = (microtime(true) - $t0) * 1000;
        echo "[REPLY in " . number_format($ms, 2) . " ms]: " . trim($reply) . "\n";

        $reply = $client->send("SET cutoff 1920.0");
        echo "[REPLY]: " . trim($reply) . "\n";

        $client->pulse(1, 16);
        echo "[PULSE]: Clock tick emitted successfully.\n";
        $client->close();
    } catch (Exception $e) {
        echo "PHP Client Note: " . $e->getMessage() . "\n";
    }
}
?>
