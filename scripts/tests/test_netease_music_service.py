import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class NeteaseMusicServiceTest(unittest.TestCase):
    def test_mcp_voice_intents_are_registered(self):
        source = (ROOT / "main" / "mcp_server.cc").read_text(encoding="utf-8")
        self.assertIn('"self.netease_music.login"', source)
        self.assertIn('"self.netease_music.logout"', source)
        self.assertIn("我要登录网易云音乐", source)
        self.assertIn("退出网易云音乐", source)

    def test_manager_api_client_is_wired_with_device_auth(self):
        source = (ROOT / "main" / "music" / "netease_music_device.cc").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("UnavailableClient", source)
        for value in (
            '"Device-Id"',
            '"Client-Id"',
            '"Authorization"',
            '"/device/netease/status"',
            '"/device/netease/sessions"',
            '"/device/netease/logout"',
        ):
            self.assertIn(value, source)

    def test_state_machine(self):
        source = ROOT / "main" / "music" / "netease_music_service.cc"
        test = ROOT / "scripts" / "tests" / "cpp" / "netease_music_service_test.cc"
        include = ROOT / "main" / "music"
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "netease_music_service_test"
            if shutil.which("g++"):
                command = [
                    "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    f"-I{include}", str(source), str(test), "-o", str(executable),
                ]
                subprocess.run(command, check=True)
                subprocess.run([str(executable)], check=True)
            elif os.name == "nt" and shutil.which("wsl.exe"):
                def wsl_path(path):
                    resolved = Path(path).resolve()
                    drive = resolved.drive.rstrip(":").lower()
                    suffix = resolved.as_posix().split(":", 1)[1]
                    return f"/mnt/{drive}{suffix}"

                linux_executable = wsl_path(executable)
                command = [
                    "wsl.exe", "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    f"-I{wsl_path(include)}", wsl_path(source), wsl_path(test),
                    "-o", linux_executable,
                ]
                subprocess.run(command, check=True)
                subprocess.run(["wsl.exe", linux_executable], check=True)
            else:
                self.skipTest("a host C++ compiler is unavailable")

    def test_lyrics_timeline(self):
        source = ROOT / "main" / "music" / "netease_music_lyrics.cc"
        test = ROOT / "scripts" / "tests" / "cpp" / "netease_music_lyrics_test.cc"
        include = ROOT / "main" / "music"
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "netease_music_lyrics_test"
            if shutil.which("g++"):
                command = [
                    "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    f"-I{include}", str(source), str(test), "-o", str(executable),
                ]
                subprocess.run(command, check=True)
                subprocess.run([str(executable)], check=True)
            elif os.name == "nt" and shutil.which("wsl.exe"):
                def wsl_path(path):
                    resolved = Path(path).resolve()
                    drive = resolved.drive.rstrip(":").lower()
                    suffix = resolved.as_posix().split(":", 1)[1]
                    return f"/mnt/{drive}{suffix}"

                linux_executable = wsl_path(executable)
                command = [
                    "wsl.exe", "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    f"-I{wsl_path(include)}", wsl_path(source), wsl_path(test),
                    "-o", linux_executable,
                ]
                subprocess.run(command, check=True)
                subprocess.run(["wsl.exe", linux_executable], check=True)
            else:
                self.skipTest("a host C++ compiler is unavailable")

    def test_lyrics_contract_is_wired_to_pcm_and_lower_screen(self):
        application = (ROOT / "main" / "application.cc").read_text(encoding="utf-8")
        audio = (ROOT / "main" / "audio" / "audio_service.cc").read_text(encoding="utf-8")
        display = (ROOT / "main" / "display" / "lcd_display.cc").read_text(encoding="utf-8")
        self.assertIn('"notifications/netease_music/lyrics"', application)
        self.assertIn("packet->lyrics_generation", application)
        self.assertIn("on_pcm_rendered(task->lyrics_generation", audio)
        self.assertIn("const int page_top = height_ / 2", display)
        self.assertIn("LV_LABEL_LONG_CLIP", display)
        self.assertIn("lv_obj_set_style_transform_pivot_x", display)
        self.assertIn("netease_lyrics_page_ == nullptr", display)


if __name__ == "__main__":
    unittest.main()
