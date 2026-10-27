"""分析器反误判测试：合成数据不是实际占位验收。"""
import copy
import tempfile
import unittest
from pathlib import Path
from analyze import analyze, read_rows


def sample(second=0):
    return dict(schema=1, readonly=True, protection_active=False, native_only=False,
        uia_init_hr=0, utc=f"2026-10-24T08:00:{second:02d}Z",
        metadata=dict(session_id=1, console_session_id=1, wts_protocol=0, build="22631",
            ubr=1, explorer_file_version="10.0.22631.1", foreground_exe="notepad.exe",
            monitors=[dict(dpi_hr=0, dpi=[96,96])]),
        taskbars=[dict(hwnd=11, uia_hr=0, uia_errors=0, uia_truncated=False, native_truncated=False,
            native=[dict(hwnd=11, pid=22, rect_ok=1, rect=[0,900,1600,950], dpi=96,
                         visible=1, **{"class":"Shell_TrayWnd"})],
            uia=[dict(index=0, parent=-1, automation_id="SystemTrayIcon", control_type=50000,
                      rect_hr=0, rect=[1200,900,1232,950], offscreen=0, runtime_id=[42,11,4,1],
                      ime_candidate=False, **{"class":"SystemTray.NormalButton"})])])


class AnalysisTests(unittest.TestCase):
    def pair(self):
        return sample(), sample(1)

    def test_static_is_not_protection_pass(self):
        r = analyze(self.pair())
        self.assertEqual(r["comparable_pairs"], 1)
        self.assertEqual(r["max_rect_delta_px"], 0)
        self.assertFalse(r["protection_active"])
        self.assertIn("不构成", r["status"])

    def test_one_pixel_motion_is_detected(self):
        a,b = self.pair()
        b["taskbars"][0]["uia"][0]["rect"] = [1199,900,1231,950]
        r = analyze([a,b])
        self.assertEqual(r["max_rect_delta_px"], 1)
        self.assertEqual(len(r["moved_buttons"]), 1)

    def test_slow_drift_against_baseline(self):
        rows = [sample(i) for i in range(4)]
        for i,row in enumerate(rows):
            row["taskbars"][0]["uia"][0]["rect"][0] += i
        self.assertEqual(analyze(rows)["max_rect_delta_px"], 3)

    def test_missing_node_is_not_stationary(self):
        a,b = self.pair()
        extra = copy.deepcopy(a["taskbars"][0]["uia"][0])
        extra.update(index=1, runtime_id=[42,11,4,2])
        a["taskbars"][0]["uia"].append(extra)
        self.assertEqual(analyze([a,b])["node_set_changes"][0]["removed"], 1)

    def test_context_changes_reset_baseline(self):
        for key,value in [("session_id",2),("wts_protocol",2),("build","26300"),
                          ("explorer_file_version","10.0.26300.1"),
                          ("monitors",[dict(dpi_hr=0, dpi=[144,144])])]:
            with self.subTest(key=key):
                a,b = self.pair()
                b["metadata"][key] = value
                r = analyze([a,b])
                self.assertEqual(r["context_resets"], 1)
                self.assertEqual(r["comparable_pairs"], 0)

    def test_explorer_restart_resets(self):
        a,b = self.pair()
        b["taskbars"][0]["native"][0]["pid"] += 1
        self.assertEqual(analyze([a,b])["context_resets"], 1)

    def test_truncation_and_errors_reject(self):
        for key,value in [("uia_errors",1),("uia_hr",-1),("uia_truncated",True),("native_truncated",True)]:
            with self.subTest(key=key):
                a,b = self.pair()
                b["taskbars"][0][key] = value
                r = analyze([a,b])
                self.assertEqual(r["valid_samples"],1)
                self.assertEqual(r["comparable_pairs"],0)
                self.assertTrue(r["issues"])

    def test_duplicate_ids_reject(self):
        a = sample()
        other = copy.deepcopy(a["taskbars"][0]["uia"][0])
        other["index"] = 1
        a["taskbars"][0]["uia"].append(other)
        self.assertEqual(analyze([a])["valid_samples"],0)

    def test_bad_rectangle_rejects(self):
        for rect in ([1,2,0,3],[1,2,3],None,[1.5,2,3,4]):
            a = sample()
            a["taskbars"][0]["uia"][0]["rect"] = rect
            self.assertTrue(analyze([a])["issues"])

    def test_native_only_not_uia_success(self):
        a = sample()
        a["native_only"] = True
        self.assertEqual(analyze([a])["valid_samples"],0)

    def test_bad_sample_breaks_chain(self):
        r = analyze([sample(),{},sample(2)])
        self.assertEqual(r["valid_samples"],2)
        self.assertEqual(r["comparable_pairs"],0)

    def test_gap_flagged(self):
        self.assertTrue(analyze([sample(),sample(10)])["issues"])

    def test_ime_candidate_excluded_from_motion(self):
        a,b = self.pair()
        b["taskbars"][0]["uia"][0].update(ime_candidate=True,rect=[1100,900,1132,950])
        self.assertEqual(analyze([a,b])["max_rect_delta_px"],0)

    def test_offscreen_excluded(self):
        a,b = self.pair()
        b["taskbars"][0]["uia"][0].update(offscreen=1,rect=[0,0,0,0])
        self.assertEqual(analyze([a,b])["max_rect_delta_px"],0)

    def test_bad_file_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/"bad.jsonl"
            for data in ("", "{", "\n"):
                path.write_text(data, encoding="utf-8")
                with self.assertRaises(ValueError):
                    read_rows(path)


if __name__ == "__main__":
    unittest.main()
