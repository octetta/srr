require_relative "../lib/srr"

def assert_equal(expected, actual, msg = "")
  raise "Assertion failed: expected #{expected.inspect}, got #{actual.inspect}. #{msg}" unless expected == actual
end

def run_tests
  puts "Running Ruby SRR Adapter unit tests..."

  # 1. CRC16 standard vector
  crc = SRR::Protocol.crc16("123456789")
  assert_equal(0x29B1, crc, "CRC16 vector failed")
  puts "  [PASS] CRC-16 CCITT vector"

  # 2. IPC Request framing
  req = SRR::Protocol.encode_ipc_request(100, "HELLO_RUBY")
  assert_equal(24 + 10, req.bytesize)
  hdr = SRR::Protocol.decode_ipc_header(req[0, 24])
  assert_equal(SRR::Protocol::MAGIC, hdr[:magic])
  assert_equal(100, hdr[:trans_id])
  assert_equal(10, hdr[:payload_len])
  assert_equal("HELLO_RUBY", req[24..-1])
  puts "  [PASS] IPC Request framing"

  # 3. IPC Pulse framing
  pulse = SRR::Protocol.encode_ipc_pulse(3, 4096)
  assert_equal(24 + 16, pulse.bytesize)
  pHdr = SRR::Protocol.decode_ipc_header(pulse[0, 24])
  assert_equal(SRR::Protocol::MSG_PULSE, pHdr[:msg_type])
  puts "  [PASS] IPC Pulse framing"

  # 4. UDP Datagram roundtrip
  dgram = SRR::Protocol.pack_udp_datagram(
    msg_type: SRR::Protocol::MSG_REQUEST,
    service_id: 4,
    operation_id: 8,
    session_id: 555,
    trans_id: 42,
    payload: "RUBY_DATAGRAM"
  )
  hdr, payload = SRR::Protocol.unpack_udp_datagram(dgram)
  assert_equal(4, hdr[:service_id])
  assert_equal(8, hdr[:operation_id])
  assert_equal(42, hdr[:trans_id])
  assert_equal("RUBY_DATAGRAM", payload)
  puts "  [PASS] UDP datagram roundtrip"

  puts "All Ruby tests passed successfully!"
end

run_tests if __FILE__ == $0
