<?php

namespace SRR;

class Protocol {
    const MAGIC   = 0x53525231; // "SRR1"
    const VERSION = 1;

    const MSG_HELLO    = 1;
    const MSG_REQUEST  = 2;
    const MSG_REPLY    = 3;
    const MSG_ERROR    = 4;
    const MSG_PULSE    = 5;
    const MSG_CANCEL   = 6;
    const MSG_GOODBYE  = 7;

    const IPC_HEADER_SIZE  = 24;
    const WIRE_HEADER_SIZE = 32;
    const CRC_SIZE         = 2;

    public static function crc16(string $data, int $init_crc = 0xFFFF): int {
        $crc = $init_crc & 0xFFFF;
        $len = strlen($data);
        for ($i = 0; $i < $len; $i++) {
            $crc ^= (ord($data[$i]) << 8) & 0xFFFF;
            for ($b = 0; $b < 8; $b++) {
                if ($crc & 0x8000) {
                    $crc = (($crc << 1) ^ 0x1021) & 0xFFFF;
                } else {
                    $crc = ($crc << 1) & 0xFFFF;
                }
            }
        }
        return $crc;
    }

    public static function cobsEncode(string $data): string {
        $len = strlen($data);
        $out = [1];
        $code_idx = 0;
        $code = 1;
        for ($i = 0; $i < $len; $i++) {
            $b = ord($data[$i]);
            if ($b === 0) {
                $out[$code_idx] = $code;
                $code = 1;
                $code_idx = count($out);
                $out[] = 1;
            } else {
                $out[] = $b;
                $code++;
                if ($code === 0xFF) {
                    $out[$code_idx] = $code;
                    $code = 1;
                    $code_idx = count($out);
                    $out[] = 1;
                }
            }
        }
        $out[$code_idx] = $code;
        return pack("C*", ...$out);
    }

    public static function cobsDecode(string $data): string {
        $len = strlen($data);
        if ($len === 0) return "";
        $out = [];
        $i = 0;
        while ($i < $len) {
            $code = ord($data[$i++]);
            if ($code === 0) throw new \UnexpectedValueException("Zero byte inside COBS data");
            for ($j = 1; $j < $code; $j++) {
                if ($i >= $len) throw new \UnexpectedValueException("Truncated COBS stream");
                $out[] = ord($data[$i++]);
            }
            if ($code < 0xFF && $i < $len) {
                $out[] = 0;
            }
        }
        return pack("C*", ...$out);
    }

    public static function encodeWireFrame(
        int $msg_type,
        int $service_id,
        int $operation_id,
        int $session_id,
        int $trans_id,
        string $payload = "",
        int $flags = 0
    ): string {
        $hdr = pack("VvvvvvvVPV", self::MAGIC, self::VERSION, $msg_type, $flags, $service_id, $operation_id, 0, $session_id, $trans_id, strlen($payload));
        $raw = $hdr . $payload;
        $crc = self::crc16($raw);
        $unencoded = $raw . pack("v", $crc);
        return self::cobsEncode($unencoded) . "\x00";
    }

    public static function decodeWireFrame(string $frame): array {
        if (strlen($frame) === 0 || substr($frame, -1) !== "\x00") {
            throw new \InvalidArgumentException("Frame must end with 0x00 delimiter");
        }
        $unencoded = self::cobsDecode(substr($frame, 0, -1));
        if (strlen($unencoded) < self::WIRE_HEADER_SIZE + self::CRC_SIZE) {
            throw new \InvalidArgumentException("Decoded frame too short");
        }
        $body = substr($unencoded, 0, -self::CRC_SIZE);
        $expected_crc = unpack("v", substr($unencoded, -self::CRC_SIZE))[1];
        $calc_crc = self::crc16($body);
        if ($expected_crc !== $calc_crc) {
            throw new \UnexpectedValueException("CRC-16 mismatch: expected $calc_crc, got $expected_crc");
        }

        $fields = unpack("Vmagic/vver/vtype/vflags/vs_id/vop_id/vres/Vsess_id/Ptid/Vlen", substr($body, 0, self::WIRE_HEADER_SIZE));
        if ($fields['magic'] !== self::MAGIC) throw new \UnexpectedValueException("Invalid magic");
        if ($fields['ver'] !== self::VERSION) throw new \UnexpectedValueException("Invalid version");

        $payload = substr($body, self::WIRE_HEADER_SIZE, $fields['len']);
        if (strlen($payload) !== $fields['len']) throw new \UnexpectedValueException("Payload length mismatch");

        return [
            'header' => [
                'magic'        => $fields['magic'],
                'version'      => $fields['ver'],
                'msg_type'     => $fields['type'],
                'flags'        => $fields['flags'],
                'service_id'   => $fields['s_id'],
                'operation_id' => $fields['op_id'],
                'session_id'   => $fields['sess_id'],
                'trans_id'     => $fields['tid'],
                'payload_len'  => $fields['len'],
            ],
            'payload' => $payload
        ];
    }

