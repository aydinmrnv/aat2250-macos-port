#!/usr/bin/env python3
"""Build llama.cpp for x86_64 macOS (the Hackintosh) with Apple clang and no CMake.

The source lists come from llama.cpp's own CMakeLists, so this follows the pinned tree instead of a copy of it.
CPU backend for the i9-285K (AVX2/FMA/F16C/BMI2/AVX-VNNI; no AVX-512 on Arrow Lake). With --cuda, ggml's CUDA
backend (an archive of nvcc-built objects) is linked over our CUDA stack: libtinycudart + libtinycublas + nvrmcu.

    python3 build-llama.py                 CPU-only llama-bench, llama-cli-simple
    python3 build-llama.py --cuda A.a      the same with -DGGML_USE_CUDA and ggml-cuda linked in
"""
import concurrent.futures, glob, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
L = os.environ.get("LLAMA", os.path.join(HERE, "llama.cpp"))
B = os.environ.get("OUT", os.path.join(HERE, "build", "llama"))
SDKMIN = "15.0"
CUDA = "--cuda" in sys.argv
CUDA_AR = sys.argv[sys.argv.index("--cuda") + 1] if CUDA else None
tag = "cuda" if CUDA else "cpu"
OBJ = os.path.join(B, "obj")
os.makedirs(OBJ, exist_ok=True)

def block(path, start_re):
    """The file names inside the first `add_library(<start_re> ...)` / `set(<start_re> ...)` block of a CMakeLists."""
    txt = open(path).read()
    m = re.search(start_re + r"(.*?)\)", txt, re.S)
    if not m:
        sys.exit("no block %r in %s" % (start_re, path))
    return [w for w in m.group(1).split() if w.endswith((".c", ".cpp"))]

def rel(base, names):
    return [os.path.normpath(os.path.join(base, n.replace("${CMAKE_CURRENT_LIST_DIR}/", ""))) for n in names]

gsrc = os.path.join(L, "ggml", "src")
ggml_base = rel(gsrc, block(os.path.join(gsrc, "CMakeLists.txt"), r"add_library\(ggml-base"))
ggml_reg = [os.path.join(gsrc, "ggml-backend-reg.cpp"), os.path.join(gsrc, "ggml-backend-dl.cpp")]
cpu_cmake = os.path.join(gsrc, "ggml-cpu", "CMakeLists.txt")
cpu = rel(gsrc, block(cpu_cmake, r"list \(APPEND GGML_CPU_SOURCES"))
cpu += rel(gsrc, ["ggml-cpu/llamafile/sgemm.cpp", "ggml-cpu/arch/x86/quants.c", "ggml-cpu/arch/x86/repack.cpp"])
cpu = sorted(set(c for c in cpu if os.path.exists(c)))
sdir = os.path.join(L, "src")
llama = rel(sdir, block(os.path.join(sdir, "CMakeLists.txt"), r"set\(LLAMA_CORE_SOURCES")) + sorted(glob.glob(os.path.join(sdir, "models", "*.cpp")))
cdir = os.path.join(L, "common")
common = rel(cdir, block(os.path.join(cdir, "CMakeLists.txt"), r"add_library\(\$\{TARGET\}\s+\$\{LLAMA_CHAT_PARSERS_SOURCES\}"))
common += rel(os.path.join(cdir, "parsers"), block(os.path.join(cdir, "parsers", "sources.cmake"), r"set\(LLAMA_CHAT_PARSERS_SOURCES"))
common += [os.path.join(L, "vendor", "cpp-httplib", "httplib.cpp")]

