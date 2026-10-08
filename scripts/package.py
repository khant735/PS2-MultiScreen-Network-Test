#!/usr/bin/env python3
import argparse, datetime, hashlib, json, os, pathlib, subprocess, zipfile, zlib
from zoneinfo import ZoneInfo

ROOT = pathlib.Path(__file__).resolve().parent.parent
def crc(path):
    return f"{zlib.crc32(path.read_bytes()) & 0xffffffff:08X}"
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="0.1.0")
    ap.add_argument("--elf", default="build/PS2-MultiScreen.ELF")
    args = ap.parse_args()
    elf = ROOT / args.elf
    if not elf.is_file():
        ap.error("Compile ELF before packaging")
    now = datetime.datetime.now(ZoneInfo("Europe/London"))
    stamp = now.strftime("(%d-%m-%Y)-[%I-%M-%S%p]")
    prefix = f"PS2-MultiScreen-v{args.version}-{stamp}"
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    outelf = dist / f"{prefix}-{crc(elf)}.ELF"
    outelf.write_bytes(elf.read_bytes())
    tmp = dist / "source-tmp.zip"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(ROOT.rglob("*")):
            if p.is_file() and not set(p.relative_to(ROOT).parts).intersection({".git", "dist", "build", "__pycache__"}):
                z.write(p, p.relative_to(ROOT).as_posix())
    outzip = dist / f"{prefix}-{crc(tmp)}-SOURCE.zip"
    tmp.rename(outzip)
    try:
        commit = subprocess.check_output(["git","rev-parse","HEAD"], cwd=ROOT, text=True).strip()
    except Exception:
        commit = "unknown"
    files = [{"name":p.name, "crc32":crc(p), "sha256":sha(p), "size":p.stat().st_size} for p in (outelf,outzip)]
    (dist/"manifest.json").write_text(json.dumps({"version":args.version,"build_time_uk":now.isoformat(),"commit":commit,"files":files},indent=2)+"\n")
    (dist/"SHA256SUMS.txt").write_text("".join(f'{sha(p)}  {p.name}\n' for p in (outelf,outzip)))
    for p in (outelf,outzip):
        print(p.name)
if __name__ == "__main__":
    main()
