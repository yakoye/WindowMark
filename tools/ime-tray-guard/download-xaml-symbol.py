"""从微软公共符号服务器分段下载已核对GUID的PDB；不上传转储。"""
import concurrent.futures
import hashlib
from pathlib import Path
import urllib.request
import sys

URL = "https://msdl.microsoft.com/download/symbols/windows.ui.xaml.pdb/E15A88ED9599B347A0D2576C13287FC41/windows.ui.xaml.pdb"
SIZE = 354881536


def main():
    root = Path(sys.argv[1]).resolve()
    root.mkdir(parents=True, exist_ok="--resume" in sys.argv)
    if (root / "windows.ui.xaml.pdb").exists():
        raise RuntimeError("完整输出已存在，不覆盖")
    count = 16
    def part(index):
        start, end = SIZE * index // count, SIZE * (index + 1) // count - 1
        path = root / f"part-{index:02d}.bin"
        have = path.stat().st_size if path.exists() else 0
        if have > end - start + 1:
            raise RuntimeError("现有分段超过预期长度")
        if have == end - start + 1:
            return path
        request = urllib.request.Request(URL, headers={"Range": f"bytes={start + have}-{end}"})
        with urllib.request.urlopen(request, timeout=45) as response, path.open("ab" if have else "wb") as out:
            if response.status != 206 or response.headers.get("Content-Range") != f"bytes {start + have}-{end}/{SIZE}":
                raise RuntimeError("服务器未返回所需的精确分段")
            while block := response.read(65536):
                out.write(block)
        if path.stat().st_size != end - start + 1:
            raise RuntimeError("分段长度不符")
        print(f"完成分段 {index + 1}/{count}", flush=True)
        return path
    with concurrent.futures.ThreadPoolExecutor(max_workers=count) as pool:
        paths = list(pool.map(part, range(count)))
    digest = hashlib.sha256()
    with (root / "windows.ui.xaml.pdb").open("xb") as out:
        for path in paths:
            with path.open("rb") as source:
                while block := source.read(1024 * 1024):
                    out.write(block)
                    digest.update(block)
    print("PDB下载完成，SHA-256=" + digest.hexdigest(), flush=True)


if __name__ == "__main__":
    main()
