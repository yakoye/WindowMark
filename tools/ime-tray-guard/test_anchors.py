import copy
import importlib.util
from pathlib import Path
import unittest
from test_analyze import sample

spec = importlib.util.spec_from_file_location('anchor_check', Path(__file__).with_name('check-anchors.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def frame(second, ime=True):
    row = sample(second)
    template = row['taskbars'][0]['uia'][0]
    nodes = []
    for index, (role, left, width) in enumerate((('overflow', 1100, 32), ('Everything', 1132, 32),
            ('StartButton', 300, 44), ('app', 344, 44), ('ime1', 1400, 32), ('ime2', 1432, 44))):
        if not ime and role.startswith('ime'): continue
        node = copy.deepcopy(template)
        node.update(index=index, parent=-1, rect=[left,900,left+width,950],
            runtime_id=[42,11,4,100+index], anchor_role=role if index<2 else '', app_geometry=role=='app')
        if role=='StartButton': node.update(automation_id=role, **{'class':'ToggleButton'})
        if role=='app': node.update(automation_id='', **{'class':'Taskbar.TaskListButtonAutomationPeer'})
        nodes.append(node)
    row['taskbars'][0]['uia'] = nodes
    return row


class AnchorTests(unittest.TestCase):
    def test_complete_stationary_scope(self):
        result = module.inspect([frame(0),frame(1,False)])
        self.assertTrue(result['geometry_passed'])
        self.assertEqual(len(result['ime_events'][0]['anchors']),4)

    def test_each_left_group_movement_is_failure(self):
        for index in range(4):
            a,b = frame(0),frame(1,False)
            b['taskbars'][0]['uia'][index]['rect'][0] += 76
            b['taskbars'][0]['uia'][index]['rect'][2] += 76
            self.assertFalse(module.inspect([a,b])['geometry_passed'])

    def test_missing_everything_does_not_pass(self):
        a,b = frame(0),frame(1,False)
        for row in (a,b): row['taskbars'][0]['uia'][1]['anchor_role']=''
        self.assertFalse(module.inspect([a,b])['geometry_passed'])

    def test_no_transition_does_not_pass(self):
        self.assertFalse(module.inspect([frame(0),frame(1)])['geometry_passed'])

    def test_identity_replacement_not_disappearance(self):
        a,b = frame(0),frame(1)
        for node in b['taskbars'][0]['uia'][4:]: node['runtime_id'][-1]+=100
        self.assertFalse(module.inspect([a,b])['geometry_passed'])

    def test_bad_sample_is_not_success(self):
        self.assertFalse(module.inspect([frame(0),{}])['geometry_passed'])
