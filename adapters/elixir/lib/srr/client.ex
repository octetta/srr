defmodule SRR.Client do
  @moduledoc """
  GenServer maintaining a persistent local IPC connection to an SRR service.
  """
  use GenServer
  require Logger
  alias SRR.Protocol

  @default_timeout 5000

  defstruct [:socket, :service_path, next_trans_id: 1, pending: %{}]

  @type t :: %__MODULE__{
          socket: :gen_tcp.socket() | nil,
          service_path: String.t(),
          next_trans_id: non_neg_integer(),
          pending: %{non_neg_integer() => GenServer.from()}
        }

  # ---------------------------------------------------------------------------
  # Client API
  # ---------------------------------------------------------------------------

  @spec start_link(String.t(), keyword()) :: GenServer.on_start()
  def start_link(service_name, opts \\ []) do
    GenServer.start_link(__MODULE__, service_name, opts)
  end

  @spec call(GenServer.server(), binary(), timeout()) :: {:ok, binary()} | {:error, term()}
  def call(server, request, timeout \\ @default_timeout) when is_binary(request) do
    GenServer.call(server, {:call, request, timeout}, timeout + 1000)
  end

  @spec pulse(GenServer.server(), non_neg_integer(), non_neg_integer()) :: :ok
  def pulse(server, code, value) when is_integer(code) and is_integer(value) do
    GenServer.cast(server, {:pulse, code, value})
  end

  @spec stop(GenServer.server()) :: :ok
  def stop(server) do
    GenServer.stop(server)
  end

  # ---------------------------------------------------------------------------
  # GenServer Callbacks
  # ---------------------------------------------------------------------------

  @impl true
  def init(service_name) do
    path = resolve_path(service_name)
    case :gen_tcp.connect({:local, to_charlist(path)}, 0, [:binary, active: true, packet: 0]) do
      {:ok, socket} ->
        {:ok, %__MODULE__{socket: socket, service_path: path}}

      {:error, reason} ->
        {:stop, {:connect_failed, reason, path}}
    end
  end

  @impl true
  def handle_call({:call, request, _timeout}, from, %{socket: socket, next_trans_id: tid, pending: pending} = state) do
    frame = Protocol.encode_request(tid, request)

    case :gen_tcp.send(socket, frame) do
      :ok ->
        new_pending = Map.put(pending, tid, from)
        {:noreply, %{state | next_trans_id: tid + 1, pending: new_pending}}

      {:error, reason} ->
        {:reply, {:error, {:send_error, reason}}, state}
    end
  end

  @impl true
  def handle_cast({:pulse, code, value}, %{socket: socket} = state) do
    frame = Protocol.encode_pulse(code, value)
    _ = :gen_tcp.send(socket, frame)
    {:noreply, state}
  end

  @impl true
  def handle_info({:tcp, _socket, binary_data}, state) do
    handle_incoming_bytes(binary_data, state)
  end

  def handle_info({:tcp_closed, _socket}, state) do
    # Cancel all outstanding callers
    for {_tid, from} <- state.pending do
      GenServer.reply(from, {:error, :disconnected})
    end
    {:stop, :normal, %{state | pending: %{}}}
  end

  def handle_info({:tcp_error, _socket, reason}, state) do
    for {_tid, from} <- state.pending do
      GenServer.reply(from, {:error, {:tcp_error, reason}})
    end
    {:stop, reason, %{state | pending: %{}}}
  end

  @impl true
  def terminate(_reason, %{socket: socket}) when socket != nil do
    _ = :gen_tcp.send(socket, Protocol.encode_goodbye())
    :gen_tcp.close(socket)
    :ok
  end

  def terminate(_reason, _state), do: :ok

  # ---------------------------------------------------------------------------
  # Internal Helpers
  # ---------------------------------------------------------------------------

  defp handle_incoming_bytes(data, state) do
    case Protocol.decode_frame(data) do
      {:ok, %{type: :reply, trans_id: tid, payload: payload}, _rest} ->
        case Map.pop(state.pending, tid) do
          {nil, _pending} ->
            {:noreply, state}

          {from, new_pending} ->
            GenServer.reply(from, {:ok, payload})
            {:noreply, %{state | pending: new_pending}}
        end

      {:ok, %{type: :error, trans_id: tid, payload: <<code::32-little, _::binary>>}, _rest} ->
        case Map.pop(state.pending, tid) do
          {nil, _pending} ->
            {:noreply, state}

          {from, new_pending} ->
            GenServer.reply(from, {:error, decode_error_code(code)})
            {:noreply, %{state | pending: new_pending}}
        end

      _other ->
        {:noreply, state}
    end
  end

  defp decode_error_code(1), do: :timeout
  defp decode_error_code(2), do: :cancelled
  defp decode_error_code(3), do: :disconnected
  defp decode_error_code(4), do: :server_died
  defp decode_error_code(5), do: :closed
  defp decode_error_code(6), do: :queue_full
  defp decode_error_code(7), do: :too_large
  defp decode_error_code(8), do: :protocol_error
  defp decode_error_code(code), do: {:unknown_error, code}

  defp resolve_path(name) when is_binary(name) do
    if String.contains?(name, "/"), do: name, else: "/tmp/srr_#{name}.sock"
  end
end