# generated headers
gen = os.path.join(B, "gen")
os.makedirs(gen, exist_ok=True)
commit = subprocess.run(["git", "-C", L, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip() or "unknown"
open(os.path.join(gen, "ggml-version.h"), "w").write('#pragma once\n#define GGML_VERSION "0.0.0"\n#define GGML_COMMIT "%s"\n' % commit)
subst = {"LLAMA_BUILD_NUMBER": "0", "LLAMA_BUILD_COMMIT": commit, "BUILD_COMPILER": "Apple clang",
         "BUILD_TARGET": "x86_64-apple-macos", "LLAMA_VERSION": "0.0.0-" + commit}
def configure(src, dst):
    t = open(src).read()
    for k, v in subst.items():
        t = t.replace("@%s@" % k, v)
    left = re.findall(r"@[A-Z_]+@", t)
    if left:
        sys.exit("unfilled placeholders %s in %s" % (left, src))
    open(dst, "w").write(t)
configure(os.path.join(cdir, "build-info.cpp.in"), os.path.join(gen, "build-info.cpp"))
configure(os.path.join(sdir, "llama-version.h.in"), os.path.join(gen, "llama-version.h"))
common.append(os.path.join(gen, "build-info.cpp"))

CC = ["xcrun", "clang", "-arch", "x86_64", "-mmacosx-version-min=" + SDKMIN]
CXX = ["xcrun", "clang++", "-arch", "x86_64", "-mmacosx-version-min=" + SDKMIN]
INC = ["-I" + p for p in [gen, os.path.join(L, "ggml", "include"), gsrc, os.path.join(gsrc, "ggml-cpu"), os.path.join(L, "include"),
                          sdir, cdir, os.path.join(L, "vendor"), os.path.join(L, "vendor", "cpp-httplib"), os.path.join(L, "tools", "llama-bench")]]
DEFS = ["-DNDEBUG", "-DGGML_SCHED_MAX_COPIES=4", "-D_XOPEN_SOURCE=600", "-D_DARWIN_C_SOURCE", "-DGGML_USE_CPU"]
CPUF = ["-mavx", "-mavx2", "-mfma", "-mf16c", "-mbmi2", "-mavxvnni", "-DGGML_USE_LLAMAFILE", "-DGGML_USE_CPU_REPACK"]
OPT = ["-O3", "-fno-common", "-Wno-unused-command-line-argument"]

def obj_of(src):
    return os.path.join(OBJ, os.path.relpath(src, L).replace("/", "_").replace("..", "up") + ".o")

def compile_one(args):
    src, extra = args
    out = obj_of(src)
    if os.path.exists(out) and os.path.getmtime(out) > os.path.getmtime(src) and "backend-reg" not in src:
        return None
    cmd = (CC + ["-std=c11"] if src.endswith(".c") else CXX + ["-std=c++17"]) + OPT + DEFS + extra + INC + ["-c", src, "-o", out]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return None if r.returncode == 0 else (src, r.stderr[-3000:])

# common/ and src/ both have a unicode.h: each library sees its own first, as its CMake target does
jobs = [(s, []) for s in ggml_base + llama] + [(s, ["-I" + cdir]) for s in common] + [(s, CPUF) for s in cpu]
jobs += [(s, (["-DGGML_USE_CUDA"] if CUDA else [])) for s in ggml_reg]
bench = [os.path.join(L, "tools", "llama-bench", "llama-bench.cpp"), os.path.join(L, "tools", "llama-bench", "main.cpp")]
jobs += [(s, []) for s in bench]
print("compiling %d files (%d ggml-base, %d cpu, %d llama, %d common)" % (len(jobs), len(ggml_base), len(cpu), len(llama), len(common)))
with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
    fails = [f for f in ex.map(compile_one, jobs) if f]
for src, err in fails:
    print("FAIL", src, "\n", err)
if fails:
    sys.exit("%d files failed" % len(fails))

def ar(name, srcs):
    a = os.path.join(B, name)
    if os.path.exists(a):
        os.remove(a)
    subprocess.check_call(["ar", "rcs", a] + [obj_of(s) for s in srcs])
    return a

libs = [ar("libllama-common.a", common), ar("libllama.a", llama), ar("libggml-reg-%s.a" % tag, ggml_reg),
        ar("libggml-cpu.a", cpu), ar("libggml-base.a", ggml_base)]
link = CXX + ["-o", os.path.join(B, "llama-bench-" + tag)] + [obj_of(s) for s in bench] + libs
link += ["-framework", "Accelerate", "-framework", "Foundation"]
if CUDA:
    R = os.environ.get("CUDART_BUILD", os.path.join(HERE, "build"))
    link += [CUDA_AR, os.path.join(R, "libtinycublas.a"), os.path.join(R, "libtinycudart.a"), os.path.join(R, "libnvrmcu.a"),
             "-framework", "IOKit", "-framework", "CoreFoundation"]
r = subprocess.run(link + ["-lc++"], capture_output=True, text=True)
if r.returncode:
    print(r.stderr[-4000:])
    sys.exit("link failed")
print("built", os.path.join(B, "llama-bench-" + tag))
