# DiskScope Windows portable archives

Build from a native Windows MSYS2 UCRT64 installation with `gcc`, `cmake`, `ninja`, `pkgconf`, `python`, `glib2`, `json-glib`, and `gtk4` for the GUI. Keep the installed versions' binary archives in the pacman cache. The packaging script matches runtime DLLs and data to that installation and records the cache archives' SHA-256 digests. It downloads matching source archives and checks each packaging recipe against the binary archive's build metadata.

From the repository root in PowerShell, with the UCRT64 binaries on `PATH` and `$Msys2Root` set to the MSYS2 installation directory, run these commands.

```powershell
$ProjectRoot = (Get-Location).Path.Replace('\', '/')
cmake --preset release -DDS_GUI=ON "-DCMAKE_C_FLAGS=-ffile-prefix-map=`"$ProjectRoot=.`""
cmake --build --preset release
cmake --install build/release --prefix build/windows-install
python packaging/windows/package.py --install build/windows-install --msys2 $Msys2Root `
    --output build/windows-dist --source-cache build/windows-source-cache --version 0.1.0
```

For a smaller CLI archive, configure with `-DDS_GUI=OFF`, install to a fresh prefix, and pass `--cli-only` to the packaging script. Omit build-directory artifacts when copying the distribution. The executable folder must stay alongside the `lib` and `share` folders from the ZIP.

The compiler path map keeps workstation-specific locations out of executable diagnostics. If the build uses a manually unpacked SQLite amalgamation outside the repository, map that directory too with a second `-ffile-prefix-map` compiler option.

Extract the binary ZIP and run `bin\diskscope.exe`. `bin\diskscope-cli.exe` and `bin\diskscope-worker.exe` are siblings. To run a metadata scan from the command line, choose a local fixture directory and a separate output path.

```powershell
.\bin\diskscope-cli.exe scan --root 'D:\disposable-fixture' --output 'D:\fixture.dscan'
```

The portable ZIP includes the native UCRT64 DLL import closure, the GTK image loaders used for icons, relocatable loader metadata, GSettings schemas, icon themes, MIME data, and the runtime libraries' packaged translations. Windows system libraries are provided by the operating system.

Keep the matching `-sources.zip` beside the binary archive when sharing the package. It contains the MSYS2 source tarballs corresponding to the packaged runtime and the SQLite amalgamation. The archive manifest records licenses, source URLs and checksums, and the `share/licenses` directory carries the packaged license texts. The dynamically linked libraries can be replaced with interface-compatible builds. [GNU Lesser General Public License 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html)
