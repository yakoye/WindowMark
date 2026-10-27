"""原型 EXE 的参数、证据保护、真实只读运行测试；通过参数指定产物。"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

EXE = Path(os.environ["IME_PROBE_EXE"]).resolve()


class ProbeTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([str(EXE), *map(str,args)], capture_output=True, timeout=25)

    def test_help(self):
        self.assertEqual(self.invoke("--help").returncode,0)

    def test_missing_and_unknown_arguments(self):
        for args in ((),("--out",),("--unknown","x")):
            self.assertEqual(self.invoke(*args).returncode,1)

    def test_numeric_limits(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"must-not-exist.jsonl"
            for args in (("--samples","0"),("--samples","601"),("--samples","2x"),
                         ("--interval-ms","249"),("--delay-ms","30001")):
                self.assertEqual(self.invoke("--out",path,*args).returncode,1)
                self.assertFalse(path.exists())

    def test_never_overwrites_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"evidence.jsonl"
            path.write_bytes(b"original evidence\n")
            self.assertEqual(self.invoke("--out",path).returncode,1)
            self.assertEqual(path.read_bytes(),b"original evidence\n")

    def test_native_only_and_unicode_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"中文 空格"/"evidence.jsonl"
            result = self.invoke("--out",path,"--native-only","--label",'中文"测试')
            self.assertEqual(result.returncode,0,result.stderr)
            row = json.loads(path.read_text(encoding="utf-8"))
            self.assertTrue(row["native_only"])
            self.assertEqual(row["label"],'中文"测试')
            self.assertTrue(row["taskbars"])
            self.assertFalse(row["protection_active"])

    def test_real_uia_sample_privacy_and_quality(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"uia.jsonl"
            result = self.invoke("--out",path)
            self.assertEqual(result.returncode,0,result.stderr)
            row = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(row["uia_init_hr"],0)
            self.assertFalse(row["protection_active"])
            self.assertTrue(row["taskbars"])
            for bar in row["taskbars"]:
                self.assertEqual(bar["uia_errors"],0)
                self.assertFalse(bar["uia_truncated"])
                for node in bar["uia"]:
                    self.assertNotIn("TaskList",node["class"])
                    self.assertNotIn("ime_name",node)

    def test_app_geometry_never_records_titles_or_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"geometry.jsonl"
            result = self.invoke("--out",path,"--include-app-geometry")
            self.assertEqual(result.returncode,0,result.stderr)
            row = json.loads(path.read_text(encoding="utf-8"))
            for bar in row["taskbars"]:
                self.assertFalse(bar["uia_truncated"])
                self.assertEqual(bar["uia_errors"],0)
                for node in bar["uia"]:
                    if node["app_geometry"]:
                        self.assertEqual(node["automation_id"],"")
                        self.assertEqual(node["name_probe_hr"],1)
                        self.assertEqual(node["ime_hint"],"")
                    self.assertIn(node["anchor_role"],("", "Everything", "overflow", "volume", "network"))


if __name__ == "__main__":
    unittest.main()
