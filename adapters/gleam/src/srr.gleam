//// SRR (Synchronous Send / Receive / Reply) for Gleam on the BEAM
////
//// Provides type-safe wire encoding, decoding, and client inter-op
//// for communicating with native C SRR services and synthesizers.

import gleam/bit_array
import gleam/dynamic.{type Dynamic}

pub const magic: Int = 0x53525231
pub const version: Int = 1

pub const msg_hello: Int = 1
pub const msg_request: Int = 2
pub const msg_reply: Int = 3
pub const msg_error: Int = 4
pub const msg_pulse: Int = 5
pub const msg_goodbye: Int = 7

/// An SRR synchronous request
pub type Request {
  Request(trans_id: Int, payload: BitArray)
}

/// An SRR reply from a service
pub type Reply {
  Success(trans_id: Int, payload: BitArray)
  Error(trans_id: Int, code: Int, reason: BitArray)
}

/// Encode a synchronous request frame (24-byte header + payload)
pub fn encode_request(trans_id: Int, payload: BitArray) -> BitArray {
  let len = bit_array.byte_size(payload)
  <<
    magic:32-little,
    version:16-little,
    msg_request:16-little,
    0:32-little,
    trans_id:64-little,
    len:32-little,
    payload:bits,
  >>
}

/// Encode an asynchronous one-way pulse (code + value)
pub fn encode_pulse(code: Int, value: Int) -> BitArray {
  <<
    magic:32-little,
    version:16-little,
    msg_pulse:16-little,
    0:32-little,
    0:64-little,
    16:32-little,
    code:32-little,
    0:32-little,
    value:64-little,
  >>
}

/// Decode a 24-byte reply header and body
pub fn decode_reply(frame: BitArray) -> Result(Reply, Nil) {
  case frame {
    <<0x53525231:32-little, 1:16-little, 3:16-little, _:32, trans_id:64-little, len:32-little, rest:bits>> -> {
      case bit_array.byte_size(rest) >= len {
        True -> {
          case bit_array.slice(rest, 0, len) {
            Ok(payload) -> Ok(Success(trans_id: trans_id, payload: payload))
            Error(_) -> Error(Nil)
          }
        }
        False -> Error(Nil)
      }
    }
    <<0x53525231:32-little, 1:16-little, 4:16-little, _:32, trans_id:64-little, _:32, code:32-little, reason:bits>> -> {
      Ok(Error(trans_id: trans_id, code: code, reason: reason))
    }
    _ -> Error(Nil)
  }
}

// BEAM / Erlang FFI bindings to srr_client.erl
pub type Client

@external(erlang, "srr_client", "start_link")
pub fn connect(service_name: String) -> Result(Client, Dynamic)

@external(erlang, "srr_client", "call")
pub fn call(client: Client, request: BitArray) -> Result(BitArray, Dynamic)

@external(erlang, "srr_client", "pulse")
pub fn pulse(client: Client, code: Int, value: Int) -> Result(Nil, Dynamic)

@external(erlang, "srr_client", "stop")
pub fn stop(client: Client) -> Nil
