#!/usr/bin/env python3
"""isim kotlin: Kotlin Multiplatform frameworks for isim, built on Linux.

  isim kotlin install [VERSION]          set up Kotlin/Native VERSION for iOS targets (once per version)
  isim kotlin framework -name NAME -include MAIN.klib [-library DEP.klib]... [-klibs FILE] [-o DIR] [-g]
                        [-kotlin VERSION] [-- KONANC ARGS...]
                                         a static NAME.framework for the simulator (x86_64) from compiled klibs
  isim kotlin konanc ARGS...             the Kotlin/Native compiler itself (kotlinc-native), for iOS targets

JetBrains' Kotlin/Native for Linux compiles Kotlin to iOS klibs but refuses to link them: it wants Xcode. isim has what
the link needs (an iOS SDK, ld64.lld, llvm-libtool-darwin), so this tool runs the same compiler with
  - the iOS targets enabled on a Linux host (one line of HostManager.kt, recompiled from the release's source),
  - isim's SDK and LLVM's tools in the place of Xcode (tools/IsimKonan.kt, through Xcode.xcodeOverride),
  - the Kotlin runtime for iOS (LLVM bitcode, the same on every host) taken from the macOS distribution.
No Apple software is involved. Gradle compiles the klibs as usual (`compileKotlinIosX64` works on Linux); `-klibs FILE`
reads their paths from a file, one per line, the first being the one to export (-include).
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.realpath(__file__))
HOME = os.environ.get("ISIM_KOTLIN_HOME") or os.path.join(
    os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "isim-kotlin")
MAVEN = "https://repo1.maven.org/maven2/org/jetbrains/kotlin/kotlin-native-prebuilt"
KOTLIN_RAW = "https://raw.githubusercontent.com/JetBrains/kotlin"
LLVM_RAW = "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-22.1.8/compiler-rt/lib/builtins/cpu_model"
TARGETS = ("ios_x64", "ios_simulator_arm64", "ios_arm64")
HOST_PATCH = ("LINUX_X64 to commonTargets,", "LINUX_X64 to commonTargets + setOf(IOS_X64, IOS_SIMULATOR_ARM64, IOS_ARM64),")


def say(msg):
    print(f"isim kotlin: {msg}", file=sys.stderr, flush=True)


def die(msg):
    say(msg)
    sys.exit(1)


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        die(f"command failed ({r.returncode}): {' '.join(cmd[:6])} ...")
    return r


def download(url, dest):
    say(f"downloading {url}")
    tmp = dest + ".part"
    try:
        with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
    except Exception as e:   # noqa: BLE001 (any network error ends the same way)
        die(f"could not download {url}: {e}")
    os.replace(tmp, dest)


def sdk_dir():
    for d in (os.environ.get("ISIM_SDK"), os.path.join(HERE, "..", "sdk"), os.path.join(HERE, "..", "out", "sdk")):
        if d and os.path.isfile(os.path.join(d, "SDKSettings.json")):
            return os.path.realpath(d)
    die("isim's SDK was not found (set ISIM_SDK)")


def isim_cli():
    for p in (os.path.join(HERE, "isim"), shutil.which("isim")):
        if p and os.path.isfile(p):
            return p
    die("the isim command was not found")


def llvm_tool(*names):
    """An LLVM tool by its plain name, or a versioned one (Ubuntu's ld64.lld-22)."""
    dirs = os.environ.get("PATH", "").split(os.pathsep)
    for name in names:
        for d in dirs:
            if not d or not os.path.isdir(d):
                continue
            for f in sorted(os.listdir(d), reverse=True):
                if f == name or re.fullmatch(re.escape(name) + r"-\d+", f):
                    return os.path.join(d, f)
    die(f"{names[0]} was not found on PATH (LLVM 21 or newer)")


def klib_version(path):
    """The compiler version a klib was built with (its manifest), or None."""
    try:
        if os.path.isdir(path):
            with open(os.path.join(path, "default", "manifest"), encoding="utf-8") as f:
                text = f.read()
        else:
            with zipfile.ZipFile(path) as z:
                text = z.read("default/manifest").decode()
    except (OSError, KeyError, zipfile.BadZipFile):
        return None
    m = re.search(r"^compiler_version=(\S+)", text, re.M)
    return m.group(1) if m else None


def install(version):
    """Kotlin/Native VERSION with the iOS runtime and isim's launcher, in HOME/VERSION; returns that directory."""
    root = os.path.join(HOME, version)
    if os.path.isfile(os.path.join(root, ".ready")):
        return root
    os.makedirs(HOME, exist_ok=True)
    work = root + ".partial"
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    dist = os.path.join(work, "kotlin-native")
    # 1. the Linux distribution: compiler, platform klibs for iOS (they are in it), LLVM for the host
    tgz = os.path.join(work, "linux.tar.gz")
    download(f"{MAVEN}/{version}/kotlin-native-prebuilt-{version}-linux-x86_64.tar.gz", tgz)
    with tarfile.open(tgz) as t:
        t.extractall(work, filter="tar")
    os.rename(os.path.join(work, f"kotlin-native-prebuilt-linux-x86_64-{version}"), dist)
    os.remove(tgz)
    # 2. the Kotlin runtime for iOS: bitcode the Linux distribution leaves out, from the macOS one
    tgz = os.path.join(work, "macos.tar.gz")
    download(f"{MAVEN}/{version}/kotlin-native-prebuilt-{version}-macos-aarch64.tar.gz", tgz)
    with tarfile.open(tgz) as t:
        for m in t:
            parts = m.name.split("/")
            if len(parts) > 3 and parts[1:3] == ["konan", "targets"] and parts[3] in TARGETS and (m.isfile() or m.isdir()):
                m.name = "/".join(parts[1:])
                t.extract(m, dist, filter="tar")
    os.remove(tgz)
    for target in TARGETS:
        if not os.path.isfile(os.path.join(dist, "konan", "targets", target, "native", "runtime.bc")):
            die(f"Kotlin/Native {version} has no runtime for {target}")
    # 3. the launcher, and HostManager with the iOS targets enabled on Linux
    src = os.path.join(work, "src")
    os.makedirs(src)
    host = os.path.join(src, "HostManager.kt")
    download(f"{KOTLIN_RAW}/v{version}/native/utils/src/org/jetbrains/kotlin/konan/target/HostManager.kt", host)
    with open(host, encoding="utf-8") as f:
        text = f.read()
    if HOST_PATCH[0] not in text:
        die(f"Kotlin {version}: HostManager.kt has changed; isim-kotlin.py needs an update for this version")
    with open(host, "w", encoding="utf-8") as f:
        f.write(text.replace(HOST_PATCH[0], HOST_PATCH[1]))
    shutil.copy(os.path.join(HERE, "IsimKonan.kt"), src)
    jar = os.path.join(dist, "konan", "lib", "kotlin-native-compiler-embeddable.jar")
    say("compiling the launcher")
    run(["java", "-cp", jar, "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler", "-no-stdlib", "-no-reflect", "-nowarn",
         "-cp", jar, f"-Xfriend-paths={jar}", "-jvm-target", "17", "-d", os.path.join(work, "classes"),
         host, os.path.join(src, "IsimKonan.kt")])
    # 4. the "Xcode toolchain" the linker steps run: LLVM's tools under Apple's names
    tools = os.path.join(work, "toolchain", "usr", "bin")
    os.makedirs(tools)
    for name, llvm in (("ld", ("ld64.lld",)), ("libtool", ("llvm-libtool-darwin",)), ("strip", ("llvm-strip",)),
                       ("dsymutil", ("dsymutil", "llvm-dsymutil"))):
        with open(os.path.join(tools, name), "w", encoding="utf-8") as f:
            f.write(f'#!/bin/sh\nexec "{llvm_tool(*llvm)}" "$@"\n')
        os.chmod(os.path.join(tools, name), 0o755)
    open(os.path.join(work, ".ready"), "w").close()
    shutil.rmtree(root, ignore_errors=True)
    os.rename(work, root)
    say(f"Kotlin/Native {version} is ready in {root}")
    return root


def konanc(root, args, **kw):
    dist = os.path.join(root, "kotlin-native")
    env = dict(os.environ, ISIM_SDK=sdk_dir(), ISIM_KONAN_TOOLCHAIN=os.path.join(root, "toolchain", "usr"))
    overrides = ";".join([f"targetToolchain.linux_x64-{t}=isim" for t in TARGETS] + ["additionalToolsDir.linux_x64=isim"])
    cmd = ["java", "-Xmx" + os.environ.get("ISIM_KOTLIN_XMX", "6g"), f"-Dkonan.home={dist}", "-Dfile.encoding=UTF-8",
           f"-Djava.library.path={os.path.join(dist, 'konan', 'nativelib')}",
           "-cp", os.path.join(root, "classes") + os.pathsep + os.path.join(dist, "konan", "lib", "kotlin-native-compiler-embeddable.jar"),
           "dev.isim.konan.IsimKonanKt", "konanc", f"-Xoverride-konan-properties={overrides}",
           # Apple's LLVM only: the Linux build of the compiler asserts on it
           "-Xbinary=enableDebugTransparentStepping=false", *args]
    return subprocess.run(cmd, env=env, **kw)


def support_objects(root):
    """Objects every Kotlin framework needs from the linker's side on isim: compiler-rt's __cpu_model (clang adds
    libclang_rt on a Mac), and CoreFoundation calls an older isim SDK lacks."""
    out = os.path.join(root, "support")
    os.makedirs(out, exist_ok=True)
    sdk, cc = sdk_dir(), isim_cli()
    objs = []
    cpu = os.path.join(out, "cpu_model.o")
    if not os.path.isfile(cpu):
        for f in ("x86.c", "cpu_model.h"):
            download(f"{LLVM_RAW}/{f}", os.path.join(out, f))
        run([cc, "cc", "-c", "-O2", os.path.join(out, "x86.c"), "-o", cpu])
    objs.append(cpu)
    header = os.path.join(sdk, "System/Library/Frameworks/CoreFoundation.framework/Headers/CFString.h")
    with open(header, encoding="utf-8") as f:
        has_cf = "CFStringGetBytes" in f.read()
    if not has_cf:
        with open(os.path.join(sdk, "SDKSettings.json"), "rb") as f:
            tag = hashlib.sha256(f.read()).hexdigest()[:12]
        cf = os.path.join(out, f"cfshims-{tag}.o")
        if not os.path.isfile(cf):
            run([cc, "cc", "-c", "-O2", "-fobjc-arc", os.path.join(HERE, "isim-kotlin-cfshims.m"), "-o", cf])
        objs.append(cf)
    return objs


def framework(argv):
    name = out = version = None
    include, libs, extra, debug = None, [], [], False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--":
            extra = argv[i + 1:]
            break
        if a == "-g":
            debug = True
        elif a in ("-name", "-include", "-library", "-klibs", "-o", "-kotlin") and i + 1 < len(argv):
            v = argv[i + 1]
            i += 1
            if a == "-name":
                name = v
            elif a == "-include":
                include = v
            elif a == "-library":
                libs.append(v)
            elif a == "-o":
                out = v
            elif a == "-kotlin":
                version = v
            else:
                with open(v, encoding="utf-8") as f:
                    paths = [line.strip() for line in f if line.strip()]
                if paths and include is None:
                    include, paths = paths[0], paths[1:]
                libs += paths
        else:
            die(f"framework: unknown argument {a} (see isim kotlin --help)")
        i += 1
    if not name or not include:
        die("usage: isim kotlin framework -name NAME (-include MAIN.klib [-library DEP.klib]... | -klibs FILE) [-o DIR] [-g]")
    version = version or klib_version(include) or die(f"{include}: no compiler version in the klib; pass -kotlin VERSION")
    root = install(version)
    out = os.path.abspath(out or ".")
    os.makedirs(out, exist_ok=True)
    fw = os.path.join(out, name + ".framework")
    shutil.rmtree(fw, ignore_errors=True)
    args = ["-target", "ios_x64", "-produce", "framework", "-Xstatic-framework", f"-Xinclude={include}"]
    for lib in libs:
        args += ["-l", lib]
    if debug:
        args.append("-g")
    say(f"{name}.framework (Kotlin {version}, simulator x86_64) -> {fw}")
    r = konanc(root, args + ["-o", os.path.join(out, name)] + extra)
    binary = os.path.join(fw, name)
    if r.returncode or not os.path.isfile(binary):
        die("the Kotlin/Native compiler failed")
    ar = llvm_tool("llvm-ar")
    run([ar, "q", binary, *support_objects(root)], stderr=subprocess.DEVNULL)
    run([ar, "s", binary])
    print(f"isim kotlin: done: {fw}")
    return 0


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return 0
    cmd, rest = argv[0], argv[1:]
    if cmd == "install":
        if len(rest) != 1:
            die("usage: isim kotlin install VERSION   (e.g. 2.2.20: the Kotlin version of your Gradle plugin)")
        install(rest[0])
        return 0
    if cmd == "framework":
        return framework(rest)
    if cmd == "konanc":
        version = os.environ.get("ISIM_KOTLIN_VERSION") or die("konanc: set ISIM_KOTLIN_VERSION (e.g. 2.2.20)")
        return konanc(install(version), rest).returncode
    die(f"unknown command {cmd} (install, framework, konanc)")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
