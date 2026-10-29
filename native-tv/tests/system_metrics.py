"""Native snapshot API acceptance with real procfs fixtures and filesystem reads."""
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

BINARY = str(Path(sys.argv.pop(1)).resolve())
FIXTURE = json.loads((Path(__file__).parent / "fixtures/system-metrics.json").read_text())
HDMI_CASES = json.loads((Path(__file__).parent / "fixtures/hdmi-status-cases.json").read_text())["cases"]
REPO_ROOT = Path(__file__).resolve().parents[2]


class SystemTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        for name, text in FIXTURE["files"].items():
            self.put(name, text)

    def tearDown(self):
        self.temporary.cleanup()

    def put(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def read(self, root=None, data_root=None):
        result = subprocess.run([BINARY, "system", "--root", str(root or self.root),
                                 "--data-root", str(data_root or self.root / "data")],
                                capture_output=True, text=True, check=True, timeout=5)
        envelope = json.loads(result.stdout)
        self.assertEqual(envelope["protocol"], 1)
        self.assertEqual(result.stderr, "")
        return envelope["result"]

    def test_public_snapshot(self):
        self.assertEqual(self.read(), FIXTURE["expected"])

    def test_missing_files_are_unknown(self):
        result = self.read(self.root / "absent")
        self.assertTrue(result["ok"])
        self.assertEqual(result["mem"], {"total": 0, "avail": 0})
        self.assertEqual(result["swap"], {"total": 0, "free": 0, "backing": None})
        for key in ("temp", "tempMillidegrees", "cpuHz", "mhz", "gpuHz", "gpuMhz", "appStorage"):
            self.assertIsNone(result[key], key)
        self.assertEqual(result["cpu"], {
            "times": {"cpus": {}, "btime": None},
            "coresTotal": 0,
            "coreLoads": [],
            "currentLoad": None
        })
        self.assertEqual(result["uptime"], 0)
        self.assertEqual(result["loadavg"], [])
        self.assertIsNone(result["net"])
        self.assertIsNone(result["wifi"])
        self.assertIsNone(result["pictureEngine"])
        self.assertFalse(result["hasHdrStatus"])
        self.assertFalse(result["hasHdmiProc"])
        self.assertIsNone(result["socArch"])
        self.assertFalse(result["hasPnwash"])
        self.assertEqual(result["hdmiPorts"], [None, None, None, None])
        self.assertEqual(result["hdmiPortFacts"], [None, None, None, None])
        self.assertIsNone(result["remote"])
        self.assertFalse(result["thermalPresent"])
        self.assertFalse(result["emmcWearPresent"])
        self.assertEqual(result["emmc"], dict(life="unknown", wear="unknown", health="unknown",
                                              eol="unknown", life_est_a=None, life_est_b=None))

    def test_cpu_times_modes_and_boot_time(self):
        self.put("proc/stat", "cpu  1 2 3 4 5 6 7 8 9 10\n"
                              "cpu0 10 0 20 30 1 0 2 0 5 6\n"
                              "cpu7 1 bad 3 4 5 6 7 8\n"
                              "intr 123\nbtime 1791204999\n")
        result = self.read()["cpu"]["times"]
        self.assertEqual(result["cpus"]["0"], {
            "user": 10, "nice": 0, "system": 20, "idle": 30,
            "iowait": 1, "irq": 0, "softirq": 2, "steal": 0
        }, "guest time is already counted in user and nice")
        self.assertEqual(result["cpus"]["7"], {
            "user": 1, "system": 3, "idle": 4, "iowait": 5,
            "irq": 6, "softirq": 7, "steal": 8
        }, "a malformed mode is left out without shifting the rest")
        self.assertEqual(result["btime"], 1791204999)
        self.put("proc/stat", "cpu 1 2 3\nbtime nope\n")
        self.assertEqual(self.read()["cpu"]["times"], {"cpus": {}, "btime": None})

    def test_online_loads_and_present_cores(self):
        self.put("proc/lg/pm/status", "cpu_num: 4\nload: 25 30 15 20\n")
        self.put("sys/devices/system/cpu/present", "0-3\n")
        self.put("sys/devices/system/cpu/online", "0,2-3\n")
        result = self.read()["cpu"]
        self.assertEqual(result["coreLoads"], [25, 15, 20], "slots follow the online ids")
        self.assertEqual(result["coresTotal"], 4)
        (self.root / "sys/devices/system/cpu/online").unlink()
        result = self.read()["cpu"]
        self.assertEqual(result["coreLoads"], [25, 30, 15, 20], "cpu_num is the fallback")
        (self.root / "sys/devices/system/cpu/present").unlink()
        self.assertEqual(self.read()["cpu"]["coresTotal"], 4, "the slots count without present")
        (self.root / "proc/lg/pm/status").unlink()
        result = self.read()["cpu"]
        self.assertEqual(result["coreLoads"], [])
        self.assertEqual(result["coresTotal"], 0)

    def test_network_rank_and_mac(self):
        self.put("sys/class/net/eth0/address", "AA:BB:CC:DD:EE:01\n")
        self.put("sys/class/net/eth0/operstate", "up\n")
        self.assertEqual(self.read()["net"], {
            "iface": "eth0", "rx": 100000000, "tx": 50000000,
            "mac": "aa:bb:cc:dd:ee:01"
        }, "equal ranks go to the larger receiver")
        (self.root / "sys/class/net/eth0/operstate").unlink()
        (self.root / "sys/class/net/wlan0/operstate").unlink()
        self.put("sys/class/net/wlan0/carrier", "1\n")
        self.assertEqual(self.read()["net"]["iface"], "wlan0", "a carrier ranks an absent operstate")
        self.put("sys/class/net/wlan0/address", "00:00:00:00:00:00\n")
        self.assertIsNone(self.read()["net"]["mac"], "an unset MAC is unknown")
        self.put("sys/class/net/wlan0/address", "not a mac\n")
        self.assertIsNone(self.read()["net"]["mac"])
        self.put("proc/net/dev", " face |bytes packets\n  lo: 1 2 3 4 5 6 7 8 9\n")
        self.assertIsNone(self.read()["net"], "loopback alone is no interface")
        self.put("proc/net/dev", "wlan0: bad 0 0 0 0 0 0 0 9\n")
        self.assertIsNone(self.read()["net"])

    def test_wifi_units_and_absent_readings(self):
        self.put("proc/net/wireless", "wlan0: 0000   70.  193.  -256\n")
        self.assertEqual(self.read()["wifi"], {"link": 70, "level": -63},
                         "a positive unsigned RSSI is read as signed dBm")
        self.put("proc/net/wireless", "wlan0: 0000  0.  0.  -256\n")
        self.assertIsNone(self.read()["wifi"])
        self.put("proc/net/wireless", "eth0: 0000 70. -63.\n")
        self.assertIsNone(self.read()["wifi"])

    def test_picture_engine_window_and_hdr(self):
        self.assertEqual(self.read()["pictureEngine"],
                         {"colorimetry": "BT.709", "hdr_mode": "sdr"},
                         "window [0], not the other picture windows")
        self.put("proc/lg/pe/hdr_status",
                 "[0]{hdrStatus:hdr10(hdr10),colorimetry:bt2020}\n")
        self.assertEqual(self.read()["pictureEngine"],
                         {"colorimetry": "BT.2020", "hdr_mode": "hdr10"})
        self.put("proc/lg/pe/hdr_status",
                 "VPQ_PQ_MODE_INFO=\nhdrStatus:hlg (hlg), colorimetry: bt601\n")
        self.assertEqual(self.read()["pictureEngine"],
                         {"colorimetry": "BT.601", "hdr_mode": "hlg"},
                         "an unindexed file is read whole")
        self.put("proc/lg/pe/hdr_status",
                 "[0]{hdrStatus: hdr10, colorimetry: BT709 }\n")
        self.assertEqual(self.read()["pictureEngine"],
                         {"colorimetry": "BT.709", "hdr_mode": "hdr10"},
                         "mapping and parentheses are optional")

    def test_picture_engine_unknown_and_absent(self):
        self.put("proc/lg/pe/hdr_status", "[0]{colorimetry:future_space}\n")
        self.assertEqual(self.read()["pictureEngine"],
                         {"colorimetry": "future_space", "hdr_mode": None},
                         "unknown colourimetry keeps the TV's spelling")
        self.put("proc/lg/pe/hdr_status", "nothing useful\n")
        self.assertEqual(self.read()["pictureEngine"],
                         {"colorimetry": None, "hdr_mode": None})
        (self.root / "proc/lg/pe/hdr_status").unlink()
        self.assertIsNone(self.read()["pictureEngine"])

    def test_hdmi_proc_capability(self):
        result = self.read()
        self.assertTrue(result["hasHdmiProc"])
        self.assertEqual(result["hdmiPorts"], ["", None, None, None])
        (self.root / "proc/lg/hdmi20/port0/status").unlink()
        (self.root / "proc/lg/hdmi20/port0").rmdir()
        (self.root / "proc/lg/hdmi20").rmdir()
        result = self.read()
        self.assertFalse(result["hasHdmiProc"])
        self.assertEqual(result["hdmiPorts"], [None, None, None, None])

    def test_hdmi_status_facts_match_existing_node_fixtures(self):
        for case in HDMI_CASES:
            status = (REPO_ROOT / case["source"]).read_text()
            self.put("proc/lg/hdmi20/port0/status", status)
            result = self.read()
            self.assertEqual(result["hdmiPorts"][0], status, case["name"] + " keeps raw text")
            self.assertEqual(result["hdmiPortFacts"][0], case["expected"], case["name"])
            self.assertEqual(result["hdmiPortFacts"][1:], [None, None, None], case["name"] + " leaves missing ports unknown")

    def test_hdr_status_capability(self):
        self.assertTrue(self.read()["hasHdrStatus"])
        (self.root / "proc/lg/pe/hdr_status").unlink()
        result = self.read()
        self.assertFalse(result["hasHdrStatus"])
        self.assertIsNone(result["pictureEngine"])

    def test_remote_pairing_facts(self):
        self.assertEqual(self.read()["remote"], {
            "battery": None,
            "model": "LGE MR24",
            "mac": "00:00:5e:00:53:02",
            "firmware": "23.63(H).04.1.0.192.7.1.0.62.1",
            "paired": True
        }, "the threshold pair is not a battery level")
        self.put("mnt/lg/cmn_data/mrcu/mrcu1.info", "Name = LGE MR20\nBattery = 80\n")
        self.assertEqual(self.read()["remote"]["battery"], 80, "a plain level is read")
        self.put("mnt/lg/cmn_data/mrcu/mrcu1.info",
                 "battery = 52 (M) 35 (L)\nname = LGE MR24\n")
        result = self.read()["remote"]
        self.assertIsNone(result["battery"], "spaces before the threshold marker still count")
        self.assertEqual(result["model"], "LGE MR24", "keys are case-insensitive")
        for text in ("Battery = nope\nhidraw = /dev/hidraw0\n", "hidraw = /dev/hidraw0\n"):
            self.put("mnt/lg/cmn_data/mrcu/mrcu1.info", text)
            self.assertIsNone(self.read()["remote"], text)
        (self.root / "mnt/lg/cmn_data/mrcu/mrcu1.info").unlink()
        self.put("mnt/lg/cmn_data/btsvc/mrcu1.info", "Battery = 0\r\n")
        self.assertEqual(self.read()["remote"],
                         {"battery": 0, "model": None, "mac": None,
                          "firmware": None, "paired": True},
                         "the fallback path is read, with a zero level")

    def test_remote_saved_battery_is_address_checked(self):
        data = self.root / "data"
        data.mkdir()
        state = data / "remote-battery.json"
        state.write_text(json.dumps({"level": 73, "mac": "00:00:5E:00:53:02",
                                     "connected": True, "isBle": True,
                                     "lowStrength": False, "firmware": "cached-fw"}))
        remote = self.read()["remote"]
        self.assertEqual(remote["battery"], 73,
                         "a cached reading for the paired remote is preferred")
        self.assertTrue(remote["connected"])
        self.assertTrue(remote["bluetooth"])
        self.assertEqual(remote["signal"], "good")
        self.assertEqual(remote["firmware"], "23.63(H).04.1.0.192.7.1.0.62.1",
                         "the pairing facts keep priority over cached firmware")
        state.write_text(json.dumps({"level": 73, "mac": "00:00:5e:00:53:03"}))
        self.assertIsNone(self.read()["remote"]["battery"],
                          "a cached reading from another remote is ignored")
        state.write_text(json.dumps({"level": 101, "mac": "00:00:5e:00:53:02"}))
        self.assertIsNone(self.read()["remote"]["battery"],
                          "out-of-range saved readings are ignored")

    def test_uptime_and_load_average(self):
        self.put("proc/uptime", "99999.99 123.4\n")
        self.put("proc/loadavg", "0.10 0.20\n")
        result = self.read()
        self.assertEqual(result["uptime"], 99999)
        self.assertEqual(result["loadavg"], ["0.10", "0.20"])
        self.put("proc/uptime", "not-a-time\n")
        self.put("proc/loadavg", "1.0 broken 2.0\n")
        result = self.read()
        self.assertEqual(result["uptime"], 0)
        self.assertEqual(result["loadavg"], ["1.0"], "tokens after a malformed one are dropped")

    def test_thermal_source_and_units(self):
        self.put("sys/class/thermal/thermal_zone0/temp", "68001\n")
        self.assertEqual(self.read()["tempMillidegrees"], 48000, "LG file takes precedence")
        (self.root / "proc/lg/pm/temperature").unlink()
        result = self.read()
        self.assertEqual(result["tempMillidegrees"], 68001)
        self.assertEqual(result["temp"], 68)
        self.assertTrue(result["thermalPresent"])
        for text in ("0", "-2000", "bad", "68000 trailing", "99999999999999999999999"):
            self.put("sys/class/thermal/thermal_zone0/temp", text)
            self.assertIsNone(self.read()["temp"], text)
        self.put("sys/class/thermal/thermal_zone0/temp", "68500")
        self.assertEqual(self.read()["temp"], 69)

    def test_clock_units_and_bounds(self):
        for raw, hz, mhz in (("1200", 1200000000, 1200), ("1400500", 1400500000, 1401),
                             ("100", 100000000, 100), ("10000", 10000000000, 10000),
                             ("50", None, None), ("10001", None, None), ("-1", None, None),
                             ("1200junk", None, None), ("18446744073709551616", None, None)):
            with self.subTest(raw=raw):
                self.put("proc/lg/pm/frequency", raw)
                result = self.read()
                self.assertEqual((result["cpuHz"], result["mhz"]), (hz, mhz))
        self.put("proc/lg/sys/status", "GPU PLL OUT : 550500000\n")
        self.assertEqual((self.read()["gpuHz"], self.read()["gpuMhz"]), (550500000, 551))
        self.put("proc/lg/sys/status", "gpu pll out: nope\n")
        self.assertIsNone(self.read()["gpuHz"])

    def test_emmc_regions_and_eol(self):
        self.put("sys/block/mmcblk0/device/life_time", "0x02 0x05")
        self.put("sys/block/mmcblk0/device/pre_eol_info", "0x02")
        result = self.read()["emmc"]
        self.assertEqual(result, dict(life="10-20% / 40-50%", wear="10-20% / 40-50%",
                                     health="50% remaining", eol="Warning", life_est_a=2, life_est_b=5))
        self.put("sys/block/mmcblk0/device/life_time", "0x0b 0x0b")
        self.put("sys/block/mmcblk0/device/pre_eol_info", "0x03")
        self.assertEqual(self.read()["emmc"]["wear"], ">100%")
        self.assertEqual(self.read()["emmc"]["health"], "0% remaining")
        self.assertEqual(self.read()["emmc"]["eol"], "Urgent")
        self.put("sys/block/mmcblk0/device/life_time", "0x00 invalid")
        self.assertEqual(self.read()["emmc"]["health"], "unknown")
        self.assertIsNone(self.read()["emmc"]["life_est_a"])
        self.assertIsNone(self.read()["emmc"]["life_est_b"])

    def test_memory_and_largest_swap(self):
        self.put("proc/meminfo", "MemTotal: 1000 kB\nMemAvailable: -3 kB\nSwapTotal: junk kB\nSwapFree: 9 MB\n")
        self.put("proc/swaps", "Filename Type Size Used Priority\n/dev/zram0 partition 200 10 1\n/swap file 800 0 0\n/dev/flash partition 500 0 0\n")
        result = self.read()
        self.assertEqual(result["mem"], {"total": 1000, "avail": 0})
        self.assertEqual(result["swap"], {"total": 0, "free": 0, "backing": "file"})
        self.put("proc/swaps", "Filename Type Size Used Priority\n/dev/flash partition 900 0 0\n")
        self.assertEqual(self.read()["swap"]["backing"], "flash")

    def test_bounded_files(self):
        self.put("proc/meminfo", "MemTotal: 42 kB\n" + "x" * 65536)
        self.assertEqual(self.read()["mem"]["total"], 0)

    def test_storage_uses_filesystem_counters(self):
        path = self.root / "mnt/lg/appstore"
        path.mkdir(parents=True)
        info = os.statvfs(path)
        storage = self.read()["appStorage"]
        total = info.f_blocks * info.f_frsize // 1024
        self.assertEqual(storage["totalKb"], total)
        self.assertEqual(storage["totalMb"], math.floor(total / 1024 + 0.5))
        self.assertGreaterEqual(storage["availKb"], 0)
        self.assertLessEqual(storage["availKb"], total)
        # Filesystem free space may change during the subprocess; allow that without
        # substituting an unavailable mount with the host root filesystem.
        after = os.statvfs(path)
        expected = after.f_bavail * after.f_frsize // 1024
        self.assertLessEqual(abs(storage["availKb"] - expected), 65536)
        path.rmdir()
        self.assertIsNone(self.read()["appStorage"])

    def test_fixed_cli_operations(self):
        for args in (["system", "--all"], ["system", "--proc-root", str(self.root)],
                     ["cpu", "--root", str(self.root)], ["system", "--root"], ["system", "cpu"]):
            self.assertNotEqual(subprocess.run([BINARY] + args, capture_output=True, timeout=5).returncode, 0)


if __name__ == "__main__":
    unittest.main()
