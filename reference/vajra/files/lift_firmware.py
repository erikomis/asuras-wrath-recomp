import argparse
import json
import os
import shutil
import subprocess
import sys

port = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(port)
tools = os.path.join(root, "ps3recomp", "tools")

ap = argparse.ArgumentParser()
ap.add_argument("--firmware", default=os.path.join(root, "data", "firmware"))
ap.add_argument("--dev-flash", default=os.environ.get("PS3_DEV_FLASH") or os.path.join(root, "data", "dev_flash"))
args = ap.parse_args()


def run(*cmd):
    subprocess.run([sys.executable, *cmd], check=True)


def lift_prx(name, base):
    prx = os.path.join(args.firmware, f"{name}.prx")
    if not os.path.isfile(prx):
        sys.exit(f"missing {prx} (decrypt {name}.sprx from dev_flash/sys/external)")
    meta = os.path.join(port, "meta", name)
    out = os.path.join(port, "recompiled_prx", name)
    os.makedirs(meta, exist_ok=True)
    run(os.path.join(tools, "lift_prx.py"), prx, "--base", base, "--output", meta)
    shutil.rmtree(out, ignore_errors=True)
    run(os.path.join(port, "tools", "prxgen.py"), meta, out, name)
    with open(os.path.join(meta, f"{name}_exports.json")) as f:
        toc = json.load(f)["toc"]
    with open(os.path.join(out, f"{name}_stubs.json")) as f:
        last_stub = max(int(s["stub"], 16) for s in json.load(f))
    run(os.path.join(tools, "ppu_lifter.py"), os.path.join(meta, f"{name}_image.bin"),
        "--raw", "--base", base, "--toc", str(toc), "--code-end", f"0x{last_stub + 0x20:08X}",
        "--functions", os.path.join(meta, f"{name}_functions.json"),
        "--hle-stubs", os.path.join(out, f"{name}_stubs.json"),
        "--symbol-prefix", f"{name}_", "--header-name", f"{name}_recomp.h", "--source-name", f"{name}_recomp.cpp", "--single-file",
        "--jobs", str(os.cpu_count()), "--output", out)


lift_prx("libsre", "0x30000000")
lift_prx("libresc", "0x30100000")

pics = []
for p in ("flashMP3.pic", "flashATRAC.pic"):
    dst = os.path.join(args.firmware, p)
    src = os.path.join(args.dev_flash, "sys", "external", p)
    if not os.path.exists(dst) and os.path.exists(src):
        shutil.copyfile(src, dst)
    if os.path.exists(dst):
        pics += ["--pic", dst]

spu_out = os.path.join(port, "recompiled_prx", "libsre_spu")
shutil.rmtree(spu_out, ignore_errors=True)
run(os.path.join(tools, "lift_firmware_spu.py"), "--name", "libsre", *pics, "--out", spu_out)
