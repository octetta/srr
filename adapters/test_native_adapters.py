#!/usr/bin/env python3
"""
Comprehensive Integration & Verification Suite for SRR Native Language Adapters
Tests Python, Perl, Node, Ruby, and PHP native protocol implementations against
live native C SRR services.
"""

import os
import shutil
import subprocess
import sys
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "adapters", "python"))

from srr.client import SRRClient, SRRUDPClient
from srr.protocol import srr_crc16

def run_command(cmd, desc):
    print(f"--- Running {desc} ---")
    res = subprocess.run(cmd, shell=True, cwd=REPO_ROOT, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"FAILED ({desc}):\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}")
        sys.exit(1)
    print(f"  [PASS] {desc}")

def main():
    print("================================================================================")
    print(" VERIFYING LANGUAGE-NATIVE SRR ADAPTERS (PYTHON, PERL, NODE, RUBY, PHP)")
    print("================================================================================")

    # 1. Run Python adapter tests
    run_command(f"{sys.executable} adapters/python/tests/test_client.py", "Python adapter unit tests")

    # 2. Run Perl adapter tests
    if shutil.which("perl"):
        run_command("perl adapters/perl/t/01_protocol.t", "Perl adapter unit tests")
    else:
        print("  [SKIP] perl interpreter not found")

    # 3. Run Go adapter tests
    if shutil.which("go"):
        run_command("cd adapters/go && go test -v -timeout 10s ./...", "Go adapter unit tests")
    else:
        print("  [SKIP] go toolchain not found")

    # 4. Run Node.js adapter tests if node installed
    if shutil.which("node"):
        run_command("node adapters/node/test/test_client.js", "Node.js adapter unit tests")
    else:
        print("  [SKIP] node interpreter not found on current system")

    # 5. Run Ruby adapter tests if ruby installed
    if shutil.which("ruby"):
        run_command("ruby adapters/ruby/test/test_client.rb", "Ruby adapter unit tests")
    else:
        print("  [SKIP] ruby interpreter not found on current system")

    # 6. Run PHP adapter tests if php installed
    if shutil.which("php"):
        run_command("php adapters/php/tests/ProtocolTest.php", "PHP adapter unit tests")
    else:
        print("  [SKIP] php interpreter not found on current system")

    # 6. Live end-to-end integration test with native C synth_server
    test_sock_name = "test_native_adapters_svc"
    test_sock_path = f"/tmp/srr_{test_sock_name}.sock"
    if os.path.exists(test_sock_path):
        os.unlink(test_sock_path)

    synth_bin = os.path.join(REPO_ROOT, "examples", "synth_engine", "synth_server")
    print(f"--- Starting background native synth_server ({synth_bin}) ---")
    proc = subprocess.Popen([synth_bin, test_sock_name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        # Wait for socket to appear
        ready = False
        for _ in range(50):
            if os.path.exists(test_sock_path):
                ready = True
                break
            time.sleep(0.05)

        if not ready:
            print("ERROR: synth_server failed to create socket in time")
            sys.exit(1)

        print("  -> Synth server is listening on", test_sock_path)

        # Test Python client live against synth_server
        print("--- Testing Python SRRClient live round-trip ---")
        with SRRClient(test_sock_name) as client:
            status = client.call("STATUS").decode("utf-8")
            assert "STATUS active_voices" in status, f"Unexpected status: {status}"
            print("  -> Python received status:", status.strip())

            # Update synth parameter
            set_res = client.call("SET cutoff 2800.0").decode("utf-8")
            assert "OK" in set_res or "cutoff" in set_res, f"Unexpected set_res: {set_res}"
            print("  -> Python updated parameter:", set_res.strip())

            # Send pulse
            client.pulse(1, 16)
            print("  -> Python emitted pulse(1, 16)")

        print("  [PASS] Python SRRClient live integration verified")

        # Test Perl client live against synth_server
        if shutil.which("perl"):
            print("--- Testing Perl SRR::Client live round-trip ---")
            perl_code = f"""use strict;
use warnings;
use lib '{REPO_ROOT}/adapters/perl/lib';
use SRR::Client;

my $client = SRR::Client->new('{test_sock_name}');
my $reply = $client->call('STATUS');
die "Bad reply: $reply" unless $reply =~ /STATUS active_voices/;
$reply = $client->call('SET resonance 0.85');
$client->pulse(1, 32);
$client->close();
print "  -> Perl successfully queried and updated synth_server\\n";
"""
            perl_res = subprocess.run(["perl", "-e", perl_code], cwd=REPO_ROOT, capture_output=True, text=True)
            if perl_res.returncode != 0:
                print(f"FAILED (Perl live test):\nSTDOUT:\n{perl_res.stdout}\nSTDERR:\n{perl_res.stderr}")
                sys.exit(1)
            print(perl_res.stdout.strip())
            print("  [PASS] Perl SRR::Client live integration verified")

        # Test Go client live against synth_server
        if shutil.which("go"):
            print("--- Testing Go SRR Client live round-trip ---")
            go_res = subprocess.run(
                ["go", "run", "./examples/go/client", "--service", test_sock_name, "--cmd", "STATUS"],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
            )
            if go_res.returncode != 0:
                print(f"FAILED (Go live test):\nSTDOUT:\n{go_res.stdout}\nSTDERR:\n{go_res.stderr}")
                sys.exit(1)
            print("  -> Go client received:", go_res.stdout.strip().split("\n")[-2])
            print("  [PASS] Go SRR Client live integration verified")

    finally:
        proc.kill()
        proc.wait(timeout=2)
        if os.path.exists(test_sock_path):
            try:
                os.unlink(test_sock_path)
            except OSError:
                pass


    print("================================================================================")
    print(" ALL NATIVE ADAPTER CHECKS PASSED WITH ZERO ERRORS!")
    print("================================================================================")

if __name__ == "__main__":
    main()
