#!/usr/bin/env python3
"""
DNS resolver test helper — builds raw DNS query packets and validates responses.
Used by test_dns.sh but can also be run standalone for detailed diagnostics.

Usage:  python3 test_dns.py [host] [port]
"""

import random
import socket
import struct
import sys

# ── DNS wire helpers ────────────────────────────────────────────────────


def encode_name(name):
    """Encode a dotted name into DNS label format."""
    parts = name.rstrip(".").split(".")
    buf = b""
    for label in parts:
        buf += bytes([len(label)]) + label.encode()
    buf += b"\x00"
    return buf


def decode_name(data, offset):
    """Decode a DNS name starting at offset, handling compression pointers."""
    labels = []
    jumped = False
    end_offset = None
    while True:
        if offset >= len(data):
            break
        length = data[offset]
        if length == 0:
            if not jumped:
                end_offset = offset + 1
            break
        if (length & 0xC0) == 0xC0:
            if not jumped:
                end_offset = offset + 2
            pointer = ((length & 0x3F) << 8) | data[offset + 1]
            offset = pointer
            jumped = True
            continue
        offset += 1
        labels.append(data[offset : offset + length].decode())
        offset += length
    return ".".join(labels), end_offset if end_offset else offset + 1


def build_query(name, qtype):
    """Build a minimal DNS query packet."""
    txn_id = random.randint(0, 0xFFFF)
    flags = 0x0100  # RD=1
    header = struct.pack("!HHHHHH", txn_id, flags, 1, 0, 0, 0)
    # Question section
    qname = encode_name(name)
    question = qname + struct.pack("!HH", qtype, 1)  # type, class IN
    return txn_id, header + question


QTYPES = {"A": 1, "NS": 2, "CNAME": 5, "MX": 15, "TXT": 16, "AAAA": 28, "SRV": 33}
RTYPE_NAMES = {v: k for k, v in QTYPES.items()}


def parse_response(data):
    """Parse a DNS response, return (rcode, answers) where answers is a list
    of (name, type_str, rdata_str) tuples."""
    if len(data) < 12:
        return -1, []
    txn_id, flags, qdcount, ancount, nscount, arcount = struct.unpack(
        "!HHHHHH", data[:12]
    )
    rcode = flags & 0x0F
    qr = (flags >> 15) & 1
    aa = (flags >> 10) & 1

    # Skip question section
    offset = 12
    for _ in range(qdcount):
        _, offset = decode_name(data, offset)
        offset += 4  # QTYPE + QCLASS

    answers = []
    for _ in range(ancount):
        name, offset = decode_name(data, offset)
        if offset + 10 > len(data):
            break
        rtype, rclass, ttl, rdlen = struct.unpack("!HHIH", data[offset : offset + 10])
        offset += 10
        rdata_raw = data[offset : offset + rdlen]
        offset += rdlen

        type_str = RTYPE_NAMES.get(rtype, str(rtype))
        rdata_str = ""
        if rtype == 1 and rdlen == 4:  # A
            rdata_str = socket.inet_ntoa(rdata_raw)
        elif rtype == 28 and rdlen == 16:  # AAAA
            rdata_str = socket.inet_ntop(socket.AF_INET6, rdata_raw)
        elif rtype in (2, 5, 12):  # NS, CNAME, PTR
            rdata_str, _ = decode_name(data, offset - rdlen)
        elif rtype == 15:  # MX
            priority = struct.unpack("!H", rdata_raw[:2])[0]
            host, _ = decode_name(data, offset - rdlen + 2)
            rdata_str = f"{priority} {host}"
        elif rtype == 16:  # TXT
            # Concatenate all TXT strings
            pos = 0
            parts = []
            while pos < rdlen:
                slen = rdata_raw[pos]
                pos += 1
                parts.append(rdata_raw[pos : pos + slen].decode(errors="replace"))
                pos += slen
            rdata_str = "|".join(parts)
        elif rtype == 33:  # SRV
            pri, wt, port = struct.unpack("!HHH", rdata_raw[:6])
            target, _ = decode_name(data, offset - rdlen + 6)
            rdata_str = f"{pri} {wt} {port} {target}"
        else:
            rdata_str = rdata_raw.hex()

        answers.append((name, type_str, rdata_str, ttl))

    return rcode, answers


def query(name, qtype_str, host="127.0.0.1", port=19953, timeout=2):
    """Send a DNS query and return (rcode, answers)."""
    qtype = QTYPES[qtype_str] if qtype_str in QTYPES else int(qtype_str)
    txn_id, packet = build_query(name, qtype)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(packet, (host, port))
        data, _ = sock.recvfrom(4096)
        return parse_response(data)
    except socket.timeout:
        return -1, []
    finally:
        sock.close()


# ── Standalone test runner ──────────────────────────────────────────────


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 19953

    tests = [
        ("single.test", "A", 0, ["10.0.0.1"]),
        ("dual.test", "A", 0, ["10.0.0.2"]),
        ("dual.test", "AAAA", 0, ["fd00::2"]),
        ("multi.test", "A", 0, ["10.0.0.10", "10.0.0.11"]),
        ("alias.test", "CNAME", 0, ["single.test"]),
        ("mail.test", "MX", 0, ["10 mx1.mail.test", "20 mx2.mail.test"]),
        ("info.test", "TXT", 0, ["v=spf1 ~all", "hello=world"]),
        ("info.test", "NS", 0, ["ns1.info.test", "ns2.info.test"]),
        ("_http._tcp.web.test", "SRV", 0, ["0 100 8080 single.test"]),
        ("nonexistent.test", "A", 3, []),  # NXDOMAIN
    ]

    passed = 0
    failed = 0
    for name, qtype, expect_rcode, expect_rdata in tests:
        rcode, answers = query(name, qtype, host, port)
        got_rdata = [a[2] for a in answers]

        ok = (rcode == expect_rcode) and (sorted(got_rdata) == sorted(expect_rdata))
        tag = "\033[32mPASS\033[0m" if ok else "\033[31mFAIL\033[0m"
        if ok:
            passed += 1
        else:
            failed += 1
        print(f"  {tag} {name} {qtype}: rcode={rcode} answers={got_rdata}")
        if not ok:
            print(f"         expected rcode={expect_rcode} answers={expect_rdata}")

    # TTL check
    rcode, answers = query("ttl60.test", "A", host, port)
    if answers and answers[0][3] == 60:
        passed += 1
        print(f"  \033[32mPASS\033[0m ttl60.test A: TTL={answers[0][3]}")
    else:
        failed += 1
        ttl_got = answers[0][3] if answers else "no answer"
        print(f"  \033[31mFAIL\033[0m ttl60.test A: TTL={ttl_got} (expected 60)")

    # Case insensitivity check
    rcode, answers = query("SINGLE.TEST", "A", host, port)
    got_rdata = [a[2] for a in answers]
    if rcode == 0 and got_rdata == ["10.0.0.1"]:
        passed += 1
        print(f"  \033[32mPASS\033[0m SINGLE.TEST (case insensitive): {got_rdata}")
    else:
        failed += 1
        print(
            f"  \033[31mFAIL\033[0m SINGLE.TEST (case insensitive): rcode={rcode} answers={got_rdata}"
        )

    total = passed + failed
    print(f"\n{passed}/{total} tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
