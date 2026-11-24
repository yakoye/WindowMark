"""发布采样判定器的反误判测试；合成数据不代替实机验收。"""
import copy
import unittest
from test_analyze import sample
from validate_release_capture import validate


def row(second, shown):
    result = sample(second)
    result['metadata']['foreground_exe'] = 'notepad.exe' if shown else 'mstsc.exe'
    base = result['taskbars'][0]['uia'][0]
    nodes = []
    for i, role in enumerate(('overflow', 'Everything', 'network', 'volume')):
        n = copy.deepcopy(base)
        n.update(index=i, anchor_role=role, runtime_id=[42, 11, 4, i], rect=[100+i*40, 900, 132+i*40, 950])
        nodes.append(n)
    if shown:
        for i, rect in enumerate(([400,900,440,950], [440,900,495,950])):
            n = copy.deepcopy(base)
            n.update(index=4+i, runtime_id=[42,11,4,40+i], rect=rect)
            nodes.append(n)
    result['taskbars'][0]['uia'] = nodes
    return result


class ReleaseCaptureTests(unittest.TestCase):
    def test_complete_round_trip(self):
        self.assertTrue(validate([row(0,True), row(1,False), row(2,True)],400,95)['passed'])

    def test_static_and_one_way_are_incomplete(self):
        for states in ((True,True,True),(False,True,True),(True,False,False)):
            self.assertFalse(validate([row(i,s) for i,s in enumerate(states)],400,95)['passed'])

    def test_one_pixel_shift_fails(self):
        rows = [row(0,True),row(1,False),row(2,True)]
        rows[1]['taskbars'][0]['uia'][0]['rect'][0] += 1
        self.assertFalse(validate(rows,400,95)['passed'])

    def test_remote_context_required(self):
        rows = [row(0,True),row(1,False),row(2,True)]
        rows[1]['metadata']['foreground_exe'] = 'notepad.exe'
        self.assertFalse(validate(rows,400,95)['passed'])

    def test_long_gap_and_layout_change_fail(self):
        rows = [row(0,True),row(10,False),row(11,True)]
        self.assertFalse(validate(rows,400,95)['passed'])
        rows = [row(0,True),row(1,False),row(2,True)]
        rows[1]['metadata']['ubr'] += 1
        self.assertFalse(validate(rows,400,95)['passed'])


if __name__ == '__main__':
    unittest.main()
