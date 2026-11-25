#!/usr/bin/env python3
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from build_spu_workloads import debase_includes
from spu_elf import scan_spu_candidates


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("failed: " + " ".join(cmd[:3]))
    return r.stdout + r.stderr


def lift(cmd, d):
    report = os.path.join(d, "lift.json")
    log = run(cmd + ["--report", report])
    open(os.path.join(d, "lift.log"), "w").write(log)
    diag = json.load(open(report))
    if diag.get("unsupported"):
        raise SystemExit("%s: unsupported instructions %s" % (d, diag["unsupported"]))
    src = os.path.join(d, "spu_recomp.c")
    for f in ("spu_recomp.c", "spu_recomp.h"):
        debase_includes(os.path.join(d, f))
    if "/* TODO spu:" in open(src).read():
        raise SystemExit("%s: unresolved instruction or branch" % d)
    return src, len(diag.get("functions", []))


def main():
    ap = argparse.ArgumentParser(
        description="Lift the position-independent firmware SPU ELFs (dev_flash .pic plugins) a title "
                    "loads at a run-time LS address: each is lifted relocatable at its link address and "
                    "registered by its code bytes.")
    ap.add_argument("--name", required=True, help="symbol prefix stem, e.g. libsre")
    ap.add_argument("--pic", action="append", default=[], help="a .pic plugin ELF")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    py = sys.executable
    lifter = os.path.join(HERE, "spu_lifter.py")
    regs = []
    sources = []
    image_id = 100

    for path in a.pic:
        elf_bytes = open(path, "rb").read()
        stem = re.sub(r"[^A-Za-z0-9_]", "_", os.path.splitext(os.path.basename(path))[0])
        name = "%s_pic_%s" % (a.name, stem)
        d = os.path.join(a.out, name)
        os.makedirs(d, exist_ok=True)
        elf = os.path.join(d, "image.elf")
        open(elf, "wb").write(elf_bytes)
        info = next((i for o, i, e in scan_spu_candidates(elf_bytes) if o == 0 and not e), None)
        if not info or len(info["executable"]) != 1:
            raise SystemExit("%s: not a single-code-segment SPU ELF" % path)
        x = info["executable"][0]
        text = [s["addr"] + s["size"] for s in info["sections"]
                if s["flags"] & 4 and s["size"] and x["addr"] <= s["addr"] < x["addr"] + x["filesz"]]
        code_end = max(text) if text else x["addr"] + x["filesz"]
        code = elf_bytes[x["off"]:x["off"] + (code_end - x["addr"])]
        src, n = lift([py, lifter, "--auto-functions", elf, "--reloc", "--symbol-prefix", name + "_",
                       "--code-end", hex(code_end), "--merge-chunks", "-o", d], d)
        print("%s: %s code 0x%X at link 0x%X, %d functions, image %d"
              % (name, os.path.basename(path), len(code), x["addr"], n, image_id))
        regs.append((name, image_id, code, x["addr"]))
        sources.append(src)
        image_id += 1

    lines = ["#include <stdint.h>", "",
             "void spu_begin_image(int image_id);",
             "int spu_overlay_register_reloc_content(const uint8_t* code, uint32_t span, uint32_t canon, int image_id);"]
    lines += ["void %s_spu_recomp_register(void);" % name for name, _, _, _ in regs]
    for name, _, code, _ in regs:
        lines += ["", "static const uint8_t %s_code[0x%X] = {" % (name, len(code))]
        lines += ["    " + "".join("0x%02X," % c for c in code[k:k + 24]) for k in range(0, len(code), 24)]
        lines += ["};"]
    lines += ["", "void %s_spu_register(void)" % a.name, "{"]
    for name, iid, code, canon in regs:
        lines.append("    spu_begin_image(%d);" % iid)
        lines.append("    %s_spu_recomp_register();" % name)
        lines.append("    spu_overlay_register_reloc_content(%s_code, 0x%Xu, 0x%Xu, %d);" % (name, len(code), canon, iid))
    lines += ["    spu_begin_image(0);", "}", ""]
    reg = os.path.join(a.out, "%s_spu_register.c" % a.name)
    open(reg, "w").write("\n".join(lines))
    sources.append(reg)
    with open(os.path.join(a.out, "sources.cmake"), "w") as f:
        f.write("set(FIRMWARE_SPU_SOURCES\n" + "".join("  [==[%s]==]\n" % os.path.abspath(s).replace("\\", "/") for s in sources) + ")\n")
    print("wrote", reg)


if __name__ == "__main__":
    main()
