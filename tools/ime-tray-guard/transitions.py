"""分析相邻样本的托盘增减与位移；不把几何相关性认定为 IME 身份或原生修改可行性。"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys

from analyze import read_rows, unpack


def visible(nodes):
    return {key: n for key, n in nodes.items() if n["offscreen"] == 0
            and n["rect"][2] > n["rect"][0] and n["rect"][3] > n["rect"][1]}


def describe(key, node):
    r = node["rect"]
    return dict(taskbar=key[0], runtime_id=list(key[1]), rect=r,
                width_px=r[2]-r[0], height_px=r[3]-r[1],
                class_name=node["class"], automation_id=node["automation_id"])


def diff_buttons(old, new, taskbar):
    old = visible({k:v for k,v in old.items() if k[0] == taskbar})
    new = visible({k:v for k,v in new.items() if k[0] == taskbar})
    added = [describe(k,new[k]) for k in sorted(new.keys()-old.keys())]
    removed = [describe(k,old[k]) for k in sorted(old.keys()-new.keys())]
    shifts = Counter()
    changed = []
    unchanged = 0
    for k in sorted(old.keys() & new.keys()):
        a,b = old[k],new[k]
        if (a["class"],a["automation_id"]) != (b["class"],b["automation_id"]):
            changed.append(dict(runtime_id=list(k[1]), reason="类型变化，身份不可靠"))
            continue
        delta = tuple(y-x for x,y in zip(a["rect"],b["rect"]))
        if delta == (0,0,0,0):
            unchanged += 1
        elif delta[0] == delta[2] and delta[1] == delta[3] == 0:
            shifts[delta[0]] += 1
        else:
            changed.append(dict(runtime_id=list(k[1]), reason="尺寸或垂直位置变化",delta=list(delta)))
    net_width = sum(n["width_px"] for n in added)-sum(n["width_px"] for n in removed)
    # 只有单向、连续且同高的一组增/减按钮才允许比较“总宽度抵消”。
    group = sorted(added or removed, key=lambda n:n["rect"][0])
    contiguous = bool(group) and all(a["rect"][2] == b["rect"][0]
        and a["rect"][1::2] == b["rect"][1::2] for a,b in zip(group,group[1:]))
    balanced = (bool(added) != bool(removed)) and contiguous and not changed and bool(shifts)
    balanced = balanced and set(shifts) == {-net_width}
    return dict(taskbar=taskbar, added=added, removed=removed,
                horizontal_shifts=[dict(dx_px=dx,count=shifts[dx]) for dx in sorted(shifts)],
                unchanged_buttons=unchanged, other_changes=changed, net_added_width_px=net_width,
                contiguous_change=contiguous, width_displacement_balanced=bool(balanced))


def summarize(rows):
    result = dict(samples=len(rows), valid_samples=0, transitions=[], contexts=0,
                  process_states=[], issues=[], semantic_identity_confirmed=False,
                  native_layout_feasibility="未验证")
    states = Counter()
    previous = context = stamp = previous_exe = None
    for i,row in enumerate(rows):
        try:
            current_context,nodes,now = unpack(row)
            exe = row["metadata"]["foreground_exe"]
        except (KeyError,ValueError,TypeError,IndexError) as exc:
            result["issues"].append(dict(sample=i,reason=str(exc)))
            previous = context = stamp = previous_exe = None
            continue
        result["valid_samples"] += 1
        states[(exe,len(visible(nodes)))] += 1
        if current_context != context:
            result["contexts"] += 1
        elif previous is not None:
            gap = (now-stamp).total_seconds()
            if gap <= 0 or gap > 5:
                result["issues"].append(dict(sample=i,reason="非连续采样，跳过转换推断"))
            else:
                for taskbar in sorted({k[0] for k in nodes} | {k[0] for k in previous}):
                    delta = diff_buttons(previous,nodes,taskbar)
                    if delta["added"] or delta["removed"] or delta["horizontal_shifts"] or delta["other_changes"]:
                        result["transitions"].append(dict(sample=i,utc=row["utc"],
                            from_exe=previous_exe,to_exe=exe,**delta))
        previous,context,stamp,previous_exe = nodes,current_context,now,exe
    result["process_states"] = [dict(exe=exe,visible_buttons=count,samples=n)
                                for (exe,count),n in sorted(states.items())]
    return result


def markdown(report):
    lines = ["# 托盘显隐转换分析", "", "只读数据的几何分析，不证明原生固定占位已可行。", "",
        f"采样 {report['samples']} 次，有效 {report['valid_samples']} 次，环境段 {report['contexts']} 个。", "",
        "## 前台程序与可见按钮数", "", "| 前台 EXE | 可见托盘按钮数 | 样本数 |",
        "| --- | --- | --- |"]
    for item in report["process_states"]:
        # EXE 是外部数据；转义 Markdown 控制字符，不把它当作指令。
        exe = item["exe"].replace("|", "\\|").replace("\n", " ").replace("\r", " ").replace("`", "")
        lines.append(f"| {exe} | {item['visible_buttons']} | {item['samples']} |")
    lines += ["", "## 转换事件", "", "| 样本 | 增加宽度(px) | 消失宽度(px) | 既有按钮水平位移 | 宽度与位移吻合 |",
              "| --- | --- | --- | --- | --- |"]
    for event in report["transitions"]:
        added = "+".join(str(n["width_px"]) for n in event["added"]) or "—"
        removed = "+".join(str(n["width_px"]) for n in event["removed"]) or "—"
        shifts = "、".join(f"{n['dx_px']:+d}px × {n['count']}" for n in event["horizontal_shifts"]) or "—"
        balanced = "是" if event["width_displacement_balanced"] else "否/证据不足"
        lines.append(f"| {event['sample']} | {added} | {removed} | {shifts} | {balanced} |")
    lines += ["", "## 限制", "",
        "- 表中的消失是从可见 UIA 按钮集合退出，不证明 XAML 节点已经销毁。",
        "- 返回时 RuntimeId 改变只证明 UIA 身份变了；不能猜测其稳定父布局容器。",
        "- 几何吻合不是输入法身份验证，必须结合用户截图/观察或原生视觉树。",
        "- 前台程序只用于描述采样，不能当作触发占位的规则。",
        "- 此报告不推断保护是否开启；须结合对应运行日志和控制状态判断。",
        f"- 数据问题数：{len(report['issues'])}。", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file")
    parser.add_argument("--out",help="创建新的 Markdown 报告；拒绝覆盖")
    args = parser.parse_args()
    try:
        report = summarize(read_rows(args.file))
        if args.out:
            with Path(args.out).open("x",encoding="utf-8",newline="\n") as stream:
                stream.write(markdown(report))
        print(json.dumps(report,ensure_ascii=False,indent=2))
        return 2 if report["issues"] or not report["valid_samples"] else 0
    except (ValueError,OSError) as exc:
        print(str(exc),file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
