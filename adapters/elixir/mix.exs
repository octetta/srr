defmodule SRR.MixProject do
  use Mix.Project

  @version "0.1.0"

  def project do
    [
      app: :srr,
      version: @version,
      elixir: "~> 1.14",
      start_permanent: Mix.env() == :prod,
      description: "Elixir client and GenServer adapter for SRR (Synchronous Request/Reply)",
      package: package(),
      deps: deps()
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp deps do
    [
      {:ex_doc, ">= 0.0.0", only: :dev, runtime: false}
    ]
  end

  defp package do
    [
      name: "srr",
      licenses: ["MIT"],
      links: %{"GitHub" => "https://github.com/example/srr"}
    ]
  end
end
