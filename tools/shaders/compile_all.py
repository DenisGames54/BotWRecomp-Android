"""Compile every MSL file latte2msl emitted and report pass/fail counts and failure classes.

  uv run python -I tools/shaders/compile_all.py build-native/shaders [-j 4]

Uses `xcrun -sdk macosx metal` when the offline Metal toolchain is installed (and then also
links a metallib); otherwise falls back to build-native/shaders-tool/mtlcheck, which compiles with
the Metal framework's own compiler (same front end, no .air output). Writes <dir>/compile_report.txt.
"""
import collections
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def have_toolchain() -> bool:
    r = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "--version"], capture_output=True, text=True)
    return r.returncode == 0


def compile_offline(path: str, air: str):
    r = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-std=metal3.1", "-c", path, "-o", air],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return "ok", ""
    m = re.search(r"error: (.*)", r.stderr)
    return "fail", m.group(1) if m else r.stderr.strip().splitlines()[-1:]


def main():
    out = sys.argv[1]
    jobs = int(sys.argv[sys.argv.index("-j") + 1]) if "-j" in sys.argv else 4
    files = sorted(os.path.join(out, "msl", f) for f in os.listdir(os.path.join(out, "msl")) if f.endswith(".metal"))
    results = {}
    if have_toolchain():
        tool = "xcrun metal"
        os.makedirs(os.path.join(out, "air"), exist_ok=True)
        with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
            futs = {ex.submit(compile_offline, f, os.path.join(out, "air", os.path.basename(f)[:-6] + ".air")): f
                    for f in files}
            for fu in concurrent.futures.as_completed(futs):
                results[futs[fu]] = fu.result()
        airs = [os.path.join(out, "air", os.path.basename(f)[:-6] + ".air") for f, (st, _) in results.items() if st == "ok"]
        if airs:
            subprocess.run(["xcrun", "-sdk", "macosx", "metallib", *airs, "-o", os.path.join(out, "botw_shaders.metallib")])
    else:
        tool = "mtlcheck (Metal framework compiler; offline toolchain not installed)"
        exe = os.path.join(ROOT, "build-native", "shaders-tool", "mtlcheck")
        r = subprocess.run([exe, "-j", str(jobs), *files], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            st, _, rest = line.partition(" ")
            path, _, err = rest.partition("\t")
            results[path] = (st, err)
    ok = [f for f, (st, _) in results.items() if st == "ok"]
    fails = {f: e for f, (st, e) in results.items() if st != "ok"}
    by_stage = collections.Counter(os.path.basename(f)[:2] for f in ok)
    total_stage = collections.Counter(os.path.basename(f)[:2] for f in files)
    classes = collections.Counter(re.sub(r"\d+", "N", str(e))[:120] for e in fails.values())
    lines = [f"compiler: {tool}",
             f"compiled {len(ok)}/{len(files)}: " + ", ".join(f"{s} {by_stage[s]}/{total_stage[s]}" for s in sorted(total_stage))]
    lines += [f"  {n:5}  {c}" for c, n in classes.most_common()]
    lines += [f"FAIL {os.path.basename(f)}: {e}" for f, e in sorted(fails.items())]
    report = "\n".join(lines) + "\n"
    open(os.path.join(out, "compile_report.txt"), "w").write(report)
    print("\n".join(lines[:12]))


if __name__ == "__main__":
    main()
