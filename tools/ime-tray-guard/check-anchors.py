"""按原生 IME 显隐事件核对所有稳定身份的托盘及应用按钮；不按图标序号猜身份。"""
import argparse
import json
from analyze import read_rows, unpack
from transitions import summarize, visible


def anchors(row):
    _, tray, _ = unpack(row)
    result = visible(tray)
    for bar in row['taskbars']:
        for node in bar['uia']:
            selected = (node['automation_id'] in ('StartButton', 'SearchButton', 'TaskViewButton')
                        or (node.get('app_geometry') and 'TaskListButton' in node['class']))
            if selected and node['offscreen'] == 0 and node['rect'][2] > node['rect'][0]:
                result[(bar['hwnd'], tuple(node['runtime_id']))] = node
    return result


def inspect(rows):
    transitions = summarize(rows)
    result = dict(samples=len(rows), issues=transitions['issues'], ime_events=[])
    for event in transitions['transitions']:
        changed = event['added'] or event['removed']
        if (bool(event['added']) == bool(event['removed']) or
                len(changed) != 2 or not event['contiguous_change']):
            continue
        index = event['sample']
        old, new = anchors(rows[index-1]), anchors(rows[index])
        left_edge = min(n['rect'][0] for n in changed)
        checked = []
        for key in sorted(old.keys() & new.keys()):
            a, b = old[key], new[key]
            if a['class'] != b['class'] or a['automation_id'] != b['automation_id']:
                continue
            checked.append(dict(role=a.get('anchor_role') or a['automation_id'] or a['class'],
                before=a['rect'], after=b['rect'], moved=a['rect'] != b['rect'],
                left_of_ime=a['rect'][2] <= left_edge, app=bool(a.get('app_geometry'))))
        roles = {n['role'] for n in checked}
        result['ime_events'].append(dict(sample=index, utc=event['utc'],
            changed_width_px=event['net_added_width_px'], anchors=checked,
            all_unchanged=all(not n['moved'] for n in checked),
            coverage_complete=('overflow' in roles and 'Everything' in roles and
                               'StartButton' in roles and any(n['app'] for n in checked))))
    result['geometry_passed'] = (not result['issues'] and bool(result['ime_events']) and
        all(e['all_unchanged'] and e['coverage_complete'] for e in result['ime_events']))
    result['limitation'] = '显隐身份须结合用户确认；几何通过不是所有系统的稳定性保证。'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input')
    args = parser.parse_args()
    result = inspect(read_rows(args.input))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(0 if result['geometry_passed'] else 2)
