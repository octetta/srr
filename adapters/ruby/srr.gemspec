Gem::Specification.new do |spec|
  spec.name          = "srr"
  spec.version       = "0.1.0"
  spec.authors       = ["SRR Project"]
  spec.summary       = "Synchronous Send/Receive/Reply client and protocol for Ruby"
  spec.description   = "Pure Ruby client communicating directly over Unix Domain Sockets and UDP with zero external gem dependencies."
  spec.files         = Dir["lib/**/*.rb", "README.md"]
  spec.require_paths = ["lib"]
  spec.required_ruby_version = ">= 2.6.0"
end
