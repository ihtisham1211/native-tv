"""Custom-CA TLS transport and MQTT 3.1.1 CONNECT/CONNACK acceptance."""

import os
import socket
import ssl
import subprocess
import tempfile
import threading
from pathlib import Path


CONNECT = bytes([
    0x10, 0x11, 0x00, 0x04, ord("M"), ord("Q"), ord("T"), ord("T"),
    0x04, 0x02, 0x00, 0x3C, 0x00, 0x05, ord("g"), ord("l"), ord("a"),
    ord("s"), ord("s")
])
CONNACK = bytes([0x20, 0x02, 0x00, 0x00])


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.PIPE, text=True)


def make_certs(directory, openssl):
    ca_key = directory / "ca.key"
    ca_cert = directory / "ca.pem"
    wrong_key = directory / "wrong-ca.key"
    wrong_cert = directory / "wrong-ca.pem"
    server_key = directory / "server.key"
    server_csr = directory / "server.csr"
    server_cert = directory / "server.pem"
    extensions = directory / "server.ext"
    run(openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-days", "2", "-subj", "/CN=nativeTv-test-CA", "-keyout", str(ca_key),
        "-out", str(ca_cert))
    run(openssl, "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-days", "2", "-subj", "/CN=nativeTv-wrong-test-CA", "-keyout",
        str(wrong_key), "-out", str(wrong_cert))
    run(openssl, "req", "-new", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-subj", "/CN=127.0.0.1", "-keyout", str(server_key), "-out", str(server_csr))
    extensions.write_text("subjectAltName=IP:127.0.0.1\nextendedKeyUsage=serverAuth\n")
    run(openssl, "x509", "-req", "-in", str(server_csr), "-CA", str(ca_cert),
        "-CAkey", str(ca_key), "-CAcreateserial", "-out", str(server_cert),
        "-days", "2", "-sha256", "-extfile", str(extensions))
    return server_cert, server_key, ca_cert, wrong_cert


class Broker:
    def __init__(self, cert, key):
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(5)
        self.port = self.listener.getsockname()[1]
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(str(cert), str(key))
        self.context = context
        self.packet = None
        self.error = None
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        try:
            raw, _ = self.listener.accept()
            raw.settimeout(3)
            with self.context.wrap_socket(raw, server_side=True) as client:
                header = self.read_exact(client, 2)
                remaining = header[1]
                self.packet = header + self.read_exact(client, remaining)
                client.sendall(CONNACK)
        except (OSError, ssl.SSLError) as error:
            self.error = error
        finally:
            self.listener.close()

    @staticmethod
    def read_exact(client, size):
        result = bytearray()
        while len(result) < size:
            chunk = client.recv(size - len(result))
            if not chunk:
                raise OSError("client closed before MQTT packet was complete")
            result.extend(chunk)
        return bytes(result)

    def finish(self):
        self.thread.join(6)
        if self.thread.is_alive():
            raise AssertionError("local TLS broker did not stop")


def main():
    import sys
    if len(sys.argv) != 3:
        raise SystemExit("usage: mqtt_tls_fixture.py MQTT_TLS_PROBE OPENSSL")
    probe = os.path.abspath(sys.argv[1])
    openssl = os.path.abspath(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="nativetv-mqtt-tls-") as temporary:
        directory = Path(temporary)
        cert, key, trusted_ca, wrong_ca = make_certs(directory, openssl)

        trusted = Broker(cert, key)
        result = subprocess.run([probe, "https://127.0.0.1:%d/" % trusted.port,
                                 str(trusted_ca)], capture_output=True, text=True,
                                 timeout=7)
        trusted.finish()
        if result.returncode != 0:
            raise AssertionError("trusted custom CA failed: " + result.stderr)
        if trusted.packet != CONNECT:
            raise AssertionError("client did not send the expected MQTT CONNECT packet")

        untrusted = Broker(cert, key)
        result = subprocess.run([probe, "https://127.0.0.1:%d/" % untrusted.port,
                                 str(wrong_ca)], capture_output=True, text=True,
                                 timeout=7)
        untrusted.finish()
        if result.returncode == 0:
            raise AssertionError("TLS accepted a server certificate from the wrong CA")
        if "TLS connection failed" not in result.stderr:
            raise AssertionError("untrusted TLS failure was not reported: " + result.stderr)

    print("custom CA TLS and MQTT CONNECT/CONNACK checks passed")


if __name__ == "__main__":
    main()
