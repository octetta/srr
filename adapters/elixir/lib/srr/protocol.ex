defmodule SRR.Protocol do
  @moduledoc """
  Wire framing encoder/decoder for the SRR binary protocol.
  """

  @srr_magic 0x53525231
  @srr_version 1

  @msg_hello 1
  @msg_request 2
  @msg_reply 3
  @msg_error 4
  @msg_pulse 5
  @msg_cancel 6
  @msg_goodbye 7

  def magic, do: @srr_magic
  def version, do: @srr_version

  @doc "Encodes a synchronous request frame"
  @spec encode_request(non_neg_integer(), binary()) :: binary()
  def encode_request(trans_id, payload) when is_binary(payload) do
    len = byte_size(payload)
    <<@srr_magic::32-little, @srr_version::16-little, @msg_request::16-little,
      0::32-little, trans_id::64-little, len::32-little, payload::binary>>
  end

  @doc "Encodes an asynchronous one-way pulse frame"
  @spec encode_pulse(non_neg_integer(), non_neg_integer()) :: binary()
  def encode_pulse(code, value) when is_integer(code) and is_integer(value) do
    payload = <<code::32-little, 0::32-little, value::64-little>>
    len = byte_size(payload)
    <<@srr_magic::32-little, @srr_version::16-little, @msg_pulse::16-little,
      0::32-little, 0::64-little, len::32-little, payload::binary>>
  end

  @doc "Encodes a goodbye/disconnect frame"
  @spec encode_goodbye() :: binary()
  def encode_goodbye do
    <<@srr_magic::32-little, @srr_version::16-little, @msg_goodbye::16-little,
      0::32-little, 0::64-little, 0::32-little>>
  end

  @doc "Decodes an incoming wire frame header and payload"
  @spec decode_frame(binary()) :: {:ok, map(), binary()} | {:error, term()}
  def decode_frame(<<@srr_magic::32-little, @srr_version::16-little, type::16-little,
                     flags::32-little, trans_id::64-little, payload_len::32-little,
                     rest::binary>>) do
    if byte_size(rest) >= payload_len do
      <<payload::binary-size(payload_len), remaining::binary>> = rest
      frame = %{
        type: decode_msg_type(type),
        flags: flags,
        trans_id: trans_id,
        payload: payload
      }
      {:ok, frame, remaining}
    else
      {:more, payload_len + 24 - byte_size(rest)}
    end
  end

  def decode_frame(<<_bad_magic::32-little, _/binary>>), do: {:error, :bad_magic}
  def decode_frame(_incomplete), do: {:more, 24}

  defp decode_msg_type(@msg_hello), do: :hello
  defp decode_msg_type(@msg_request), do: :request
  defp decode_msg_type(@msg_reply), do: :reply
  defp decode_msg_type(@msg_error), do: :error
  defp decode_msg_type(@msg_pulse), do: :pulse
  defp decode_msg_type(@msg_cancel), do: :cancel
  defp decode_msg_type(@msg_goodbye), do: :goodbye
  defp decode_msg_type(other), do: {:unknown, other}
end
