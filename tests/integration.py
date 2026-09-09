"""Exercise the real processes and wire protocol using only Python's standard library."""
import json
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import unittest

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


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="meterbridge-test-")
        self.directory = Path(self.temp.name)
        self.db = self.directory / "readings.db"
        self.processes = []
        self.logs = []
        self.meter_port = port()

    def tearDown(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in self.logs:
            log.close()
        self.temp.cleanup()

    def start(self, name, *args):
        log = open(self.directory / f"{name}-{len(self.logs)}.log", "w+")
        self.logs.append(log)
        process = subprocess.Popen([str(BIN / name), *map(str, args)], stdout=log, stderr=log)
        self.processes.append(process)
        return process

    def sim(self):
        return self.start("meter-sim", "--port", self.meter_port)

    def gateway(self, *args):
        return self.start("meterbridge", "--meter-port", self.meter_port,
                          "--db", self.db, "--interval-ms", 100, *args)

    def rows(self):
        with sqlite3.connect(self.db) as db:
            return db.execute("SELECT id,voltage_v,current_a,power_w,energy_wh FROM readings ORDER BY id").fetchall()

    def test_poll_persist_and_reconnect(self):
        meter = self.sim()
        gateway = self.gateway()
        eventually(lambda: len(self.rows()) >= 3)
        self.assertEqual(self.rows()[0][1:4], (230.0, 5.0, 1150))
        meter.terminate()
        self.assertEqual(meter.wait(3), 0)
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
