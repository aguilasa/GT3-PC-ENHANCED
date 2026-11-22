"""compare.py - member-by-member record layouts of the game, Clang i686 Linux against clang-cl.

The decompiled game and engine (the gt3_game and gt3_engine object targets)
must lay their records out the same on Linux i386 (Clang, SysV ABI, Itanium
C++ ABI) as on Windows (clang-cl, MSVC ABI): the game files, the saves and
the relocated data are read straight into these structs.  This dumps the
record layouts both compilers compute for every translation unit of the two
targets (`-Xclang -fdump-record-layouts`, syntax only), from the
compile_commands.json of each build tree, and compares every record that
appears on both sides: sizeof, alignment, and the offset of every base,
member and bitfield (bit range included), members of anonymous structs and
unions too.  Members named pcTail_* (PC_TAIL_PAD in include/types.h, Linux
only, filling the tail padding a GCC / Clang derived class would reuse) are
skipped; records of std::, of the compiler / CRT (names starting with _)
and lambdas of system headers are not compared (each side has its own
standard library); the records found on one side only are listed.

A record that differs fails the run unless it is in EXCEPTIONS below; each
exception says why the difference is harmless (C++ ABI of a polymorphic class,
pointers to members, a different STL, or PC-only code that is never read from
or written to the game files, the saves or relocated data).  Only add one
after checking that.  A decompiled struct read from a file that differs is a
layout bug: fix the header, do not list it.

    python3 tools/layout_probe/compare.py --linux-build build-linux --win-build build-win

The Linux dumps run in the gt3-i386 container (docker, the same invocation
as the build: the user's id, $HOME mounted at the same path); --container ''
runs them directly (already inside the container).  The Windows dumps run on
the host with the compiler named in build-win/compile_commands.json.  About
a minute each with 16 jobs.

Writes, under --out (default <linux-build>/layout_probe):
    linux/*.txt, win/*.txt    the raw dumps, one per translation unit
    report.txt                the same report as stdout

Exit status: 0 when every common record matches or is a listed exception,
1 when a record differs unexpectedly, 2 when a translation unit fails to
dump or no record was compared.

Checks of the checker:
    --linux-flag F    adds F to every Linux compile (e.g. -fpack-struct=4
                      breaks the 8-byte alignment of u64: the run must fail);
    --perturb REC     shifts the last member of REC by 4 on the Linux side
                      after parsing (the run must fail on REC);
    --reuse           compares dumps already under --out without recompiling.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TARGETS = ("gt3_game.dir", "gt3_engine.dir")

# Records whose layout is allowed to differ between the two targets: name ->
# why it does not matter.  Each one was checked: no file, save or relocated
# data is read into it or written from it (no fread / fwrite / memcpy of the
# whole record, no sizeof of it outside the asserts noted).  The run lists
# every exception it used and warns about the ones that no longer differ.
# Records of an anonymous namespace carry the unit they come from; unnamed
# ones their place (path relative to the repository).
EXCEPTIONS = {
    # vfptr: MSVC rounds the space of the vfptr up to the class's alignment
    # (data at +8 when the class holds 8-aligned members); Itanium puts it at +4.
    # The game.h EGCS_SIZE note already says polymorphic classes take the PC
    # compiler's layout.
    "RaceEntryCar": "polymorphic: MSVC pads the vfptr to 8; allocated by its own operator new "
                    "and filled by the race set-up, never read from or written to a file",
    "RaceOrganization": "polymorphic: MSVC pads the vfptr to 8; allocated by its own operator new "
                        "and filled by the race set-up, never read from or written to a file",
    # Pointers to members: MSVC 12/16 bytes, Itanium 4/8 (the EGCS_SIZE_ABI case).
    # The Linux sizes are the PS2 ones the headers assert for egcs.
    "IOPLoadCommand": "pointer to member function (MSVC 12 bytes, Itanium 8): Linux is the PS2 size "
                      "0xC; a command table in the code, never in a file",
    "RaceMeterLayout": "pointer to data member (MSVC 12 bytes, Itanium 4): Linux is the PS2 size "
                       "0x10; a table built by a static initializer, never in a file",
    # Bitfield allocation.
    "gt2k_menu::CarGetControl": "8-bit bitfields of unsigned int after a s8: GCC/Clang put carsShown/done "
                                "at unk_ED4+1/+2, the PS2 layout the header documents (0xED4, 0xED5, "
                                "0xED6); MSVC opens a new unsigned int at +4.  The Linux side is the "
                                "PS2-correct one; polymorphic menu control, never in a file",
    # Different standard library (MS STL against libstdc++).
    "paramdb::ExtraTable": "std::string / std::vector / std::unordered_map members (MS STL against "
                           "libstdc++); PC-only settings database, in memory",
    "(anonymous namespace)::Pool [src/game/app/pc_prize_cars.cpp]":
        "std::vector / std::unordered_map members (MS STL against libstdc++); PC-only, in memory",
    "(anonymous namespace)::Pool [src/game/race/dynamics/pc_physics_threads.cpp]":
        "std::mutex / std::condition_variable members (MS STL against libstdc++); PC-only "
        "physics worker pool, in memory",
    # PC-only code with raw 8-byte types (double, int64_t, uint64_t; not the
    # aligned u64/s64/f64 typedefs): SysV i386 aligns them to 4 inside a struct.
    "PglVGlyph": "PC-only vector-font glyph (uint64_t tex0), filled by pgl_vfont, in memory only",
    "paramdb::Value": "PC-only SQLite value (int64_t, double, std::string); in memory, stored "
                      "member by member through SQLite",
    "SndNativeBgmDecoder": "PC-only decoder callbacks (uint64_t frames); built on the stack, in memory only",
    "SndNativeBgmStatus": "PC-only BGM status (int64_t time): same sizeof (0x40) and member offsets on "
                          "both sides, only its alignment differs (4/8); copied byte for byte into "
                          "the decomp's BgmStatus, which is identical on both",
    "gt3media::Mp4AudioReader": "PC-only Media Foundation reader state (int64_t skipTo_), in memory only",
    "spu2::AdsListTrack": "PC-only ads.txt track (int64_t, double, std::string), parsed from text, "
                          "in memory only",
    "PcTween": "PC-only frame-interpolation slot (double), in memory only",
    "(anonymous namespace)::Still [src/game/app/pc_still.cpp]":
        "PC-only still picture slot (uint64_t tex0, std::vector), in memory only",
    "(anonymous namespace)::Decode [src/game/app/pc_still.cpp]":
        "PC-only PNG decode job (double pixelAspect), in memory only",
    "(anonymous namespace)::Mp3Bgm [src/engine/pdi/T521_pdibgm.cpp]":
        "PC-only MP3 BGM state: wchar_t path[1024] (2 bytes on Windows, 4 on Linux), "
        "Mp4AudioReader, std::vector; in memory only",
    "(unnamed at src/game/app/pc_debugcam.cpp:46:8)":
        "PC-only debug camera listener state s_listen (double at), in memory only",
    "(unnamed at src/game/app/pc_debugcam.cpp:59:8)":
        "PC-only debug camera state s (double last), in memory only",
}


# ------------------------------------------------------------------ dumping
def units(build):
    """The compile_commands.json entries of the game and engine targets."""
    with open(os.path.join(build, "compile_commands.json"), encoding="utf-8") as f:
        db = json.load(f)
    return [e for e in db if any(t in e.get("output", "") + e["command"] for t in TARGETS)]


def dump_name(entry):
    rel = os.path.relpath(entry["file"], ROOT)
    return rel.replace(os.sep, "__").replace("..", "_") + ".txt"


def linux_args(entry, extra):
    a = shlex.split(entry["command"])
    if "-o" in a:
        i = a.index("-o")
        del a[i:i + 2]
    a = [x for x in a if x != "-c"]
    return a + list(extra) + ["-fsyntax-only", "-Xclang", "-fdump-record-layouts"]


def win_args(entry, extra):
    a = shlex.split(entry["command"])
    a = [x for x in a if not (x.startswith(("/Fo", "/Fd", "-Fo", "-Fd")) or x in ("-c", "/c", "-Zi", "/Zi"))]
    i = a.index("--") if "--" in a else len(a) - 1
    a[i:i] = list(extra) + ["/Zs", "-Xclang", "-fdump-record-layouts"]
    return a


def dump(side, build, outdir, jobs, extra):
    """Dump every unit of build into outdir; returns the failed units."""
    os.makedirs(outdir, exist_ok=True)
    for fn in os.listdir(outdir):
        if fn.endswith(".txt"):
            os.remove(os.path.join(outdir, fn))
    make = linux_args if side == "linux" else win_args
    es = units(build)

    def run(e):
        path = os.path.join(outdir, dump_name(e))
        with open(path, "w", encoding="utf-8") as f:
            r = subprocess.run(make(e, extra), cwd=e["directory"], stdout=f, stderr=subprocess.PIPE,
                               errors="replace")
        return None if r.returncode == 0 else (e["file"], r.stderr.strip().splitlines()[-3:])

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        failed = [x for x in ex.map(run, es) if x]
    with open(os.path.join(outdir, "units.json"), "w", encoding="utf-8") as f:
        json.dump({"units": len(es), "failed": failed}, f, indent=1)
    return len(es), failed


def dump_linux(args, outdir):
    """The Linux dumps, inside the container unless --container is empty."""
    if not args.container:
        return dump("linux", args.linux_build, outdir, args.jobs, args.linux_flag)
    home = os.path.expanduser("~")
    os.makedirs(outdir, exist_ok=True)          # (here, so docker does not create it as root)
    mounts = []
    for p in (home, ROOT, os.path.realpath(args.linux_build), os.path.realpath(args.out)):
        if not any(p == m or p.startswith(m + os.sep) for m in mounts):
            mounts.append(p)
    cmd = ["docker", "run", "--rm", "--user", f"{os.getuid()}:{os.getgid()}", "-e", "HOME=" + home]
    for m in mounts:
        cmd += ["-v", f"{m}:{m}"]
    cmd += ["-w", ROOT, args.container, "python3", os.path.abspath(__file__), "--dump-linux-only",
            "--container", "", "--linux-build", os.path.realpath(args.linux_build),
            "--out", os.path.realpath(args.out), "--jobs", str(args.jobs)]
    for f in args.linux_flag:
        cmd.append("--linux-flag=" + f)
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(f"container run failed ({r.returncode}): {' '.join(cmd)}")
    with open(os.path.join(outdir, "units.json"), encoding="utf-8") as f:
        st = json.load(f)
    return st["units"], st["failed"]


# ------------------------------------------------------------------ parsing
LINE_RE = re.compile(r"^\s*(\d+)(?::(\d+)-(\d+))? \|( +)(.*)$")
HEAD_RE = re.compile(r"^\s*0 \| (?:class|struct|union) (.*?)(?: \(empty\))?$")
SIZE_RE = re.compile(r"sizeof=(\d+)")
ALIGN_RE = re.compile(r"\balign=(\d+)")
# An anonymous struct/union member: the type is "(anonymous at file:line:col)"
# and no member name follows.
ANON_RE = re.compile(r"\(anonymous at [^)]*\)\s*$")


def entry_key(text):
    """(kind, name) of one dump line: a base, the vptr, or a member by name."""
    text = re.sub(r"\s*\(empty\)$", "", text)
    if "vftable pointer" in text or "vtable pointer" in text:
        return "vptr", ""
    if "vbtable pointer" in text:
        return "vbptr", ""
    m = re.match(r"(?:class|struct|union) (.*) \((primary |virtual )?base\)$", text)
    if m:
        return "base", m.group(1)
    return "member", text.rsplit(None, 1)[-1] if text.split() else text


def is_anon(text):
    return bool(ANON_RE.search(text))


def parse_block(lines):
    """One record: (name, sizeof, align, [(offset, bits, path, text)]).
    Entries are the record's own bases and members, plus the members of
    anonymous structs/unions nested in it (path keeps their nesting)."""
    m = HEAD_RE.match(lines[0])
    if not m:
        return None
    name = m.group(1)
    body = "\n".join(lines)
    sz, al = SIZE_RE.search(body), ALIGN_RE.search(body)
    ents, stack = [], []          # stack: (depth, key, anonymous) of the open parents
    for l in lines[1:]:
        mm = LINE_RE.match(l)
        if not mm:
            continue
        depth = (len(mm.group(4)) - 1) // 2
        text = mm.group(5)
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if depth == 1 or (stack and all(s[2] for s in stack if s[0] >= 1)):
            key = entry_key(text)
            if not key[1].startswith("pcTail_"):
                path = tuple(s[1] for s in stack if s[0] >= 1) + (key,)
                bits = (int(mm.group(2)), int(mm.group(3))) if mm.group(2) else None
                ents.append((int(mm.group(1)), bits, path, text))
        stack.append((depth, entry_key(text), is_anon(text) and depth >= 1))
    return name, sz and int(sz.group(1)), al and int(al.group(1)), ents


def unit_of(dump_file):
    """The source path a dump file name stands for (dump_name reversed)."""
    return dump_file[:-len(".txt")].replace("__", "/")


def skip(name):
    """Records of the standard library, the CRT or the compiler (names with a
    leading _, lambdas of system headers): each side has its own."""
    head = name.split("::")[0]
    return name.startswith("std::") or head.startswith("_") or name.startswith("(lambda at /")


def load(d):
    """name -> {layout: first dump file} for every record of the dumps in d."""
    recs = {}
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".txt"):
            continue
        with open(os.path.join(d, fn), encoding="utf-8", errors="replace") as f:
            blocks = f.read().split("*** Dumping AST Record Layout")[1:]
        for b in blocks:
            r = parse_block(b.strip("\n").split("\n"))
            if not r:
                continue
            name, sz, al, ents = r
            name = name.replace(ROOT + os.sep, "")
            if skip(name):
                continue
            if "(anonymous namespace)" in name:
                name += f" [{unit_of(fn)}]"
            lay = (sz, al, tuple((o, bits, path) for o, bits, path, _ in ents))
            recs.setdefault(name, {}).setdefault(lay, (fn, ents))
    return recs


def describe(name, w, l):
    """Lines telling how the layouts of name differ (first variant of each side)."""
    (wl, (wfn, wents)), (ll, (lfn, lents)) = next(iter(w.items())), next(iter(l.items()))
    out = [f"  {name}: sizeof {wl[0]}/{ll[0]} align {wl[1]}/{ll[1]} (win/linux)"
           + (f"; {len(w)}/{len(l)} variants" if len(w) > 1 or len(l) > 1 else "")]
    wm = {p: (o, b, t) for o, b, p, t in wents}
    lm = {p: (o, b, t) for o, b, p, t in lents}
    shown = 0
    for p in list(wm) + [p for p in lm if p not in wm]:
        a, c = wm.get(p), lm.get(p)
        if a and c and a[:2] == c[:2]:
            continue
        fmt = lambda x: "-" if not x else f"{x[0]:#x}" + (f":{x[1][0]}-{x[1][1]}" if x[1] else "")
        label = ".".join(k[1] or k[0] for k in p)
        out.append(f"    {label}: win {fmt(a)} linux {fmt(c)}")
        shown += 1
        if shown >= 6:
            out.append("    ...")
            break
    out.append(f"    dumps: win/{wfn} linux/{lfn}")
    return out


def perturb(recs, name):
    if name not in recs:
        sys.exit(f"--perturb: no record {name} on the Linux side")
    new = {}
    for (sz, al, ents), (fn, raw) in recs[name].items():
        ents = list(ents)
        o, b, p = ents[-1]
        ents[-1] = (o + 4, b, p)
        raw = list(raw)
        ro, rb, rp, rt = raw[-1]
        raw[-1] = (ro + 4, rb, rp, rt)
        new[(sz, al, tuple(ents))] = (fn, raw)
    recs[name] = new


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--linux-build", default="build-linux")
    ap.add_argument("--win-build", default="build-win")
    ap.add_argument("--out", help="output directory (default <linux-build>/layout_probe)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--container", default="gt3-i386", help="docker image for the Linux dumps ('' = run here)")
    ap.add_argument("--linux-flag", action="append", default=[], help="extra Linux compile flag (control runs)")
    ap.add_argument("--perturb", metavar="RECORD", help="shift RECORD's last member on the Linux side (control)")
    ap.add_argument("--reuse", action="store_true", help="compare the dumps already under --out")
    ap.add_argument("--dump-linux-only", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()
    args.out = args.out or os.path.join(args.linux_build, "layout_probe")
    ldir, wdir = os.path.join(args.out, "linux"), os.path.join(args.out, "win")

    if args.dump_linux_only:
        dump("linux", args.linux_build, ldir, args.jobs, args.linux_flag)
        return 0

    report = []

    def say(s=""):
        print(s)
        report.append(s)

    status = 0
    if not args.reuse:
        for side, run in (("linux", lambda: dump_linux(args, ldir)),
                          ("win", lambda: dump("win", args.win_build, wdir, args.jobs, []))):
            n, failed = run()
            say(f"{side}: {n} units dumped, {len(failed)} failed")
            for fn, err in failed:
                say(f"  FAILED {fn}")
                for e in err:
                    say(f"    {e}")
            if failed or not n:
                status = 2

    w, l = load(wdir), load(ldir)
    if args.perturb:
        perturb(l, args.perturb)
    common = sorted(set(w) & set(l))
    equal, hit, bad = [], [], []
    for n in common:
        (equal if set(w[n]) == set(l[n]) else hit if n in EXCEPTIONS else bad).append(n)
    if args.linux_flag or args.perturb:
        say(f"CONTROL RUN: linux flags {args.linux_flag or '-'}, perturbed {args.perturb or '-'}")
    say(f"records: win {len(w)}, linux {len(l)}, compared (on both sides) {len(common)}, "
        f"equal {len(equal)}, accepted exceptions {len(hit)}, unexpected differences {len(bad)}")
    nbits = sum(1 for n in common if any(b for lay in l[n] for _, b, _ in lay[2]))
    nanon = sum(1 for n in common if any(len(p) > 1 for lay in l[n] for _, _, p in lay[2]))
    say(f"  of the compared: {nbits} with bitfields (bit ranges compared), "
        f"{nanon} with anonymous struct/union members (compared inside)")
    for side, mine, other in (("win", w, l), ("linux", l, w)):
        only = sorted(set(mine) - set(other))
        say(f"only in {side} ({len(only)}, not compared): " + (", ".join(only) or "-"))
    say()
    say("Accepted exceptions (reviewed, see EXCEPTIONS in tools/layout_probe/compare.py):")
    for n in hit:
        say(f"  {n}: {EXCEPTIONS[n]}")
        for d in describe(n, w[n], l[n])[:-1]:
            say("  " + d)
    stale = [n for n in EXCEPTIONS if n not in hit]
    for n in stale:
        say(f"  (not hit: {n} " + ("matches now" if n in common else "not on both sides") + ")")
    say()
    say(f"Unexpected differences: {len(bad)}")
    for n in bad:
        for d in describe(n, w[n], l[n]):
            say(d)
    if bad:
        status = max(status, 1)
    if not common:
        status = 2
    say()
    say("RESULT: " + ("OK" if status == 0 else "FAIL") + f" (exit {status})")
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "report.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(report) + "\n")
    return status


if __name__ == "__main__":
    sys.exit(main())
