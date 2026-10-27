import copy
import unittest
from test_analyze import sample
from transitions import summarize


def with_two_slots(second=1):
    row = sample(second)
    original = row["taskbars"][0]["uia"][0]
    original["rect"] = [1105,900,1137,950]
    for index,rect in [(1,[1137,900,1177,950]),(2,[1177,900,1232,950])]:
        node = copy.deepcopy(original)
        node.update(index=index,rect=rect,runtime_id=[42,11,4,100+index+second*10])
        row["taskbars"][0]["uia"].append(node)
    return row


class TransitionTests(unittest.TestCase):
    def test_40_and_55_pixels_match_95_shift(self):
        report = summarize([sample(),with_two_slots()])
        event = report["transitions"][0]
        self.assertEqual(event["net_added_width_px"],95)
        self.assertEqual(event["horizontal_shifts"],[dict(dx_px=-95,count=1)])
        self.assertTrue(event["width_displacement_balanced"])
        self.assertFalse(report["semantic_identity_confirmed"])

    def test_disappearance_moves_right(self):
        event = summarize([with_two_slots(0),sample(1)])["transitions"][0]
        self.assertEqual(event["net_added_width_px"],-95)
        self.assertEqual(event["horizontal_shifts"],[dict(dx_px=95,count=1)])
        self.assertTrue(event["width_displacement_balanced"])

    def test_identity_replacement_not_width_gain(self):
        event = summarize([with_two_slots(0),with_two_slots(1)])["transitions"][0]
        self.assertEqual(len(event["added"]),2)
        self.assertEqual(len(event["removed"]),2)
        self.assertFalse(event["width_displacement_balanced"])

    def test_gap_not_collapsed_to_sum(self):
        row = with_two_slots()
        row["taskbars"][0]["uia"][1]["rect"] = [1136,900,1176,950]
        event = summarize([sample(),row])["transitions"][0]
        self.assertFalse(event["contiguous_change"])
        self.assertFalse(event["width_displacement_balanced"])

    def test_bad_width_balance(self):
        row = with_two_slots()
        row["taskbars"][0]["uia"][0]["rect"] = [1106,900,1138,950]
        self.assertFalse(summarize([sample(),row])["transitions"][0]["width_displacement_balanced"])

    def test_environment_change_not_transition(self):
        row = with_two_slots()
        row["metadata"]["session_id"] = 2
        r = summarize([sample(),row])
        self.assertEqual(r["contexts"],2)
        self.assertEqual(r["transitions"],[])

    def test_bad_sample_breaks_chain(self):
        self.assertEqual(summarize([sample(),{},with_two_slots(2)])["transitions"],[])

    def test_long_gap_not_interpreted(self):
        r = summarize([sample(),with_two_slots(10)])
        self.assertEqual(r["transitions"],[])
        self.assertTrue(r["issues"])

    def test_multiple_taskbars_do_not_cancel_widths(self):
        a,b = sample(),with_two_slots()
        for row,source in [(a,with_two_slots()),(b,sample())]:
            bar = copy.deepcopy(source["taskbars"][0])
            bar["hwnd"] = bar["native"][0]["hwnd"] = 12
            row["taskbars"].append(bar)
        events = summarize([a,b])["transitions"]
        self.assertEqual([e["net_added_width_px"] for e in events],[95,-95])
        self.assertTrue(all(e["width_displacement_balanced"] for e in events))


if __name__ == "__main__":
    unittest.main()
