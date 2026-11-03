#!/usr/bin/env python3
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess
import sys
import tarfile

IDENT = re.compile(r"^[A-Za-z_]\w*$")
SOURCE_SUFFIXES = (".o", ".obj", ".cpp", ".cc", ".cxx", ".c++", ".c", ".S", ".s")


def load_compile_commands(binary_dir: Path, source_dir: Path):
    for cand in (binary_dir / "compile_commands.json", source_dir / "build" / "compile_commands.json"):
        if cand.is_file():
            return json.loads(cand.read_text(encoding="utf-8"))
    raise SystemExit(f"component_as_lib: compile_commands.json not found under {binary_dir}")


def strip_io(args):
    out, skip = [], False
    for arg in args:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-c"):
            skip = arg == "-o"
            continue
        if arg.endswith(SOURCE_SUFFIXES) and not arg.startswith("-"):
            continue
        out.append(arg)
    return out


def donor(cc, source_dir: Path, suffix: str):
    main = str(source_dir / "src" / "main.cpp")
    for entry in cc:
        if suffix == ".cpp" and entry.get("file", "") == main:
            return entry
    for entry in cc:
        file = entry.get("file", "")
        if file.endswith(suffix) and "/src/esphome/" in file.replace("\\", "/"):
            return entry
    for entry in cc:
        if entry.get("file", "").endswith(suffix):
            return entry
    return None


