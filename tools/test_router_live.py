#!/usr/bin/env python3
"""Read-only live tests from the USB-sharing computer (Python standard library)."""
import argparse
import base64
import concurrent.futures
import http.client
import ipaddress
import json
import re
import secrets
import socket
import struct
import time
import urllib.parse


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wan_ip", type=ipaddress.IPv4Address)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--report", help="Save measured results as JSON")
    parser.add_argument("--site", action="append", default=[],
                        help="Additional hostname to fetch over verified HTTPS (repeatable)")
    args = parser.parse_args()
    results = {}

    def request(target, method="GET"):
        connection = http.client.HTTPConnection(str(args.wan_ip), args.port, timeout=30)
        start = time.monotonic()
        try:
            connection.request(method, target)
            response = connection.getresponse()
            body = response.read(4 * 1024 * 1024 + 1)
            return response.status, body, time.monotonic() - start
        finally:
            connection.close()

    # Waiting for SNTP is explicit; certificate validation is never bypassed.
    deadline = time.monotonic() + 90
    while True:
        status, home, _ = request("/")
        assert status == 200, (status, home[:160])
        if b"Network clock: ready" in home:
            break
        if time.monotonic() >= deadline:
            raise TimeoutError("ESP network clock did not synchronize")
        print("Waiting for ESP network time...", flush=True)
        time.sleep(2)
    print("PASS: USB host access, HTTP/1.0 home page, synchronized clock", flush=True)

    query_id = secrets.randbits(16)
    query = struct.pack("!HHHHHH", query_id, 0x100, 1, 0, 0, 0)
    query += b"\x07example\x03com\0" + struct.pack("!HH", 1, 1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as dns:
        dns.settimeout(4)
        dns.connect((str(args.wan_ip), 53))
        dns.send(query)
        answer = dns.recv(1232)
    ident, flags, _, answers, _, _ = struct.unpack("!6H", answer[:12])
    assert ident == query_id and flags & 0x8000 and not flags & 15 and answers > 0
    print("PASS: DNS relay returned an A response", flush=True)

    original = b'<!doctype html><title>PSP gateway test</title><a href="https://example.org/a">ok</a>'
    encoded = urllib.parse.quote(base64.urlsafe_b64encode(original).decode(), safe="")
    fixture = "http://httpbingo.org/base64/" + encoded + "?content-type=text%2Fhtml"
    status, body, duration = request(fixture)
    # The public fixture may HTML-escape its payload; check the URL semantics.
    assert status == 200 and b"PSP gateway test" in body, (status, body[:160])
    assert b"http://example.org/a" in body and b"https://example.org/a" not in body
    results["html_fixture"] = {"status": status, "bytes": len(body), "seconds": duration}
    print("PASS: verified HTTPS fetch and HTML link rewrite", flush=True)

    # Positive control on the same service distinguishes trust rejection from
    # inability to reach badssl.com. ESP-TLS may report verification failures
    # without detailed X.509 flags, so the public HTTP error text is not an API.
    status, body, _ = request("http://sha256.badssl.com/")
    assert status == 200 and body, (status, body[:160])
    results["certificate_checks"] = {"valid_sha256": status}
    for name in ("expired", "wrong.host", "self-signed"):
        status, body, _ = request("http://" + name + ".badssl.com/")
        assert status == 502 and b"HTTPS" in body, (name, status, body[:160])
        results["certificate_checks"][name] = status
    print("PASS: valid certificate accepted; expired, wrong-name and self-signed certificates rejected", flush=True)

    results["additional_sites"] = {}
    for name in args.site:
        status, body, duration = request("http://" + name + "/")
        assert 200 <= status < 400, (name, status, body[:160])
        results["additional_sites"][name] = {"status": status, "bytes": len(body), "seconds": duration}
        print("PASS: verified HTTPS for", name, "status", status, "bytes", len(body), flush=True)

    status, _, _ = request("example.com:443", "CONNECT")
    assert status == 405
    print("PASS: CONNECT is not used as a certificate workaround", flush=True)

    status, body, _ = request("http://httpbingo.org/redirect-to?url=https%3A%2F%2Fexample.org%2Fa")
    assert status == 302, (status, body[:160])
    # Inspect Location separately because the helper intentionally returns no
    # headers and never follows redirects.
    connection = http.client.HTTPConnection(str(args.wan_ip), args.port, timeout=30)
    try:
        connection.request("GET", "http://httpbingo.org/redirect-to?url=https%3A%2F%2Fexample.org%2Fa")
        response = connection.getresponse()
        assert response.status == 302 and response.getheader("Location") == "http://example.org/a"
        response.read()
    finally:
        connection.close()
    results["redirect_rewrite"] = True
    print("PASS: HTTPS redirect rewritten to HTTP without following it on ESP", flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(request, fixture, method) for method in ("GET", "HEAD")]
        pair = [f.result() for f in futures]
        assert pair[0][0] == pair[1][0] == 200 and not pair[1][1]
    print("PASS: two concurrent HTTPS requests", flush=True)

    measurements = []
    for _ in range(3):
        status, data, duration = request("http://httpbingo.org/bytes/65536")
        assert status == 200 and len(data) == 65536, (status, len(data), data[:120])
        # The home page measures on the MCU, without retaining URLs or content.
        home_status, home, _ = request("/")
        assert home_status == 200
        match = re.search(rb"Last: complete, (\d+) bytes, connection (\d+) ms, total (\d+) ms", home)
        assert match, home[-400:]
        measured_bytes, connect_ms, total_ms = map(int, match.groups())
        assert measured_bytes == 65536
        measurement = {"bytes": measured_bytes, "connection_ms": connect_ms,
                       "total_ms": total_ms, "host_seconds": duration,
                       "end_to_end_kib_s": 64 / duration}
        measurements.append(measurement)
        print("MEASURE:", json.dumps(measurement), flush=True)
    results["https_64k"] = measurements
    results["note"] = "Connection includes DNS/TCP/TLS; total includes server latency and delivery to USB host."
    if args.report:
        with open(args.report, "w") as output:
            json.dump(results, output, indent=2)
            output.write("\n")
    print("PASS: all live checks; direct PSP NAPT/radio still requires PSP", flush=True)


if __name__ == "__main__":
    main()
