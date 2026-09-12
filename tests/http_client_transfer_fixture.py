import http.server
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        try:
            if self.path == "/slow":
                for _ in range(500):
                    self.wfile.write(b"x")
                    self.wfile.flush()
                    time.sleep(0.01)
            else:
                self.wfile.write(b"x" * 65536 if self.path == "/large" else b"test")
        except (BrokenPipeError, ConnectionResetError):
            pass


with tempfile.TemporaryDirectory(prefix="opennow-http-test-") as temporary:
    directory = pathlib.Path(temporary)
    certificate = directory / "certificate.pem"
    key = directory / "key.pem"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
         "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1",
         "-keyout", str(key), "-out", str(certificate)],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    plain = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    secure = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certificate, key)
    secure.socket = context.wrap_socket(secure.socket, server_side=True)
    threads = [threading.Thread(target=server.serve_forever) for server in (plain, secure)]
    for thread in threads:
        thread.start()
    try:
        subprocess.run(
            [sys.argv[1], f"http://127.0.0.1:{plain.server_port}",
             f"https://127.0.0.1:{secure.server_port}"],
            check=True, timeout=10,
        )
    finally:
        for server in (plain, secure):
            server.shutdown()
            server.server_close()
        for thread in threads:
            thread.join()
