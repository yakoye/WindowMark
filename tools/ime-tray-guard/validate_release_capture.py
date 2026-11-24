"""核对已人工确认的原生输入法槽位往返显隐及既有图标坐标；仅处理只读采样文件。"""
import argparse
import json
from pathlib import Path
from analyze import read_rows, unpack
from transitions import visible


def app_nodes(row):
    return {tuple(n['runtime_id']): n for b in row['taskbars'] for n in b['uia'] if n.get('app_geometry')}


def app_position_unchanged(before, after):
    if before['class'] != after['class']:
        return False
    a, b = before['rect'], after['rect']
    if a == b:
        return True
    # 系统 Image 的焦点动画会以中心为锚点等比缩放；父按钮仍须逐边坐标完全一致。
    return (before['class'] == 'Image' and a[2] > a[0] and a[3] > a[1]
            and b[2] > b[0] and b[3] > b[1]
            and a[0] + a[2] == b[0] + b[2] and a[1] + a[3] == b[1] + b[3]
            and (a[2] - a[0]) * (b[3] - b[1]) == (b[2] - b[0]) * (a[3] - a[1]))


def validate(rows, left, width):
    previous = None
    context = None
    events = []
    failures = []
    centered_image_scales = 0
    remote_hidden = False
    for i, row in enumerate(rows):
        current_context, nodes, stamp = unpack(row)
        main = next(b for b in row['taskbars'] if b['native'][0]['class'] == 'Shell_TrayWnd')
        buttons = visible({k: n for k, n in nodes.items() if k[0] == main['hwnd']})
        slot = {k: n for k, n in buttons.items() if left <= n['rect'][0] < left + width}
        state = len(slot)
        if state not in (0, 2):
            failures.append(f'{i}: 原生槽位按钮数量异常 {state}')
        if state == 2:
            rects = sorted(n['rect'] for n in slot.values())
            if rects[0][0] != left or rects[0][2] != rects[1][0] or rects[1][2] != left + width:
                failures.append(f'{i}: 原生按钮没有填满已确认槽位')
        roles = {n.get('anchor_role'): n['rect'] for n in buttons.values() if n.get('anchor_role')}
        if not all(role in roles for role in ('overflow', 'Everything', 'network', 'volume')):
            failures.append(f'{i}: 缺少要求的托盘锚点')
        exe = row['metadata']['foreground_exe'].lower()
        if state == 0 and exe in ('mstsc.exe', 'vncviewer.exe', 'mobaxterm.exe', 'rustdesk.exe', 'todesk.exe'):
            remote_hidden = True
        if context is not None and context != current_context:
            failures.append(f'{i}: 采样中任务栏或显示布局改变')
        if previous and state != previous['state']:
            gap = (stamp - previous['stamp']).total_seconds()
            if gap <= 0 or gap > 5:
                failures.append(f'{i}: 显隐之间采样不连续')
            old = previous['outside']
            outside = {k: n for k, n in buttons.items() if k not in slot}
            changed = [str(k) for k in old.keys() & outside.keys() if old[k]['rect'] != outside[k]['rect']]
            if old.keys() != outside.keys() or changed or roles != previous['roles']:
                failures.append(f'{i}: 原生显隐时其他托盘按钮改变')
            old_apps = previous['apps']
            apps = app_nodes(row)
            common_apps = old_apps.keys() & apps.keys()
            if any(not app_position_unchanged(old_apps[k], apps[k]) for k in common_apps):
                failures.append(f'{i}: 原生显隐时任务栏应用区移动')
            centered_image_scales += sum(old_apps[k]['rect'] != apps[k]['rect']
                                        and app_position_unchanged(old_apps[k], apps[k]) for k in common_apps)
            events.append(dict(sample=i, utc=row['utc'], before=previous['state'], after=state,
                               from_exe=previous['exe'], to_exe=exe, common_tray_buttons=len(old.keys() & outside.keys())))
        previous = dict(state=state, stamp=stamp, outside={k: n for k, n in buttons.items() if k not in slot}, roles=roles,
                        exe=exe, apps=app_nodes(row))
        context = current_context
    appeared = any(e['before'] == 0 and e['after'] == 2 for e in events)
    disappeared = any(e['before'] == 2 and e['after'] == 0 for e in events)
    return dict(passed=appeared and disappeared and remote_hidden and not failures,
                samples=len(rows), slot_left_px=left, slot_width_px=width,
                appeared=appeared, disappeared=disappeared, remote_hidden=remote_hidden,
                centered_image_scale_events=centered_image_scales,
                events=events, failures=failures,
                scope='现有远程应用前后台往返；不代表断线重连、Windows 10 或所有显示布局已验证')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('file')
    p.add_argument('--slot-left', type=int, required=True)
    p.add_argument('--slot-width', type=int, required=True)
    p.add_argument('--out', required=True)
    a = p.parse_args()
    result = validate(read_rows(a.file), a.slot_left, a.slot_width)
    with Path(a.out).open('x', encoding='utf-8') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(0 if result['passed'] else 2)
