"""真实任务栏入口在稳定性问题解决前必须拒绝；这些测试不附加Explorer。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

EXE=Path(os.environ.get("IME_CONTAINER_TRIAL_EXE","missing-container.exe")).resolve()

@unittest.skipUnless(EXE.is_file(),"设置IME_CONTAINER_TRIAL_EXE")
class ContainerTrialTests(unittest.TestCase):
    def invoke(self,*args):
        return subprocess.run([str(EXE),*map(str,args)],capture_output=True,timeout=10)

    def test_help_discloses_native_gate(self):
        result=self.invoke("--help")
        self.assertEqual(result.returncode,0)
        self.assertIn(b"RESEARCH",result.stdout)
        self.assertIn(b"DISABLED",result.stdout)
        self.assertIn(b"No global visual-tree subscription",result.stdout)
        self.assertIn(b"exact loaded-image identity required",result.stdout)

    def test_native_observation_and_writes_blocked_before_attachment(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"not-created.jsonl"
            for mode in ("--observe-taskbar","--reserve-taskbar"):
                result=self.invoke(mode,"--out",path)
                self.assertEqual(result.returncode,1,result.stderr)
                self.assertIn(b"no attachment attempted",result.stderr)
                self.assertFalse(path.exists())

    def test_foreign_process_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"not-created.jsonl"
            result=self.invoke("--fixture-pid",os.getpid(),"--reserve","--out",path)
            self.assertEqual(result.returncode,1)
            self.assertIn(b"Target identity mismatch",result.stderr)
            self.assertFalse(path.exists())

    def test_invalid_arguments(self):
        for args in (("--reserve",),("--fixture-pid","0"),("--seconds","91"),
                     ("--seconds","5x"),("--endpoint","17"),("--unknown",)):
            self.assertEqual(self.invoke(*args).returncode,1)

    def test_client_lease_requires_persistent_reservation(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"not-created.jsonl"
            result=self.invoke("--fixture-pid",os.getpid(),"--client-map","invalid","--out",path)
            self.assertEqual(result.returncode,1)
            self.assertIn(b"Client lease requires",result.stderr)
            self.assertFalse(path.exists())

    def test_client_mapping_prefix_and_absence_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"not-created.jsonl"
            for name in ("foreign-map","Local\\WindowMark.ImeGuard.Client.nonexistent-test"):
                result=self.invoke("--fixture-pid",os.getpid(),"--reserve","--persistent","--client-map",name,"--out",path)
                self.assertEqual(result.returncode,1)
                self.assertFalse(path.exists())

    def test_blocked_mode_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"evidence.jsonl"
            path.write_text("original",encoding="utf-8")
            self.assertEqual(self.invoke("--reserve-taskbar","--out",path).returncode,1)
            self.assertEqual(path.read_text(encoding="utf-8"),"original")

if __name__=="__main__":
    unittest.main()
