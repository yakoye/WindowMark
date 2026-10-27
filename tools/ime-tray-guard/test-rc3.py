"""RC3 自建宿主：提示语空间/显隐/关闭、后台自动恢复；不操作系统任务栏。"""
import argparse
import json
import subprocess
import time
from pathlib import Path

def run(args):
    build=Path(args.build).resolve(); out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=False)
    results=[]
    for mode in ('hints','transparent','live_hints','recovery'):
        folder=out/mode;folder.mkdir()
        with (folder/'fixture.jsonl').open('wb') as log,(folder/'stderr.log').open('wb') as err:
            command=[str(build/'WindowMarkXamlContainerFixture.exe'),'--seconds','30' if mode=='recovery' else '22']
            if mode=='recovery':command+=['--always-visible']
            fixture=subprocess.Popen(command,stdout=log,stderr=err,creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                deadline=time.monotonic()+5
                while not (folder/'fixture.jsonl').stat().st_size:
                    if fixture.poll() is not None or time.monotonic()>deadline:raise RuntimeError('夹具未就绪')
                    time.sleep(.05)
                if mode=='recovery':
                    command=[str(build/'WindowMarkImeGuardRecoveryTest.exe'),str(fixture.pid),str(folder/'leases')]
                elif mode=='live_hints':
                    command=[str(build/'WindowMarkImeGuardClientTest.exe'),str(fixture.pid),str(folder/'control.jsonl'),'18','0','--toggle-hints']
                else:
                    command=[str(build/'WindowMarkXamlContainerTrial.exe'),'--fixture-pid',str(fixture.pid),'--reserve','--persistent','--seconds','18','--out',str(folder/'control.jsonl')]
                    if mode=='hints':command+=['--show-hints']
                control=subprocess.run(command,capture_output=True,timeout=50,creationflags=subprocess.CREATE_NO_WINDOW)
                (folder/'control.stdout.log').write_bytes(control.stdout);(folder/'control.stderr.log').write_bytes(control.stderr)
                if control.returncode:raise RuntimeError(f'控制器失败: {control.returncode}')
                if fixture.wait(timeout=35):raise RuntimeError('夹具异常退出')
                rows=[json.loads(line) for line in (folder/'fixture.jsonl').read_text().splitlines()]
                if mode!='recovery':
                    hidden=[r for r in rows if 7<=r['second']<=13]
                    assert hidden and all(r['width']==76 and r['min_width']==76 for r in hidden),'空位尺寸不正确'
                    assert all(not r['hint_hit_test'] for r in rows),'文字拦截点击'
                    if mode=='hints':
                        assert all(r['hint_visible'] and 0<r['hint_chars']<=4 and 0<r['hint_width']<=76 for r in hidden),'隐藏时未正确显示短句'
                        assert all(not r['hint_visible'] for r in rows if r['content_visible']),'覆盖原生内容'
                    elif mode=='live_hints':
                        assert all(not r['hint_present'] for r in rows if 8<=r['second']<=9),'关闭文字未立即清理'
                        assert all(r['hint_visible'] for r in rows if 12<=r['second']<=13),'隐藏期间重新开启文字未生效'
                        assert all(not r['hint_visible'] for r in rows if r['content_visible']),'覆盖原生内容'
                    else:assert all(not r['hint_present'] for r in rows),'透明模式仍插入文字'
                    assert all(not r['hint_present'] and r['local_unset'] and r['min_width']==0 for r in rows if r['second']>=20),'退出后未恢复'
                else:
                    assert b'layout_auto_recovery=PASS' in control.stdout
                    assert b'session_resume=PASS' in control.stdout
                    assert b'manual_off_cancels_recovery=PASS' in control.stdout
                result={'mode':mode,'passed':True}
            except Exception as e:result={'mode':mode,'passed':False,'error':str(e)}
            finally:
                if fixture.poll() is None:fixture.wait(timeout=40)
            results.append(result);print(json.dumps(result,ensure_ascii=False),flush=True)
    (out/'summary.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
    return 0 if all(r['passed'] for r in results) else 1
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',required=True);p.add_argument('--out',required=True)
    raise SystemExit(run(p.parse_args()))
