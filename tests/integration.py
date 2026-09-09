"""Exercise the real processes and wire protocol using only Python's standard library."""
import json
from contextlib import closing
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import urlopen

BIN = Path(sys.argv.pop(1)).resolve()


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def eventually(check, timeout=8):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            result = check()
            if result:
                return result
        except (OSError, sqlite3.Error, ValueError) as error:
            last = error
        time.sleep(0.05)
    raise AssertionError(f"condition did not become true: {last}")


def mqtt_packet(sock):
    def read(size):
        result = b""
        while len(result) < size:
            chunk = sock.recv(size - len(result))
            if not chunk:
                raise OSError("MQTT connection closed")
            result += chunk
        return result

    kind = read(1)[0]
    size, multiplier = 0, 1
    for _ in range(4):
        byte = read(1)[0]
        size += (byte & 127) * multiplier
        if byte < 128:
            return kind, read(size)
        multiplier *= 128
    raise AssertionError("invalid MQTT remaining length")


def subscribe(broker_port):
    sock = socket.create_connection(("127.0.0.1", broker_port), timeout=2)
    try:
        sock.sendall(b"\x10\x12\x00\x04MQTT\x04\x02\x00\x1e\x00\x06mbtest")
        assert mqtt_packet(sock) == (0x20, b"\x00\x00")
        topic = b"meterbridge/readings"
        body = b"\x00\x01" + struct.pack(">H", len(topic)) + topic + b"\x01"
        sock.sendall(bytes([0x82, len(body)]) + body)
        assert mqtt_packet(sock) == (0x90, b"\x00\x01\x01")
        return sock
    except Exception:
        sock.close()
        raise


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="meterbridge-test-")
        self.directory = Path(self.temp.name)
        self.db = self.directory / "readings.db"
        self.processes = []
        self.logs = []
        self.meter_port = port()
        self.http_port = port()

    def tearDown(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for process, log in zip(self.processes, self.logs):
            if process.returncode and process.returncode > 0:
                log.seek(0)
                print(log.read(), file=sys.stderr)
            log.close()
        self.temp.cleanup()

    def start(self, name, *args):
        log = open(self.directory / f"{Path(name).name}-{len(self.logs)}.log", "w+")
        self.logs.append(log)
        executable = str(BIN / name) if (BIN / name).exists() else name
        process = subprocess.Popen([executable, *map(str, args)], stdout=log, stderr=log)
        self.processes.append(process)
        return process

    def sim(self):
        return self.start("meter-sim", "--port", self.meter_port)

    def gateway(self, *args):
        return self.start("meterbridge", "--meter-port", self.meter_port,
                          "--db", self.db, "--interval-ms", 100, "--http-port", self.http_port, *args)

    def api(self, path):
        try:
            response = urlopen(f"http://127.0.0.1:{self.http_port}{path}", timeout=2)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.load(response)

    def rows(self):
        with closing(sqlite3.connect(self.db)) as db:
            return db.execute("SELECT id,voltage_v,current_a,power_w,energy_wh FROM readings ORDER BY id").fetchall()

    def test_poll_persist_and_reconnect(self):
        meter = self.sim()
        gateway = self.gateway()
        eventually(lambda: len(self.rows()) >= 3)
        self.assertEqual(self.rows()[0][1:4], (230.0, 5.0, 1150))
        eventually(lambda: self.api("/health")[0] == 200)
        meter.terminate()
        self.assertEqual(meter.wait(3), 0)
        eventually(lambda: self.api("/health")[0] == 503)
        time.sleep(0.3)
        count = len(self.rows())
        time.sleep(0.3)
        self.assertEqual(len(self.rows()), count)
        self.sim()
        eventually(lambda: len(self.rows()) > count)
        gateway.terminate()
        self.assertEqual(gateway.wait(3), 0)
        previous_id = self.rows()[-1][0]
        self.gateway()
        eventually(lambda: self.rows()[-1][0] > previous_id)

    def test_rest_validation_and_retention(self):
        self.sim()
        self.gateway("--max-rows", 3)
        eventually(lambda: self.api("/health")[1]["samples"] >= 6)
        status, readings = self.api("/readings?limit=2")
        self.assertEqual(status, 200)
        self.assertEqual(len(readings), 2)
        self.assertGreater(readings[0]["id"], readings[1]["id"])
        self.assertEqual(len(self.rows()), 3)
        for limit in ("0", "101", "-1", "abc", "2x", ""):
            self.assertEqual(self.api(f"/readings?limit={limit}")[0], 400)
        self.assertEqual(self.api("/health")[1]["pending"], 0)

    def test_mqtt_durable_replay_and_broker_recovery(self):
        self.sim()
        broker_port = port()
        args = ("--mqtt-host", "127.0.0.1", "--mqtt-port", broker_port)
        gateway = self.gateway(*args)
        eventually(lambda: self.api("/health")[1]["pending"] >= 3)
        self.assertEqual(self.api("/health")[0], 503)
        gateway.kill()  # WAL/outbox must survive an unclean exit.
        gateway.wait(3)
        pending_ids = {row[0] for row in self.rows()}
        broker = self.start("mosquitto", "-p", broker_port)
        with eventually(lambda: subscribe(broker_port)) as subscriber:
            self.gateway(*args)
            received = set()
            deadline = time.monotonic() + 8
            while not pending_ids <= received and time.monotonic() < deadline:
                kind, payload = mqtt_packet(subscriber)
                self.assertEqual(kind & 0xF6, 0x32)  # QoS 1 PUBLISH, DUP permitted.
                topic_size = struct.unpack(">H", payload[:2])[0]
                offset = 2 + topic_size
                packet_id = payload[offset:offset + 2]
                reading = json.loads(payload[offset + 2:])
                self.assertEqual(reading["voltage_v"], 230)
                received.add(reading["id"])
                subscriber.sendall(b"\x40\x02" + packet_id)
            self.assertTrue(pending_ids <= received)
        eventually(lambda: self.api("/health")[1]["pending"] == 0)
        broker.terminate()
        self.assertEqual(broker.wait(3), 0)
        eventually(lambda: self.api("/health")[1]["pending"] >= 3)
        self.start("mosquitto", "-p", broker_port)
        eventually(lambda: self.api("/health")[1]["pending"] == 0, timeout=12)
        eventually(lambda: self.api("/health")[0] == 200)

    def test_outbox_backpressure(self):
        self.sim()
        self.gateway("--max-rows", 3, "--mqtt-host", "127.0.0.1", "--mqtt-port", port())
        eventually(lambda: self.api("/health")[1]["poll_errors"] > 0)
        status, health = self.api("/health")
        self.assertEqual(status, 503)
        self.assertFalse(health["storage_ok"])
        self.assertEqual(health["pending"], 3)
        self.assertEqual(len(self.rows()), 3)

    def test_modbus_wire_contract(self):
        self.sim()

        def connect():
            return socket.create_connection(("127.0.0.1", self.meter_port), timeout=1)

        with eventually(connect) as sock:
            def request(pdu):
                sock.sendall(struct.pack(">HHHB", 17, 0, len(pdu) + 1, 1) + pdu)

                def read(size):
                    result = b""
                    while len(result) < size:
                        chunk = sock.recv(size - len(result))
                        if not chunk:
                            raise AssertionError("truncated Modbus response")
                        result += chunk
                    return result

                tid, protocol, size, unit = struct.unpack(">HHHB", read(7))
                self.assertEqual((tid, protocol, unit), (17, 0, 1))
                return read(size - 1)

            pdu = request(struct.pack(">BHH", 3, 0, 6))
            self.assertEqual(pdu[:2], bytes([3, 12]))
            self.assertEqual(struct.unpack(">6H", pdu[2:])[:4], (2300, 500, 0, 1150))
            self.assertEqual(request(struct.pack(">BHH", 3, 6, 1)), bytes([0x83, 2]))
            self.assertEqual(request(struct.pack(">BHH", 6, 0, 999)), bytes([0x86, 1]))


if __name__ == "__main__":
    unittest.main(verbosity=2)
