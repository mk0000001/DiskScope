#!/usr/bin/env python3
"""Build Windows x64 portable archives from a CMake install and an MSYS2 UCRT64 prefix."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile


UCRT_PACKAGE = "mingw-w64-ucrt-x86_64-"
MSYS2_MIRROR = "https://mirror.msys2.org/mingw"
TOOLS = ("gcc", "binutils", "headers", "crt", "cmake", "ninja", "pkgconf", "python")
EXES = ("diskscope-cli.exe", "diskscope-worker.exe")
SPAWN_HELPERS = ("bin/gspawn-win64-helper.exe", "bin/gspawn-win64-helper-console.exe")
SQLITE_VERSION = "3.54.1"
SQLITE_SHA256 = "0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9"
SQLITE_ARCHIVE = "sqlite-amalgamation-3540100.zip"
SQLITE_URL = f"https://www.sqlite.org/2026/{SQLITE_ARCHIVE}"


def command(executable, *args):
    result = subprocess.run([executable, *args], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise ValueError(f"{Path(executable).name} failed with exit code {result.returncode}")
    return result.stdout


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def check_build_paths(binary, roots):
    data = binary.read_bytes().lower()
    for root in roots:
        absolute = root.resolve()
        if len(absolute.parts) < 2:
            continue
        windows = str(absolute)
        for form in (windows, windows.replace("\\", "/")):
            for encoding in ("utf-8", "utf-16le"):
                if form.encode(encoding).lower() in data:
                    raise ValueError(f"Compile {binary.name} with -ffile-prefix-map to remove local build paths")


def fields(path):
    values = {}
    for field in path.read_text(encoding="utf-8").strip().split("\n\n"):
        if not field:
            continue
        name, *data = field.splitlines()
        values[name.strip("%")] = data
    return values


class Runtime:
    def __init__(self, msys, stage):
        self.msys = msys
        self.prefix = msys / "ucrt64"
        self.stage = stage
        self.packages = {}
        self.owners = {}
        self.payload = {}
        self.dependencies = set()
        self.system_dlls = set()
        self.objdump = self.prefix / "bin/objdump.exe"
        self.tar = msys / "usr/bin/bsdtar.exe"

        for directory in sorted((msys / "var/lib/pacman/local").glob(f"{UCRT_PACKAGE}*")):
            data = fields(directory / "desc")
            name = data["NAME"][0]
            self.packages[name] = data
            for member in fields(directory / "files").get("FILES", []):
                if member.startswith("ucrt64/") and not member.endswith("/"):
                    self.owners[member.removeprefix("ucrt64/")] = name
        self.runtime_dlls = {member.lower(): member for member in self.owners
                             if member.startswith("bin/") and member.lower().endswith(".dll")}

    def copy_runtime(self, relative):
        if relative in self.payload:
            return
        owner = self.owners.get(relative)
        if not owner:
            raise ValueError(f"No UCRT64 package owns {relative}")
        source = self.prefix / relative
        destination = self.stage / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        self.dependencies.add(owner)
        self.payload[relative] = owner

    def imports(self, relative):
        binary = self.stage / relative
        pe = command(self.objdump, "-p", binary).decode("utf-8", errors="replace")
        if "file format pei-x86-64" not in pe:
            raise ValueError(f"Unexpected PE machine type in {relative}")
        return set(re.findall(r"DLL Name:\s+([^\s]+)", pe, flags=re.IGNORECASE))

    def collect_imports(self, seeds):
        pending = list(seeds)
        system = Path(os.environ["SystemRoot"]) / "System32"
        while pending:
            for dll in sorted(self.imports(pending.pop()), key=str.lower):
                if not re.fullmatch(r"[A-Za-z0-9_.+-]+\.(?:dll|drv)", dll, flags=re.IGNORECASE):
                    raise ValueError("Invalid DLL import name")
                relative = self.runtime_dlls.get(f"bin/{dll.lower()}")
                if relative in self.payload:
                    continue
                if relative:
                    self.copy_runtime(relative)
                    pending.append(relative)
                elif dll.lower().startswith(("api-ms-win-", "ext-ms-win-")) or (system / dll).is_file():
                    self.system_dlls.add(dll.lower())
                else:
                    raise ValueError(f"Unresolved Windows runtime DLL {dll}")

    def gtk_data(self):
        loaders = []
        supported_loaders = {"pixbufloader_svg.dll", "libpixbufloader-png.dll",
                             "libpixbufloader-ico.dll", "libpixbufloader-bmp.dll"}
        for member in sorted(self.owners):
            path = Path(member)
            if member.startswith("lib/gdk-pixbuf-2.0/") and path.name in supported_loaders:
                loaders.append(member)
                self.copy_runtime(member)
            elif (member.startswith(("share/icons/Adwaita/", "share/icons/AdwaitaLegacy/",
                                     "share/icons/hicolor/", "share/mime/")) or
                  (member.startswith("share/glib-2.0/schemas/") and member.endswith(".gschema.xml")
                   and "Demo4" not in member)):
                self.copy_runtime(member)

        if {Path(loader).name for loader in loaders} != supported_loaders:
            raise ValueError("The installed GTK runtime does not provide the required image loaders")
        schemas = self.stage / "share/glib-2.0/schemas"
        command(self.prefix / "bin/glib-compile-schemas.exe", "--strict", schemas)
        self.payload["share/glib-2.0/schemas/gschemas.compiled"] = "generated"

        cache = command(self.prefix / "bin/gdk-pixbuf-query-loaders.exe",
                        *(self.prefix / loader for loader in loaders)).decode("utf-8")
        cache = "\n".join(line for line in cache.splitlines() if not line.startswith("# LoaderDir")) + "\n"
        signatures = "\n".join(line for line in cache.splitlines() if not line.startswith("#")).strip()
        paths = {block.splitlines()[0].strip('"').replace("\\\\", "/")
                 for block in signatures.split("\n\n") if block}
        if paths != set(loaders):
            raise ValueError("Image-loader cache does not contain portable module paths")
        cache_name = (Path(loaders[0]).parent.parent / "loaders.cache").as_posix()
        (self.stage / cache_name).write_text(cache, encoding="utf-8", newline="\n")
        self.payload[cache_name] = "generated"
        return loaders

    def locale_and_notices(self):
        for member, owner in sorted(self.owners.items()):
            if owner not in self.dependencies:
                continue
            if (member.startswith("share/licenses/") or
                (member.startswith("share/doc/") and is_notice(Path(member).name))):
                self.copy_runtime(member)
            elif member.startswith("share/locale/") and member.endswith(".mo"):
                self.copy_runtime(member)

    def package_record(self, name):
        info = self.packages[name]
        version = info["VERSION"][0]
        file_version = version.replace(":", "~")
        binary = f"{name}-{file_version}-{info['ARCH'][0]}.pkg.tar.zst"
        archive = self.msys / "var/cache/pacman/pkg" / binary
        if not archive.is_file():
            raise ValueError(f"Keep the matching pacman archive in the cache: {binary}")
        buildinfo = command(self.tar, "-xOf", archive, ".BUILDINFO").decode("utf-8")
        recipe = re.search(r"^pkgbuild_sha256sum = ([0-9a-f]{64})$", buildinfo, flags=re.MULTILINE)
        if not recipe:
            raise ValueError(f"Missing recipe checksum for {name}")
        base = info["BASE"][0]
        return {
            "package": name,
            "version": version,
            "licenses": info.get("LICENSE", []),
            "homepage": info["URL"][0],
            "binary_archive": binary,
            "binary_sha256": digest(archive),
            "binary_url": f"{MSYS2_MIRROR}/ucrt64/{binary}",
            "source_base": base,
            "source_archive": f"{base}-{file_version}.src.tar.zst",
            "recipe_sha256": recipe.group(1),
        }


def is_notice(name):
    return re.match(r"^(?:authors|copying|licen[cs]es?|copyright|notice)(?:[._-]|$)",
                    name, flags=re.IGNORECASE) is not None


def source_notices(runtime, record, src_path):
    owner = record["package"]
    if any(package == owner and member.startswith(("share/licenses/", "share/doc/"))
           for member, package in runtime.payload.items()):
        return

    base = record["source_base"]
    upstream_name = base.removeprefix("mingw-w64-")
    archived = command(runtime.tar, "-tf", src_path).decode("utf-8").splitlines()
    upstream = [member for member in archived if member.startswith(f"{base}/{upstream_name}-") and
                member.endswith((".tar.xz", ".tar.gz", ".tar.bz2"))]
    if len(upstream) != 1:
        raise ValueError(f"No upstream license archive could be identified for {owner}")
    data = command(runtime.tar, "-xOf", src_path, upstream[0])
    found_license = False
    with tarfile.open(fileobj=io.BytesIO(data)) as source:
        for member in source:
            relative = Path(member.name)
            if len(relative.parts) != 2 or not is_notice(relative.name) or not member.isfile():
                continue
            name = f"share/licenses/{owner}/{relative.name}"
            destination = runtime.stage / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            with source.extractfile(member) as text, destination.open("wb") as notice:
                shutil.copyfileobj(text, notice)
            runtime.payload[name] = owner
            found_license |= not relative.name.upper().startswith("AUTHORS")
    if not found_license:
        raise ValueError(f"No top-level license text found in upstream source for {owner}")


def download(filename, url, cache):
    target = cache / filename
    if not target.is_file():
        with tempfile.NamedTemporaryFile(dir=cache, suffix=".part", delete=False) as partial:
            temporary = Path(partial.name)
        curl = Path(os.environ["SystemRoot"]) / "System32/curl.exe"
        try:
            command(curl, "--silent", "--fail", "--location", "--proto", "=https",
                    "--retry", "2", "--max-time", "180", "--output", temporary, url)
            os.replace(temporary, target)
        except ValueError as failure:
            raise ValueError(f"Could not fetch {filename}") from failure
        finally:
            temporary.unlink(missing_ok=True)
    return target


def get_package_source(record, cache, tar):
    archive = record["source_archive"]
    url = f"{MSYS2_MIRROR}/sources/{archive}"
    path = download(archive, url, cache)
    recipe = command(tar, "-xOf", path, f"{record['source_base']}/PKGBUILD")
    if hashlib.sha256(recipe).hexdigest() != record["recipe_sha256"]:
        raise ValueError(f"Source recipe checksum differs from installed binary's recipe: {archive}")
    return path, {"source_archive": archive, "source_sha256": digest(path), "source_url": url}


def make_zip(destination, members, stored=False):
    with tempfile.NamedTemporaryFile(dir=destination.parent, suffix=".part", delete=False) as partial:
        temporary = Path(partial.name)
    try:
        with zipfile.ZipFile(temporary, "w") as output:
            for relative, path in sorted(members.items()):
                entry = zipfile.ZipInfo(relative, (1980, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED
                entry.file_size = path.stat().st_size
                with path.open("rb") as source, output.open(entry, "w") as member:
                    shutil.copyfileobj(source, member)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def package(args):
    if sys.platform != "win32" or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9]+)*",
                                                   args.version):
        raise ValueError("Build with native Windows Python and a valid version")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cache = args.source_cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    os.environ["PATH"] = f"{args.msys2.resolve() / 'ucrt64/bin'};{os.environ['SystemRoot']}\\System32"

    repo = Path(__file__).resolve().parents[2]
    profile = "cli" if args.cli_only else "portable"
    name = f"DiskScope-{args.version}-windows-x64-{profile}"
    with tempfile.TemporaryDirectory(prefix="diskscope-package-") as temp:
        stage = Path(temp)
        runtime = Runtime(args.msys2.resolve(), stage)
        executables = [f"bin/{exe}" for exe in EXES]
        if not args.cli_only:
            executables.append("bin/diskscope.exe")
        (stage / "bin").mkdir()
        for executable in executables:
            if not (args.install / executable).is_file():
                raise ValueError(f"CMake install is missing {executable}")
            shutil.copyfile(args.install / executable, stage / executable)
            command(runtime.prefix / "bin/strip.exe", "--strip-debug", stage / executable)
            check_build_paths(stage / executable, (repo, args.install, args.msys2, Path.home()))
            runtime.payload[executable] = "DiskScope"

        for helper in SPAWN_HELPERS:
            runtime.copy_runtime(helper)
        seeds = executables + list(SPAWN_HELPERS) + ([] if args.cli_only else runtime.gtk_data())
        runtime.collect_imports(seeds)
        runtime.locale_and_notices()
        shutil.copyfile(repo / "LICENSE", stage / "LICENSE")
        runtime.payload["LICENSE"] = "DiskScope"

        toolchain = [runtime.package_record(f"{UCRT_PACKAGE}{tool}") for tool in TOOLS]
        packages = [runtime.package_record(owner) for owner in sorted(runtime.dependencies)]
        by_source = {}
        for record in packages:
            previous = by_source.setdefault(record["source_archive"], record)
            if previous["recipe_sha256"] != record["recipe_sha256"]:
                raise ValueError(f"Inconsistent source recipe for {record['source_archive']}")
        source_files = {}
        source_results = {}
        with ThreadPoolExecutor(max_workers=4) as pool:
            for path, metadata in pool.map(lambda record: get_package_source(record, cache, runtime.tar),
                                           by_source.values()):
                source_files[f"msys2/{path.name}"] = path
                source_results[path.name] = metadata
        for record in packages:
            source_notices(runtime, record, source_files[f"msys2/{record['source_archive']}"])
            record.update(source_results[record["source_archive"]])
            record.pop("source_base")

        sqlite = download(SQLITE_ARCHIVE, SQLITE_URL, cache)
        if digest(sqlite) != SQLITE_SHA256:
            raise ValueError("SQLite amalgamation archive has the wrong SHA-256")
        source_files[f"sqlite/{SQLITE_ARCHIVE}"] = sqlite
        sources_name = f"{name}-sources.zip"
        manifest = {
            "project": "DiskScope", "version": args.version, "target": "Windows x64 UCRT64",
            "profile": profile, "packages": packages, "toolchain": toolchain,
            "sqlite": {"version": SQLITE_VERSION, "license": "public domain", "source_url": SQLITE_URL,
                       "source_archive": SQLITE_ARCHIVE, "source_sha256": SQLITE_SHA256},
            "system_dll_imports": sorted(runtime.system_dlls), "source_companion": sources_name,
            "files": [{"path": file, "sha256": digest(stage / file), "owner": owner}
                      for file, owner in sorted(runtime.payload.items())],
        }
        (stage / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                                                     encoding="utf-8", newline="\n")
        freetype_notice = ("This software is based in part on the work of the FreeType Team.\n"
                           if f"{UCRT_PACKAGE}freetype" in runtime.dependencies else "")
        (stage / "THIRD_PARTY_NOTICES.txt").write_text(
            "DiskScope uses the separately replaceable UCRT64 runtime DLLs in bin/ and lib/.\n"
            "The runtime manifest identifies their versions, licenses, binary archive hashes,\n"
            f"and corresponding source archives in {sources_name}.\n"
            "Keep the binary and source archives together when redistributing this bundle.\n"
            "The shared libraries can be replaced by interface-compatible builds.\n"
            "See share/licenses/ for notices supplied by the originating packages and sources.\n" +
            freetype_notice + "SQLite is compiled into DiskScope. Its amalgamation is in the public domain.\n\n" +
            "\n".join(f"{r['package']} {r['version']}: {', '.join(r['licenses'])}" for r in packages) + "\n",
            encoding="utf-8", newline="\n")
        source_files["runtime-manifest.json"] = stage / "runtime-manifest.json"
        source_files["THIRD_PARTY_NOTICES.txt"] = stage / "THIRD_PARTY_NOTICES.txt"
        make_zip(output / sources_name, source_files, stored=True)
        binary_members = {file: stage / file for file in runtime.payload}
        binary_members["runtime-manifest.json"] = stage / "runtime-manifest.json"
        binary_members["THIRD_PARTY_NOTICES.txt"] = stage / "THIRD_PARTY_NOTICES.txt"
        make_zip(output / f"{name}.zip", binary_members)
    print(f"Created {name}.zip and {sources_name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", type=Path, required=True, help="CMake install prefix")
    parser.add_argument("--msys2", type=Path, required=True, help="MSYS2 root with the UCRT64 packages")
    parser.add_argument("--output", type=Path, required=True, help="ZIP output directory")
    parser.add_argument("--source-cache", type=Path, required=True, help="Download cache for source archives")
    parser.add_argument("--version", required=True, help="DiskScope version")
    parser.add_argument("--cli-only", action="store_true", help="Package only the CLI and worker")
    try:
        package(parser.parse_args())
    except (OSError, ValueError, KeyError) as failure:
        detail = str(failure) if isinstance(failure, ValueError) else "check the MSYS2 and CMake install directories"
        print(f"Packaging failed ({type(failure).__name__}): {detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
