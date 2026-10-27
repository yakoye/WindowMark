"""只读 JSONL 证据分析。绝不输出“保护通过”，不跨文件/环境拼接证据。"""
import argparse
import json
import sys
from collections import Counter
from datetime import datetime
from pathlib import Path


def read_rows(path):
    rows = []
    with Path(path).open(encoding="utf-8-sig") as stream:
        for number, line in enumerate(stream, 1):
            if not line.strip():
                raise ValueError(f"第 {number} 行为空，证据不完整")
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"第 {number} 行 JSON 无效/被截断") from exc
    if not rows:
        raise ValueError("没有采样数据")
    return rows


def rect_valid(rect):
    return (isinstance(rect, list) and len(rect) == 4
            and all(type(x) is int for x in rect)
            and rect[2] >= rect[0] and rect[3] >= rect[1])


def unpack(row):
    if row["schema"] != 1 or row["readonly"] is not True or row["protection_active"] is not False:
        raise ValueError("不是已知只读探针格式")
    if row.get("native_only", False) or row["uia_init_hr"] != 0:
        raise ValueError("UIA 不可用或仅采集了 HWND")
    meta = row["metadata"]
    context = {key: meta[key] for key in ("session_id", "console_session_id", "wts_protocol",
               "build", "ubr", "explorer_file_version", "monitors")}
    if not meta["monitors"] or any(m["dpi_hr"] != 0 for m in meta["monitors"]):
        raise ValueError("显示器/DPI 证据不完整")
    bars = row["taskbars"]
    if not bars:
        raise ValueError("未找到任务栏")
    nodes = {}
    roots = []
    for bar in bars:
        if (bar["uia_hr"] != 0 or bar["uia_errors"] != 0 or bar["uia_truncated"]
                or bar["native_truncated"] or not bar["uia"]):
            raise ValueError("任务栏树错误、截断或为空")
        native = bar["native"][0]
        if native["hwnd"] != bar["hwnd"] or not native["rect_ok"]:
            raise ValueError("原生任务栏根无效")
        roots.append({key: native[key] for key in ("hwnd", "pid", "class", "rect", "dpi", "visible")})
        by_index = {n["index"]: n for n in bar["uia"]}
        if len(by_index) != len(bar["uia"]):
            raise ValueError("节点索引重复")
        in_tray = set()
        for n in bar["uia"]:
            if (n["class"].startswith("SystemTray.") or n["class"] == "TrayNotifyWnd"
                    or n["parent"] in in_tray):
                in_tray.add(n["index"])
            if n["index"] not in in_tray or n["control_type"] != 50000:
                continue
            if n["rect_hr"] != 0 or not rect_valid(n["rect"]) or n["offscreen"] not in (0, 1):
                raise ValueError("按钮坐标或可见性读取失败")
            if not n["runtime_id"]:
                raise ValueError("按钮缺少 RuntimeId，不能按顺序猜身份")
            key = (bar["hwnd"], tuple(n["runtime_id"]))
            if key in nodes:
                raise ValueError("按钮 RuntimeId 重复")
            nodes[key] = n
    context["taskbars"] = sorted(roots, key=lambda n: n["hwnd"])
    if not nodes:
        raise ValueError("没有可识别身份的托盘按钮（含 Win10 未支持的结构）")
    stamp = datetime.fromisoformat(row["utc"].replace("Z", "+00:00"))
    if stamp.tzinfo is None:
        raise ValueError("时间戳缺少时区")
    return json.dumps(context, sort_keys=True), nodes, stamp


def analyze(rows):
    report = dict(status="仅诊断，不构成原生占位验收", protection_active=False,
                  samples=len(rows), valid_samples=0, comparable_pairs=0, context_resets=0,
                  max_rect_delta_px=0, moved_buttons=[], node_set_changes=[],
                  candidate_button_counts=[], foreground_exes=[], issues=[])
    baseline = previous = None
    context = stamp = None
    exes = set()
    for index, row in enumerate(rows):
        try:
            new_context, nodes, new_stamp = unpack(row)
            exes.add(row["metadata"]["foreground_exe"])
        except (KeyError, IndexError, TypeError, ValueError) as exc:
            report["issues"].append(f"样本 {index}: {exc}")
            baseline = previous = None
            context = stamp = None
            continue
        report["valid_samples"] += 1
        report["candidate_button_counts"].append(sum(n["ime_candidate"] for n in nodes.values()))
        if context != new_context:
            if context is not None:
                report["context_resets"] += 1
                report["issues"].append(f"样本 {index}: 环境/任务栏实例变化，重置比较基线")
            baseline = previous = nodes
            context, stamp = new_context, new_stamp
            continue
        gap = (new_stamp - stamp).total_seconds()
        if gap <= 0 or gap > 5:
            report["issues"].append(f"样本 {index}: 采样时间间隔 {gap:g} 秒，不能排除间隙内变化")
        stamp = new_stamp
        report["comparable_pairs"] += 1
        added, removed = set(nodes) - set(previous), set(previous) - set(nodes)
        if added or removed:
            report["node_set_changes"].append(dict(sample=index, added=len(added), removed=len(removed)))
        for key in set(nodes) & set(baseline):
            a, b = baseline[key], nodes[key]
            if (a["class"], a["automation_id"]) != (b["class"], b["automation_id"]):
                report["issues"].append(f"样本 {index}: RuntimeId 对应类型改变，不比较该按钮")
                continue
            if a["offscreen"] or b["offscreen"] or a["ime_candidate"] or b["ime_candidate"]:
                continue
            delta = max(abs(x-y) for x, y in zip(a["rect"], b["rect"]))
            report["max_rect_delta_px"] = max(delta, report["max_rect_delta_px"])
            if delta:
                report["moved_buttons"].append(dict(sample=index, runtime_id=list(key[1]),
                    taskbar=key[0], before=a["rect"], after=b["rect"], delta_px=delta))
        previous = nodes
    report["foreground_exes"] = sorted(exes)
    report["limitations"] = [
        "未被启发式标记的按钮仍可能是 IME；必须人工确认真实目标。",
        "UIA 缺失不证明 XAML 节点被销毁，UIA 树不等于视觉树。",
        "未观察到位移不等于保护生效；没有占位代码，也没有逐帧测量。",
        "每份文件独立分析，不跨设备、会话或 Explorer 实例比较。"]
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+")
    args = parser.parse_args()
    failed = False
    for path in args.files:
        try:
            report = analyze(read_rows(path))
            print(json.dumps({"file":str(path), **report}, ensure_ascii=False, indent=2))
            failed |= bool(report["issues"]) or report["comparable_pairs"] == 0
        except (OSError, ValueError) as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            failed = True
    return 2 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