    public static function encodeIpcRequest(int $trans_id, string $payload): string {
        $hdr = pack("VvvVPV", self::MAGIC, self::VERSION, self::MSG_REQUEST, 0, $trans_id, strlen($payload));
        return $hdr . $payload;
    }

    public static function encodeIpcPulse(int $code, int $value): string {
        $body = pack("VVP", $code, 0, $value);
        $hdr = pack("VvvVPV", self::MAGIC, self::VERSION, self::MSG_PULSE, 0, 0, strlen($body));
        return $hdr . $body;
    }

    public static function encodeIpcGoodbye(): string {
        return pack("VvvVPV", self::MAGIC, self::VERSION, self::MSG_GOODBYE, 0, 0, 0);
    }

    public static function decodeIpcHeader(string $raw_hdr): array {
        if (strlen($raw_hdr) < self::IPC_HEADER_SIZE) {
            throw new \InvalidArgumentException("Incomplete header response");
        }
        $fields = unpack("Vmagic/vver/vtype/Vflags/Ptid/Vlen", substr($raw_hdr, 0, self::IPC_HEADER_SIZE));
        if ($fields['magic'] !== self::MAGIC) {
            throw new \UnexpectedValueException("Invalid SRR protocol magic");
        }
        if ($fields['ver'] !== self::VERSION) {
            throw new \UnexpectedValueException("Unsupported SRR protocol version: " . $fields['ver']);
        }
        return [
            'magic'       => $fields['magic'],
            'version'     => $fields['ver'],
            'msg_type'    => $fields['type'],
            'flags'       => $fields['flags'],
            'trans_id'    => $fields['tid'],
            'payload_len' => $fields['len'],
        ];
    }

    public static function packUdpDatagram(
        int $msg_type,
        int $service_id,
        int $operation_id,
        int $session_id,
        int $trans_id,
        string $payload = ""
    ): string {
        $hdr = pack("VvvvvvvVPV", self::MAGIC, self::VERSION, $msg_type, 0, $service_id, $operation_id, 0, $session_id, $trans_id, strlen($payload));
        $body = $hdr . $payload;
        $crc = self::crc16($body);
        return $body . pack("v", $crc);
    }

    public static function unpackUdpDatagram(string $dgram): array {
        if (strlen($dgram) < self::WIRE_HEADER_SIZE + self::CRC_SIZE) {
            throw new \InvalidArgumentException("Datagram too short");
        }
        $body = substr($dgram, 0, strlen($dgram) - self::CRC_SIZE);
        $expected_crc = unpack("v", substr($dgram, -self::CRC_SIZE))[1];
        $calc_crc = self::crc16($body);
        if ($expected_crc !== $calc_crc) {
            throw new \UnexpectedValueException("CRC-16 mismatch: expected $calc_crc, got $expected_crc");
        }

        $fields = unpack("Vmagic/vver/vtype/vflags/vs_id/vop_id/vres/Vsess_id/Ptid/Vlen", substr($body, 0, self::WIRE_HEADER_SIZE));
        if ($fields['magic'] !== self::MAGIC) {
            throw new \UnexpectedValueException("Invalid magic");
        }

        $payload = substr($body, self::WIRE_HEADER_SIZE, $fields['len']);
        return [
            'header'  => $fields,
            'payload' => $payload,
        ];
    }
}
