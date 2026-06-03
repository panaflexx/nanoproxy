#!/usr/bin/env python3
"""Minimal HTTP backend for proxy timeout tests."""

import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"ok\n")

    def log_message(self, format, *args):
        pass  # suppress logs


port = int(sys.argv[1]) if len(sys.argv) > 1 else 19991
HTTPServer(("127.0.0.1", port), Handler).serve_forever()
