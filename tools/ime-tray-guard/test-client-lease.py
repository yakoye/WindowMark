"""串行测试自身夹具上的长租约、界面心跳失联与界面进程死亡；不附加Explorer。"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main(args):
    base=Path(args.build).resolve()
    output=Path(args.evidence).resolve()
    output.mkdir(parents=True,exist_ok=False)
    results=[]
    for mode in ("normal", "heartbeat", "owner_death"):
        folder=output/mode
        folder.mkdir()
        fixture_seconds=50 if mode=="normal" else 25
        with (folder/"fixture.jsonl").open("wb") as f, (folder/"client.jsonl").open("wb") as c, (folder/"stderr.log").open("wb") as e:
            fixture=subprocess.Popen([str(base/"WindowMarkXamlContainerFixture.exe"),"--seconds",str(fixture_seconds)],stdout=f,stderr=e,creationflags=subprocess.CREATE_NO_WINDOW)
            client=None
            try:
                deadline=time.monotonic()+5
                while not (folder/"fixture.jsonl").stat().st_size:
                    if fixture.poll() is not None or time.monotonic()>deadline:raise RuntimeError("夹具未就绪")
                    time.sleep(.05)
                client=subprocess.Popen([str(base/"WindowMarkImeGuardClientTest.exe"),str(fixture.pid),str(folder/"control.jsonl"),"40" if mode=="normal" else "20","7" if mode=="heartbeat" else "0"],stdout=c,stderr=e,creationflags=subprocess.CREATE_NO_WINDOW)
                if mode=="owner_death":
                    deadline=time.monotonic()+10
                    while time.monotonic()<deadline:
                        rows=[json.loads(s) for s in (folder/"fixture.jsonl").read_text().splitlines() if s.endswith("}")]
                        if any(r["second"]>=7 and not r["content_visible"] and r["min_width"]==76 for r in rows):break
                        time.sleep(.1)
                    else:raise RuntimeError("终止前未确认真实占位")
                    client.kill() # 只结束本测试刚创建的界面夹具，不是Explorer。
                code=client.wait(timeout=55)
                if mode!="owner_death" and code:raise RuntimeError(f"界面测试失败 {code}")
                if fixture.wait(timeout=30):raise RuntimeError("夹具异常退出")
                rows=[json.loads(s) for s in (folder/"fixture.jsonl").read_text().splitlines()]
                if not any(not r["content_visible"] and r["min_width"]==76 and r["width"]==76 for r in rows):raise RuntimeError("缺少占位证据")
                restore_at=42 if mode=="normal" else 11
                if not any(r["second"]>=restore_at and r["local_unset"] and r["min_width"]==0 for r in rows):raise RuntimeError("未观察到原值恢复")
                if not any(r["second"]>=restore_at and not r["hint_present"] for r in rows):raise RuntimeError("退出后提示语未移除")
                records=[json.loads(s) for s in (folder/"control.jsonl").read_text().splitlines()]
                final=records[-1]
                if final.get("state")!=5 or final.get("writes")!=1 or final.get("restores")!=1 or final.get("restore_failures"):raise RuntimeError("控制程序未确认完整恢复")
                if mode=="normal" and not any(r["second"]>=35 and r["min_width"]==76 for r in rows):raise RuntimeError("没有越过初始30秒期限")
                result={"mode":mode,"passed":True,"controller_final":{k:final[k] for k in ("state","writes","restores","restore_failures")}}
            except Exception as error:
                result={"mode":mode,"passed":False,"error":str(error)}
            finally:
                if client and client.poll() is None:client.kill();client.wait()
                if fixture.poll() is None:fixture.wait(timeout=55)
            results.append(result)
            print(json.dumps(result,ensure_ascii=False),flush=True)
    (output/"summary.json").write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding="utf-8")
    return 0 if all(r["passed"] for r in results) else 1


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build",required=True)
    parser.add_argument("--evidence",required=True)
    raise SystemExit(main(parser.parse_args()))
