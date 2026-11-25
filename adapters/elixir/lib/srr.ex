defmodule SRR do
  @moduledoc """
  Top-level facade for the SRR (Send / Receive / Reply) client library in Elixir.
  """

  @doc """
  Starts an SRR client process linked to the caller.
  """
  defdelegate start_link(service_name, opts \\ []), to: SRR.Client

  @doc """
  Performs a synchronous Send/Reply transaction against the remote SRR service.
  Blocks the caller process until the server replies, times out, or fails.
  """
  defdelegate call(client, request, timeout \\ 5000), to: SRR.Client

  @doc """
  Sends an asynchronous one-way pulse notification to the SRR service.
  Does not wait for a reply.
  """
  defdelegate pulse(client, code, value), to: SRR.Client

  @doc """
  Disconnects and terminates the client process.
  """
  defdelegate stop(client), to: SRR.Client
end
