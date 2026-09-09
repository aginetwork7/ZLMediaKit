#!/usr/bin/env python3
"""RTSP Digest SHA-256 auth test client.

Sends DESCRIBE with Digest SHA-256 to verify ZLM's onAuthSha256 implementation.
Usage: python3 test_rtsp_sha256.py [rtsp_url] [username] [password]
"""
import hashlib
import re
import socket
import sys

def sha256hex(s: str) -> str:
    return hashlib.sha256(s.encode()).hexdigest()

def parse_challenge(header: str) -> dict:
    """Parse WWW-Authenticate: Digest ... into a dict."""
    m = re.match(r'Digest\s+(.*)', header, re.IGNORECASE)
    if not m:
        return {}
    result = {}
    for item in re.findall(r'(\w+)="([^"]*)"', m.group(1)):
        result[item[0]] = item[1]
    # algorithm may not be quoted
    m2 = re.search(r'algorithm=(\S+)', header, re.IGNORECASE)
    if m2:
        result['algorithm'] = m2.group(1).strip(',')
    return result

def build_digest_response(username, password, realm, nonce, uri, method, qop=None, opaque=None, nc="00000001", cnonce="abcd1234"):
    """Build Digest SHA-256 response per RFC 7616."""
    ha1 = sha256hex(f"{username}:{realm}:{password}")
    ha2 = sha256hex(f"{method}:{uri}")
    if qop and qop == "auth":
        response = sha256hex(f"{ha1}:{nonce}:{nc}:{cnonce}:{qop}:{ha2}")
    else:
        response = sha256hex(f"{ha1}:{nonce}:{ha2}")

    parts = [
        f'username="{username}"',
        f'realm="{realm}"',
        f'nonce="{nonce}"',
        f'uri="{uri}"',
        f'response="{response}"',
        'algorithm=SHA-256',
    ]
    if qop:
        parts.extend([f'qop={qop}', f'nc={nc}', f'cnonce="{cnonce}"'])
    if opaque:
        parts.append(f'opaque="{opaque}"')
    return "Digest " + ", ".join(parts)

def rtsp_exchange(sock, request: str) -> str:
    """Send an RTSP request and read the full response."""
    sock.sendall(request.encode())
    data = b""
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        # Check if we have the full RTSP response (headers end with \r\n\r\n)
        if b"\r\n\r\n" in data:
            # Check Content-Length for body
            headers_end = data.index(b"\r\n\r\n") + 4
            headers = data[:headers_end].decode(errors='replace')
            m = re.search(r'Content-Length:\s*(\d+)', headers, re.IGNORECASE)
            if m:
                content_len = int(m.group(1))
                if len(data) >= headers_end + content_len:
                    break
            else:
                break
    return data.decode(errors='replace')

def main():
    url = sys.argv[1] if len(sys.argv) > 1 else "rtsp://localhost:554/live/C47905B7D4E2/1/s0"
    username = sys.argv[2] if len(sys.argv) > 2 else "tinynvr"
    password = sys.argv[3] if len(sys.argv) > 3 else "U9DqVCmGwLp17ggmYKFohkw0xaPtPpDR"

    # Parse host/port from URL
    m = re.match(r'rtsp://([^/:]+)(?::(\d+))?(/.*)$', url)
    if not m:
        print(f"Invalid RTSP URL: {url}")
        sys.exit(1)
    host = m.group(1)
    port = int(m.group(2)) if m.group(2) else 554
    uri = m.group(3)

    print(f"=== RTSP Digest SHA-256 Auth Test ===")
    print(f"URL: {url}")
    print(f"Host: {host}:{port}")
    print(f"User: {username}")
    print()

    sock = socket.create_connection((host, port), timeout=5)

    # Step 1: DESCRIBE without auth -> expect 401
    print("--- Step 1: DESCRIBE (no auth) ---")
    req1 = f"DESCRIBE {url} RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n"
    resp1 = rtsp_exchange(sock, req1)
    print(resp1[:500])

    if "401" not in resp1:
        print("\n[UNEXPECTED] Did not get 401. Auth may not be required.")
        sock.close()
        return

    # Extract WWW-Authenticate header
    auth_line = ""
    for line in resp1.split("\r\n"):
        if line.lower().startswith("www-authenticate:"):
            auth_line = line.split(":", 1)[1].strip()
            break

    if not auth_line:
        print("\n[ERROR] No WWW-Authenticate header in 401 response")
        sock.close()
        return

    print(f"\nChallenge: {auth_line}")
    params = parse_challenge(auth_line)
    print(f"Parsed: {params}")

    realm = params.get('realm', '')
    nonce = params.get('nonce', '')
    opaque = params.get('opaque', '')
    algorithm = params.get('algorithm', '')

    # Fix: parse qop separately since it may be inside algorithm due to non-quoted parsing
    qop_match = re.search(r'qop="([^"]*)"', auth_line, re.IGNORECASE)
    qop = qop_match.group(1) if qop_match else ''

    # Fix: parse algorithm more carefully - it's the unquoted value before comma
    alg_match = re.search(r'algorithm=([\w-]+)', auth_line, re.IGNORECASE)
    algorithm = alg_match.group(1) if alg_match else ''

    print(f"\nAlgorithm from server: '{algorithm}'")
    print(f"QoP: '{qop}'")

    # Keep same connection! Server sends close=false on first 401

    # Step 3: DESCRIBE with SHA-256 Digest auth
    print("\n--- Step 2: DESCRIBE (SHA-256 Digest) ---")
    auth_header = build_digest_response(username, password, realm, nonce, uri, "DESCRIBE", qop or None, opaque or None)
    print(f"Authorization: {auth_header}")

    req2 = f"DESCRIBE {url} RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\nAuthorization: {auth_header}\r\n\r\n"
    resp2 = rtsp_exchange(sock, req2)
    print(f"\nResponse:\n{resp2[:800]}")

    if "200 OK" in resp2:
        print("\n[SUCCESS] SHA-256 Digest auth passed!")
    elif "401" in resp2:
        print("\n[FAILED] SHA-256 Digest auth rejected (401)")
        # Show what server expected vs what we sent for debugging
        print(f"\nDebug info:")
        print(f"  HA1 = SHA256({username}:{realm}:{password})")
        print(f"       = {sha256hex(f'{username}:{realm}:{password}')}")
        print(f"  HA2 = SHA256(DESCRIBE:{uri})")
        print(f"       = {sha256hex(f'DESCRIBE:{uri}')}")
    else:
        print(f"\n[UNEXPECTED] Got response: {resp2[:100]}")

    sock.close()

if __name__ == "__main__":
    main()
