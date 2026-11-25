module SRR
  module Protocol
    MAGIC   = 0x53525231 # "SRR1"
    VERSION = 1

    MSG_HELLO    = 1
    MSG_REQUEST  = 2
    MSG_REPLY    = 3
    MSG_ERROR    = 4
    MSG_PULSE    = 5
    MSG_CANCEL   = 6
    MSG_GOODBYE  = 7

    # IPC Header (24 bytes Little-Endian):
    # uint32 magic (V), uint16 ver (v), uint16 type (v), uint32 flags (V), uint64 tid (Q<), uint32 len (V)
    IPC_HEADER_FMT = "V v v V Q< V"
    IPC_HEADER_SIZE = 24

    # Wire Header (32 bytes Little-Endian):
    WIRE_HEADER_FMT = "V v v v v v v V Q< V"
    WIRE_HEADER_SIZE = 32
    CRC_SIZE = 2

    # CRC-16-CCITT (False): Poly 0x1021, Init 0xFFFF
    def self.crc16(data, init_crc = 0xFFFF)
      crc = init_crc & 0xFFFF
      bytes = data.is_a?(String) ? data.bytes : data
      bytes.each do |byte|
        crc ^= (byte << 8) & 0xFFFF
        8.times do
          if (crc & 0x8000) != 0
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF
          else
            crc = (crc << 1) & 0xFFFF
          end
        end
      end
      crc
    end

    def self.cobs_encode(data)
      bytes = data.is_a?(String) ? data.bytes : data
      out = [1]
      code_idx = 0
      code = 1
      bytes.each do |b|
        if b == 0
          out[code_idx] = code
          code = 1
          code_idx = out.length
          out << 1
        else
          out << b
          code += 1
          if code == 0xFF
            out[code_idx] = code
            code = 1
            code_idx = out.length
            out << 1
          end
        end
      end
      out[code_idx] = code
      out.pack("C*")
    end

    def self.cobs_decode(data)
      bytes = data.is_a?(String) ? data.bytes : data
      return "".b if bytes.empty?
      out = []
      i = 0
      n = bytes.length
      while i < n
        code = bytes[i]
        i += 1
        raise "Zero byte inside COBS data" if code == 0
        (1...code).each do
          raise "Truncated COBS stream" if i >= n
          out << bytes[i]
          i += 1
        end
        out << 0 if code < 0xFF && i < n
      end
      out.pack("C*")
    end

    def self.encode_wire_frame(msg_type:, service_id:, operation_id:, session_id:, trans_id:, payload: "", flags: 0)
      payload_bytes = payload.to_s.b
      raw = [MAGIC, VERSION, msg_type, flags, service_id, operation_id, 0, session_id, trans_id, payload_bytes.bytesize].pack(WIRE_HEADER_FMT) + payload_bytes
      crc = crc16(raw)
      unencoded = raw + [crc].pack("v")
      cobs_encode(unencoded) + "\x00".b
    end

    def self.decode_wire_frame(frame)
      bytes = frame.to_s.b
      raise "Wire frame must end with 0x00 delimiter" if bytes.empty? || bytes[-1] != "\x00".b
      unencoded = cobs_decode(bytes[0...-1])
      raise "Decoded wire frame too short" if unencoded.bytesize < WIRE_HEADER_SIZE + CRC_SIZE

      body = unencoded[0...-CRC_SIZE]
      expected_crc = unencoded[-CRC_SIZE, CRC_SIZE].unpack1("v")
      calc_crc = crc16(body)
      raise "CRC-16 mismatch" unless expected_crc == calc_crc

      magic, ver, msg_type, flags, s_id, op_id, res, sess_id, tid, plen = body[0, WIRE_HEADER_SIZE].unpack(WIRE_HEADER_FMT)
      raise "Invalid magic" unless magic == MAGIC
      raise "Invalid version" unless ver == VERSION

      payload = body[WIRE_HEADER_SIZE, plen]
      raise "Payload length mismatch" if payload.bytesize != plen

      [
        {
          magic: magic,
          version: ver,
          msg_type: msg_type,
          flags: flags,
          service_id: s_id,
          operation_id: op_id,
          session_id: sess_id,
          trans_id: tid,
          payload_len: plen
        },
        payload
      ]
    end

    def self.encode_ipc_request(trans_id, payload)
      payload_bytes = payload.to_s.b
      hdr = [MAGIC, VERSION, MSG_REQUEST, 0, trans_id, payload_bytes.bytesize].pack(IPC_HEADER_FMT)
      hdr + payload_bytes
    end

    def self.encode_ipc_pulse(code, value)
      body = [code, 0, value].pack("V V Q<")
      hdr = [MAGIC, VERSION, MSG_PULSE, 0, 0, body.bytesize].pack(IPC_HEADER_FMT)
      hdr + body
    end

    def self.encode_ipc_goodbye
      [MAGIC, VERSION, MSG_GOODBYE, 0, 0, 0].pack(IPC_HEADER_FMT)
    end

    def self.decode_ipc_header(raw_hdr)
      raise "Truncated header" if raw_hdr.bytesize < IPC_HEADER_SIZE
      magic, ver, msg_type, flags, trans_id, plen = raw_hdr[0, IPC_HEADER_SIZE].unpack(IPC_HEADER_FMT)
      raise "Invalid magic: 0x#{magic.to_s(16)}" unless magic == MAGIC
      raise "Invalid version: #{ver}" unless ver == VERSION

      {
        magic: magic,
        version: ver,
        msg_type: msg_type,
        flags: flags,
        trans_id: trans_id,
        payload_len: plen
      }
    end

    def self.pack_udp_datagram(msg_type:, service_id:, operation_id:, session_id:, trans_id:, payload: "")
      payload_bytes = payload.to_s.b
      hdr = [MAGIC, VERSION, msg_type, 0, service_id, operation_id, 0, session_id, trans_id, payload_bytes.bytesize].pack(WIRE_HEADER_FMT)
      body = hdr + payload_bytes
      crc = crc16(body)
      body + [crc].pack("v")
    end

    def self.unpack_udp_datagram(dgram)
      raise "Datagram too short" if dgram.bytesize < WIRE_HEADER_SIZE + CRC_SIZE

      body = dgram[0...-CRC_SIZE]
      expected_crc = dgram[-CRC_SIZE, CRC_SIZE].unpack1("v")
      calc_crc = crc16(body)
      raise "CRC-16 mismatch" unless expected_crc == calc_crc

      magic, ver, msg_type, flags, s_id, op_id, res, sess_id, tid, plen = body[0, WIRE_HEADER_SIZE].unpack(WIRE_HEADER_FMT)
      raise "Invalid magic" unless magic == MAGIC

      payload = body[WIRE_HEADER_SIZE, plen]
      [
        {
          magic: magic,
          version: ver,
          msg_type: msg_type,
          service_id: s_id,
          operation_id: op_id,
          session_id: sess_id,
          trans_id: tid,
          payload_len: plen
        },
        payload
      ]
    end
  end
end