def command(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def tool(compiler: str, name: str) -> str:
    base = Path(compiler).name
    if "-" not in base:
        return name
    return str(Path(compiler).with_name(base.rsplit("-", 1)[0] + "-" + name))


def thread_local_symbols(nm: str, elf: Path):
    result = subprocess.run([tool(nm, "readelf"), "-sW", str(elf)], capture_output=True, text=True)
    names = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[3] == "TLS":
            names.add(parts[7].split("@", 1)[0])
    return names


def global_symbols(nm: str, elf: Path):
    result = subprocess.run([nm, "-g", "--defined-only", str(elf)], capture_output=True, text=True)
    thread_local = thread_local_symbols(nm, elf)
    names = set()
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "TDBRWVGSA" and IDENT.match(parts[2]) and parts[2] not in thread_local:
            names.add(parts[2])
    return sorted(names)


def undefined_symbols(nm: str, so: Path):
    result = subprocess.run([nm, "-u", str(so)], capture_output=True, text=True)
    return sorted({line.split()[-1] for line in result.stdout.splitlines() if line.strip()})


def write_if_changed(path: Path, text: str) -> bool:
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return False
    path.write_text(text, encoding="utf-8")
    return True


def symbol_table(source_dir: Path, names):
    lines = [
        "#include <stddef.h>",
        "#include <stdint.h>",
        "#pragma GCC diagnostic push",
        '#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"',
    ]
    lines += [f"extern int {name};" for name in names]
    lines += [
        "#pragma GCC diagnostic pop",
        "typedef struct { const char *name; uintptr_t addr; } cal_sym_t;",
        "static const cal_sym_t cal_symbols[] = {",
    ]
    lines += [f'    {{"{name}", (uintptr_t) &{name}}},' for name in names]
    lines += [
        "};",
        "uintptr_t component_as_lib_find_symbol(const char *name) {",
        "    size_t low = 0;",
        "    size_t high = sizeof(cal_symbols) / sizeof(cal_symbols[0]);",
        "    while (low < high) {",
        "        size_t mid = (low + high) / 2;",
        "        int order = __builtin_strcmp(cal_symbols[mid].name, name);",
        "        if (order == 0)",
        "            return cal_symbols[mid].addr;",
        "        if (order < 0)",
        "            low = mid + 1;",
        "        else",
        "            high = mid;",
        "    }",
        "    return 0;",
        "}",
        "",
    ]
    out = source_dir / "component_libs" / "component_as_lib_symbols.c"
    out.parent.mkdir(parents=True, exist_ok=True)
    write_if_changed(out, "\n".join(lines))
    print(f"component_as_lib: symbol table {len(names)} symbols the packages need -> {out}")


def failure(obj_dir: Path, source: Path, stderr: str) -> str:
    log = obj_dir / f"{Path(source).stem}.log"
    log.write_text(stderr, encoding="utf-8")
    errors = [line for line in stderr.splitlines() if "error:" in line or "undefined reference" in line]
    return "\n".join(errors[:60]) + f"\n({len(errors)} errors, full output {log})"


DATA_WINDOW = 0x08000000


def window_script(cxx: str, obj_dir: Path) -> Path:
    result = subprocess.run([cxx, "-shared", "-nostdlib", "-Wl,--verbose", "-o", os.devnull],
                            capture_output=True, text=True)
    parts = result.stdout.split("=" * 50)
    if len(parts) < 3:
        raise SystemExit("component_as_lib: no default linker script from the toolchain")
    script = parts[1]
    script = re.sub(r"\.\s*=\s*DATA_SEGMENT_ALIGN\s*\([^;]*\);", f". = {DATA_WINDOW:#x};", script)
    script = re.sub(r"\.\s*=\s*DATA_SEGMENT_RELRO_END\s*\([^;]*\);", "", script)
    script = re.sub(r"\.\s*=\s*DATA_SEGMENT_END\s*\(\s*\.\s*\);", "", script)
    out = obj_dir / "package.ld"
    write_if_changed(out, script)
    return out


PAGE = 0x1000


def layout(so: Path):
    data = so.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise SystemExit(f"component_as_lib: {so.name} is not a 32-bit little-endian ELF")
    phoff, shoff = struct.unpack_from("<II", data, 28)
    phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 42)
    data_start, data_end = None, 0
    for i in range(phnum):
        p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, _flags, _align = struct.unpack_from(
            "<IIIIIIII", data, phoff + i * phentsize
        )
        if p_type == 2:
            for j in range(p_offset, p_offset + p_filesz, 8):
                tag, value = struct.unpack_from("<iI", data, j)
                if tag == 22 or (tag == 30 and value & 4):
                    raise SystemExit(f"component_as_lib: {so.name} needs relocations in its code")
                if tag == 0:
                    break
        if p_type != 1:
            continue
        if p_vaddr >= DATA_WINDOW:
            data_start = p_vaddr if data_start is None else min(data_start, p_vaddr)
            data_end = max(data_end, p_vaddr + p_memsz)
        elif p_offset != p_vaddr or p_memsz != p_filesz:
            raise SystemExit(f"component_as_lib: {so.name} code segment at {p_vaddr:#x} is not its file image")
    if data_start is None:
        data_start = data_end = DATA_WINDOW
    internal = 0
    if shoff and shnum:
        names = struct.unpack_from("<IIIIIIIIII", data, shoff + shstrndx * shentsize)[4]
        for i in range(shnum):
            name, _type, _flags, _addr, _offset, size = struct.unpack_from("<IIIIII", data, shoff + i * shentsize)
            end = data.index(b"\0", names + name)
            if data[names + name : end].startswith((b".iram", b".dram")):
                internal += size
    flash = (max(len(data), data_end - DATA_WINDOW) + PAGE - 1) // PAGE * PAGE
    return flash, data_end - data_start, internal


def lib_dir(item, source_dir: Path):
    name = item["lib"].replace("/", "__")
    for root in (source_dir / "managed_components", source_dir / "components"):
        if (root / name).is_dir():
            return (root / name).resolve()
    raise SystemExit(f"component_as_lib: lib {item['lib']} not found under {source_dir}")


def compile_lib(item, source_dir: Path, binary_dir: Path, cc):
    root = lib_dir(item, source_dir)
    arch = idf_target(binary_dir)
    archives = [path for path in sorted(root.rglob("*.a")) if arch in path.parts]
    if archives:
        return None, None
    obj_dir = source_dir / "component_libs" / f"{item['package']}.obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objects = []
    cxx = None
    for index, entry in enumerate(cc):
        source = Path(entry.get("file", ""))
        if not source.is_absolute():
            source = Path(entry.get("directory", "")) / source
        try:
            source.resolve().relative_to(root)
        except ValueError:
            continue
        args = command(entry)
        if cxx is None and Path(args[0]).name.endswith("g++"):
            cxx = args[0]
        obj = obj_dir / f"{index:04d}_{source.stem}.o"
        cmd = [args[0], *strip_io(args[1:]), "-fPIC", "-c", str(source), "-o", str(obj)]
        result = subprocess.run(cmd, capture_output=True, text=True, cwd=entry.get("directory") or None)
        if result.returncode != 0:
            raise SystemExit(f"component_as_lib: compile {source} failed:\n{failure(obj_dir, source, result.stderr)}")
        objects.append(str(obj))
    if not objects:
        raise SystemExit(f"component_as_lib: lib {item['lib']} has no sources in compile_commands.json")
    if cxx is None:
        cxx = tool(command(donor(cc, source_dir, ".cpp"))[0], "g++")
    so = source_dir / "component_libs" / f"{item['package']}.so"
    script = window_script(cxx, obj_dir)
    link = [cxx, "-shared", "-fPIC", "-nostdlib", "-nostartfiles", f"-Wl,-T,{script}", "-o", str(so), *objects]
    result = subprocess.run(link, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"component_as_lib: link {so.name} failed:\n{failure(obj_dir, so, result.stderr)}")
    return so, cxx


