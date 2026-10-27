"""仅启动自身屏幕外夹具；重复订阅/退出，保留首次失败，不操作Explorer。"""
import argparse
import json
import subprocess
import time
from pathlib import Path


def run(args):
    destination = Path(args.evidence).resolve()
    destination.mkdir(parents=True, exist_ok=False)
    flags = subprocess.CREATE_NO_WINDOW
    processes = []
    summary = {"ui_threads": args.ui_threads, "retire_secondary": args.retire_secondary, "close_secondary": args.close_secondary, "cycles": [], "native_attachment": False}
    with (destination / "fixture.jsonl").open("wb") as out, (destination / "fixture.stderr.log").open("wb") as err:
        fixture_args = [args.fixture, "--ui-threads", str(args.ui_threads), "--seconds", str(30 if args.steady_toggle else args.cycles * 7 + 8), "--churn"]
        if args.retire_secondary:
            fixture_args.append("--retire-secondary")
        if args.close_secondary:
            fixture_args.append("--close-secondary")
        fixture = subprocess.Popen(fixture_args, stdout=out, stderr=err, creationflags=flags)
        processes.append(fixture)
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if fixture.poll() is not None:
                    raise RuntimeError(f"fixture startup failed: {fixture.returncode}")
                lines = (destination / "fixture.jsonl").read_text().splitlines()
                ready = {json.loads(line)["island"] for line in lines if line.endswith("}")}
                if len(ready) >= args.ui_threads:
                    break
                time.sleep(.05)
            else:
                raise RuntimeError("fixture did not become ready")
            for i in range(args.cycles):
                if i and args.close_secondary:
                    time.sleep(3)
                executable = args.second_trial if i and args.second_trial else args.trial
                evidence = destination / f"trial-{i}.jsonl"
                with (destination / f"trial-{i}.stdout.log").open("wb") as cout, (destination / f"trial-{i}.stderr.log").open("wb") as cerr:
                    command = [executable, "--fixture-pid", str(fixture.pid), "--seconds", "23" if args.steady_toggle else "5", "--out", str(evidence)]
                    if args.steady_toggle:
                        command += ["--reserve", "--toggle-ms", "4000"]
                    if args.persistent:
                        command += ["--persistent"]
                    controller = subprocess.Popen(command, stdout=cout, stderr=cerr, creationflags=flags)
                    processes.append(controller)
                    code = controller.wait(timeout=40 if args.steady_toggle else 22)
                records = [json.loads(line) for line in evidence.read_text().splitlines()] if evidence.exists() else []
                states = [r for r in records if "state" in r]
                final = states[-1] if states else {}
                summary["cycles"].append({"index": i, "exit": code, "state": final.get("state"), "writes": final.get("writes"), "subscribe_hr": final.get("subscribe_hr"), "unsubscribe_hr": final.get("unsubscribe_hr"), "node_max": max((len(r["nodes"]) for r in states), default=0)})
                if code or fixture.poll() is not None or final.get("state") != 5:
                    raise RuntimeError(f"cycle {i} failed; controller={code}, fixture={fixture.poll()}")
                if args.steady_toggle:
                    if final.get("writes", 0) < 3 or final.get("restores") != final.get("writes") or final.get("restore_failures"):
                        raise RuntimeError("单订阅反复开关未完整恢复")
                    samples = [json.loads(line) for line in (destination / "fixture.jsonl").read_text().splitlines() if line.endswith("}")]
                    hidden = [r for r in samples if r.get("island") == 0 and r.get("content_visible") is False]
                    if not any(r["width"] == 76 and r["min_width"] == 76 for r in hidden):
                        raise RuntimeError("未观测到隐藏时真实占位")
                    if not any(r["width"] == 0 and r["min_width"] == 0 and r["local_unset"] for r in hidden):
                        raise RuntimeError("未观测到关闭后的原值恢复")
            summary["fixture_exit"] = fixture.wait(timeout=25)
            if summary["fixture_exit"]:
                raise RuntimeError("fixture crashed on shutdown")
            summary["passed"] = True
        except Exception as error:
            summary["passed"] = False
            summary["error"] = str(error)
        finally:
            for p in processes[1:]:
                if p.poll() is None:
                    p.kill()
                    p.wait(timeout=5)
            if fixture.poll() is None:
                try:
                    fixture.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    fixture.kill()
                    fixture.wait(timeout=5)
                    summary["fixture_forced_exit"] = True
                    summary["passed"] = False
            summary["fixture_exit"] = fixture.returncode
            (destination / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(json.dumps(summary))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--trial", required=True)
    parser.add_argument("--second-trial")
    parser.add_argument("--retire-secondary", action="store_true")
    parser.add_argument("--close-secondary", action="store_true")
    parser.add_argument("--steady-toggle", action="store_true")
    parser.add_argument("--persistent", action="store_true")
    parser.add_argument("--ui-threads", type=int, choices=range(1, 5), default=1)
    parser.add_argument("--cycles", type=int, choices=range(1, 7), default=2)
    parser.add_argument("--evidence", required=True)
    args = parser.parse_args()
    if args.steady_toggle and args.cycles != 1:
        parser.error("--steady-toggle要求--cycles 1")
    raise SystemExit(run(args))
