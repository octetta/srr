require "socket"
require "timeout"
require_relative "protocol"

module SRR
  class Error < StandardError; end

  class Client
    attr_reader :socket_path

    def initialize(service_name, socket_dir: "/tmp")
      @socket_path = service_name.start_with?("/") ? service_name : File.join(socket_dir, "srr_#{service_name}.sock")
      @sock = nil
      @next_trans_id = 1
    end

    def connect(timeout: 2.0)
      return self if @sock
      Timeout.timeout(timeout) do
        @sock = UNIXSocket.new(@socket_path)
      end
      self
    end

    def close
      if @sock
        begin
          @sock.write(Protocol.encode_ipc_goodbye)
          @sock.close
        rescue IOError, SystemCallError
        end
        @sock = nil
      end
    end

    def call(payload, timeout: 2.0)
      connect(timeout: timeout) unless @sock

      tid = @next_trans_id
      @next_trans_id += 1

      frame = Protocol.encode_ipc_request(tid, payload)
      @sock.write(frame)

      raw_hdr = @sock.read(Protocol::IPC_HEADER_SIZE)
      raise Error, "Premature connection close" unless raw_hdr && raw_hdr.bytesize == Protocol::IPC_HEADER_SIZE

      hdr = Protocol.decode_ipc_header(raw_hdr)
      reply_payload = hdr[:payload_len] > 0 ? @sock.read(hdr[:payload_len]) : ""

      if hdr[:msg_type] == Protocol::MSG_ERROR
        raise Error, "Remote error: #{reply_payload}"
      end

      reply_payload
    end

    def pulse(code, value)
      connect unless @sock
      frame = Protocol.encode_ipc_pulse(code, value)
      @sock.write(frame)
    end
  end

  class UDPClient
    def initialize(host, port, default_timeout: 1.5)
      @host = host
      @port = port
      @default_timeout = default_timeout
      @session_id = Time.now.to_i & 0xFFFFFFFF
      @next_trans_id = 1
      @sock = UDPSocket.new
    end

    def close
      @sock.close if @sock
      @sock = nil
    end

    def call(service_id, operation_id, payload = "", timeout: nil)
      effective_timeout = timeout || @default_timeout
      tid = @next_trans_id
      @next_trans_id += 1

      dgram = Protocol.pack_udp_datagram(
        msg_type: Protocol::MSG_REQUEST,
        service_id: service_id,
        operation_id: operation_id,
        session_id: @session_id,
        trans_id: tid,
        payload: payload
      )

      @sock.send(dgram, 0, @host, @port)

      Timeout.timeout(effective_timeout) do
        loop do
          resp_data, _peer = @sock.recvfrom(65535)
          begin
            hdr, reply_payload = Protocol.unpack_udp_datagram(resp_data)
            if hdr[:trans_id] == tid
              raise Error, "Remote error: #{reply_payload}" if hdr[:msg_type] == Protocol::MSG_ERROR
              return reply_payload
            end
          rescue StandardError
            # skip malformed or mismatched frames
          end
        end
      end
    end

    def pulse(service_id, operation_id, code, value)
      body = [code, 0, value].pack("V V Q<")
      dgram = Protocol.pack_udp_datagram(
        msg_type: Protocol::MSG_PULSE,
        service_id: service_id,
        operation_id: operation_id,
        session_id: @session_id,
        trans_id: 0,
        payload: body
      )
      @sock.send(dgram, 0, @host, @port)
    end
  end
end