def compile_package(item, source_dir: Path, cc):
    cpp_entry = donor(cc, source_dir, ".cpp")
    if cpp_entry is None:
        raise SystemExit("component_as_lib: no C++ compile command to take the flags from")
    cxx_args = command(cpp_entry)
    cxx = cxx_args[0]
    cxx_flags = ["-I" + item["include"], *strip_io(cxx_args[1:])]
    c_entry = donor(cc, source_dir, ".c")
    c_flags = ["-I" + item["include"], *strip_io(command(c_entry)[1:])] if c_entry else None
    gcc = tool(cxx, "gcc")
    sources = []
    source_root = Path(item.get("sources", source_dir / "src"))
    if "sources" in item:
        cxx_flags.insert(1, "-I" + str(source_root))
        if c_flags is not None:
            c_flags.insert(1, "-I" + str(source_root))
    for component in item["components"]:
        directory = source_root / "esphome" / "components" / component
        if directory.is_dir():
            sources += sorted(p for p in directory.rglob("*") if p.suffix in (".cpp", ".c"))
    sources.append(Path(item["glue"]))
    obj_dir = source_dir / "component_libs" / f"{item['package']}.obj"
    obj_dir.mkdir(parents=True, exist_ok=True)
    objects = []
    for index, source in enumerate(sources):
        obj = obj_dir / f"{index:03d}_{source.stem}.o"
        if source.suffix == ".c":
            if c_flags is None:
                raise SystemExit(f"component_as_lib: no C compile command for {source}")
            cmd = [gcc, *c_flags, "-fPIC", "-c", str(source), "-o", str(obj)]
        else:
            cmd = [cxx, *cxx_flags, "-fPIC", "-c", str(source), "-o", str(obj)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"component_as_lib: compile {source} failed:\n{failure(obj_dir, source, result.stderr)}")
        objects.append(str(obj))
    so = source_dir / "component_libs" / f"{item['package']}.so"
    script = window_script(cxx, obj_dir)
    link = [cxx, "-shared", "-fPIC", "-nostdlib", "-nostartfiles", "-Wl,--gc-sections", f"-Wl,-T,{script}",
            "-o", str(so), *objects]
    result = subprocess.run(link, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"component_as_lib: link {so.name} failed:\n{failure(obj_dir, so, result.stderr)}")
    return so, cxx


def tar_gz(members):
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        with tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
            for name, data in members:
                info = tarfile.TarInfo(name)
                info.size = len(data)
                info.mode = 0o644
                tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def ar(members):
    out = bytearray(b"!<arch>\n")
    for name, data in members:
        out += f"{name + '/':<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n".encode("ascii")
        out += data
        if len(data) % 2:
            out += b"\n"
    return bytes(out)


def idf_target(binary_dir: Path) -> str:
    sdkconfig = binary_dir / "config" / "sdkconfig.json"
    return json.loads(sdkconfig.read_text(encoding="utf-8"))["IDF_TARGET"]


