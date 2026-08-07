import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class ScheduleManagerTest(unittest.TestCase):
    def test_host_schedule_core(self):
        if not shutil.which("g++"):
            self.skipTest("a host C++ compiler is unavailable")
        source = ROOT / "main" / "schedule" / "schedule_manager.cc"
        test = ROOT / "scripts" / "tests" / "cpp" / "schedule_manager_test.cc"
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "schedule_manager_test"
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{source.parent}",
                    str(source),
                    str(test),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)

    def test_device_integration_contract(self):
        application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        mcp = (ROOT / "main" / "mcp_server.cc").read_text(encoding="utf-8")
        board = (
            ROOT
            / "main/boards/waveshare/esp32-s3-touch-amoled-1.32"
            / "esp32-s3-touch-amoled-1.32.cc"
        ).read_text(encoding="utf-8")
        self.assertIn('"notifications/schedule/triggered"', application)
        self.assertIn("speak", application)
        for tool in ("create", "list", "delete", "clear", "stop", "snooze"):
            self.assertIn(f'"self.schedule.{tool}"', mcp)
        self.assertIn("IsScheduleAlertActive", board)


if __name__ == "__main__":
    unittest.main()
