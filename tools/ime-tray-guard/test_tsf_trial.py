"""独立TSF实验的接口与进程生命周期测试；不把注册成功当作显示成功。"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

EXE = Path(os.environ.get("IME_TSF_TRIAL_EXE", "missing-tsf-trial.exe")).resolve()


@unittest.skipUnless(EXE.is_file(), "设置IME_TSF_TRIAL_EXE以执行真实TSF实验")
class TsfTrialTests(unittest.TestCase):
    def invoke(self, *args):
        return subprocess.run([str(EXE), *map(str,args)], capture_output=True, timeout=25)

    def rows(self, path):
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

    def test_help_does_not_register(self):
        for args in ((), ("--help",)):
            result = self.invoke(*args)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(b"NOT a guard", result.stdout)

    def test_self_test_contract(self):
        result = self.invoke("--self-test")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_run_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"never.jsonl"
            self.assertEqual(self.invoke("--out",path).returncode,1)
            self.assertFalse(path.exists())
            self.assertEqual(self.invoke("--run-trial").returncode,1)

    def test_invalid_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"never.jsonl"
            for extra in (("--seconds","0"),("--seconds","91"),("--seconds","5x"),
                          ("--seconds","999999999999999999999999999999"),
                          ("--baseline-seconds","1"),("--baseline-seconds","16"),
                          ("--unknown",),("--seconds",)):
                self.assertEqual(self.invoke("--run-trial","--out",path,*extra).returncode,1)
                self.assertFalse(path.exists())

    def test_never_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"original.jsonl"
            path.write_bytes(b"original\n")
            result = self.invoke("--run-trial","--out",path)
            self.assertEqual(result.returncode,1,result.stderr)
            self.assertEqual(path.read_bytes(),b"original\n")

    def test_stop_without_trial(self):
        self.assertEqual(self.invoke("--stop").returncode,4)

    def test_normal_lifecycle_unicode_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"中文 空格.jsonl"
            result = self.invoke("--run-trial","--out",path,"--seconds","5","--baseline-seconds","2")
            self.assertEqual(result.returncode,0,result.stderr)
            rows = self.rows(path)
            self.assertEqual([r["item"] for r in rows if r["phase"]=="add"],[0,1])
            self.assertEqual([r["item"] for r in rows if r["phase"]=="remove"],[1,0])
            absent = [r for r in rows if r["phase"]=="after_remove_get"]
            self.assertEqual(len(absent),2)
            self.assertTrue(all(r["absence_confirmed"] and r["hr"]==1 for r in absent))
            self.assertTrue(rows[-1]["cleanup_confirmed"])
            self.assertTrue(all(not r["protection_active"] and not r["visibility_verified"] for r in rows))

    def test_stop_and_reject_duplicate(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"stop.jsonl"
            child = subprocess.Popen([str(EXE),"--run-trial","--out",str(path),
                                      "--seconds","30","--baseline-seconds","2"],
                                      stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            try:
                limit = time.monotonic()+10
                while time.monotonic()<limit:
                    if path.exists() and '"phase":"registered"' in path.read_text(encoding="utf-8"):
                        break
                    self.assertIsNone(child.poll())
                    time.sleep(.05)
                else:
                    self.fail("未在期限内注册")
                second = Path(tmp)/"duplicate.jsonl"
                result = self.invoke("--run-trial","--out",second)
                self.assertEqual(result.returncode,4)
                self.assertFalse(second.exists())
                self.assertEqual(self.invoke("--stop").returncode,0)
                stdout,stderr = child.communicate(timeout=12)
                self.assertEqual(child.returncode,0,(stdout,stderr))
                rows = self.rows(path)
                self.assertIn("stop_requested",[r["phase"] for r in rows])
                self.assertTrue(rows[-1]["cleanup_confirmed"])
            finally:
                if child.poll() is None:
                    self.invoke("--stop")
                    try:
                        child.communicate(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.communicate()


if __name__ == "__main__":
    unittest.main()