def write_ipk(item, so: Path, needs, arch: str, out_dir: Path) -> Path:
    name = item["package"]
    package = name.replace("_", "-").lower()
    data = so.read_bytes()
    flash, psram, iram = layout(so)
    file_name = f"{name}.so"
    control = (
        f"Package: {package}\n"
        f"Version: {item['version']}\n"
        f"Architecture: {arch}\n"
        f"Installed-Size: {len(data)}\n"
        f"Description: ESPHome package {name}\n"
        f"ESPHome-ABI: {item['abi']}\n"
        f"ESPHome-Flash: {flash}\n"
        f"ESPHome-PSRAM: {psram}\n"
        f"ESPHome-IRAM: {iram}\n"
        + (f"Depends: {', '.join(item['depends'])}\n" if item.get("depends") else "")
    ).encode("ascii")
    sums = f"{hashlib.sha256(data).hexdigest()}  ./{file_name}\n".encode("ascii")
    control_tar = tar_gz(
        [
            ("./control", control),
            ("./sha256sums", sums),
            ("./needs", ("\n".join(needs) + "\n").encode("ascii")),
            ("./ids", ("\n".join(item["ids"]) + "\n").encode("ascii")),
        ]
    )
    data_tar = tar_gz([(f"./{file_name}", data)])
    ipk = out_dir / f"{package}_{item['version']}_{arch}.ipk"
    ipk.write_bytes(ar([("debian-binary", b"2.0\n"), ("control.tar.gz", control_tar), ("data.tar.gz", data_tar)]))
    return ipk


def in_base(libs: Path):
    path = libs / "libs_in_base.json"
    return set(json.loads(path.read_text(encoding="utf-8"))) if path.is_file() else set()


def prelink(source_dir: Path, binary_dir: Path, items):
    cc = load_compile_commands(binary_dir, source_dir)
    nm = tool(command(cc[0])[0], "nm")
    libs = source_dir / "component_libs"
    provided, thread_local, needed = set(), set(), set()
    kept_in_base = []
    for item in items:
        if "lib" in item:
            so, _cxx = compile_lib(item, source_dir, binary_dir, cc)
            if so is None:
                kept_in_base.append(item["package"])
                print(f"component_as_lib: lib {item['lib']} ships prebuilt archives that are not position independent: "
                      "kept in base until the installer relocates package code")
                continue
        else:
            so, _cxx = compile_package(item, source_dir, cc)
        needs = [name for name in undefined_symbols(nm, so) if IDENT.match(name)]
        write_if_changed(libs / f"{item['package']}.needs", "\n".join(needs) + "\n")
        provided |= set(global_symbols(nm, so))
        thread_local |= thread_local_symbols(nm, so)
        needed |= set(needs)
    write_if_changed(libs / "libs_in_base.json", json.dumps(kept_in_base))
    symbol_table(source_dir, sorted(needed - provided - thread_local))


def main():
    prelink_mode = sys.argv[1] == "prelink"
    args = sys.argv[2:] if prelink_mode else sys.argv[1:]
    source_dir = Path(args[0]).resolve()
    binary_dir = Path(args[1]).resolve()
    libs = source_dir / "component_libs"
    manifest = libs / "manifest.json"
    items = json.loads(manifest.read_text(encoding="utf-8")) if manifest.is_file() else []
    if prelink_mode:
        prelink(source_dir, binary_dir, items)
        return
    if not items:
        return
    out_dir = binary_dir / "packages"
    out_dir.mkdir(exist_ok=True)
    arch = idf_target(binary_dir)
    strip = tool(command(load_compile_commands(binary_dir, source_dir)[0])[0], "strip")
    base_libs = {name.replace("_", "-") for name in in_base(libs)}
    for item in items:
        if item["package"].replace("_", "-") in base_libs:
            continue
        item["depends"] = [dep for dep in item.get("depends", []) if dep not in base_libs]
        so = libs / f"{item['package']}.so"
        needs = (libs / f"{item['package']}.needs").read_text(encoding="utf-8").split()
        (out_dir / so.name).write_bytes(so.read_bytes())
        stripped = libs / f"{item['package']}.stripped.so"
        result = subprocess.run([strip, "--strip-debug", "-o", str(stripped), str(so)], capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"component_as_lib: strip {so.name} failed:\n{result.stderr[-2000:]}")
        ipk = write_ipk(item, stripped, needs, arch, out_dir)
        flash, psram, iram = layout(stripped)
        print(f"component_as_lib: package {item['package']} {item['version']} ({len(needs)} core symbols, "
              f"{len(item['ids'])} core ids, flash {flash}, PSRAM {psram}, IRAM {iram}) -> {ipk}")


if __name__ == "__main__":
    main()
