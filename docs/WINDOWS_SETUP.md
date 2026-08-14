# Building on Windows

Companion to [`build_and_flash_windows.cmd`](../build_and_flash_windows.cmd), the Windows
equivalent of `build_and_flash.sh` / `build_and_flash_mac.sh`.

## Quick start

From **cmd.exe** or **PowerShell** in the repo root:

```bat
build_and_flash_windows.cmd
```

Defaults to `-p COM58 -e basic_connect -t esp32s3`, then builds, flashes and opens the
monitor (`Ctrl-]` to exit).

```bat
build_and_flash_windows.cmd -p COM12                  :: different port
build_and_flash_windows.cmd -e cellular_connect       :: different example
build_and_flash_windows.cmd --no-monitor              :: flash, don't attach monitor
build_and_flash_windows.cmd --clean --erase           :: fullclean + erase-flash first
build_and_flash_windows.cmd -h                        :: all options
```

Find your port with `mode` in cmd, or:

```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description
```

The ESP32-S3's built-in USB-JTAG shows up as `USB Serial Device`
(`VID_303A&PID_1001`) and needs no driver on Windows 11.

## What the script does differently from the shell versions

| `build_and_flash.sh` (Linux) | Windows equivalent |
|---|---|
| `sudo fuser -k /dev/ttyACM0` | kills `idf_monitor` / `esp_idf_monitor` processes |
| `sudo chmod 666 /dev/ttyACM0` | not needed — COM ports have no unix permissions |
| `pkill -f idf_monitor` | `taskkill` + a `Win32_Process` sweep |
| `eim run "idf.py ..."` | `call export.bat` once, then `idf.py` directly |
| symlink `components/wireguard_lwip` | directory **junction** (see below) |

It also fails fast: the COM port is verified *before* the slow ESP-IDF export, and it
lists the ports that do exist if yours is missing.

## The three Windows-only gotchas

### 1. `components/wireguard_lwip` is a git symlink

The real component lives at `components/microlink/components/wireguard_lwip`; the
top-level entry is a symlink, because ESP-IDF does not recurse into a component's own
`components/` subdirectory.

Git on Windows only materializes symlinks with `core.symlinks=true` **and** Developer
Mode (or admin) — otherwise you get a 35-byte text file containing the target path, and
CMake silently can't see the component. This repo has `core.symlinks=false`.

The script repairs it with a **directory junction**, which is equivalent for CMake's
purposes and, unlike a symlink, needs no elevation:

```bat
mklink /J components\wireguard_lwip components\microlink\components\wireguard_lwip
```

It then runs `git update-index --skip-worktree components/wireguard_lwip` so git stops
reporting the junction as a modified symlink. This is idempotent — reruns print
`[ok] components\wireguard_lwip resolves correctly`. **Do not `git add` that path** from
Windows; the skip-worktree bit keeps the Linux/Mac symlink intact upstream.

### 2. Line endings

There was no `.gitattributes` and `core.autocrlf=true`, so every file was checked out
CRLF — including `*.sh` (breaks bash with `\r: command not found`) and the four `*.s`
assembly files. Committing from Windows in that state would have rewritten all ~100
files and produced a whole-repo diff on Linux/Mac.

Now fixed by [`.gitattributes`](../.gitattributes): LF in the repo, LF forced for
`*.sh`/`*.s`, CRLF forced for `*.cmd`/`*.bat` (cmd.exe mis-parses `goto` labels in
LF-only batch files). Set `git config core.autocrlf input` locally to match.

### 3. This machine is Windows-on-ARM

It's a Snapdragon X Elite, and **the ESP-IDF Windows installer refuses to run on ARM64**
(`This program does not support the version of Windows your computer is running`).
ESP-IDF v5.3.1 ships no `win-arm64` toolchain — only `master`/6.x does.

So ESP-IDF is a plain git clone, using the **x86_64** toolchain under Windows' Prism
emulation. That works fine; a full clean build takes roughly 10-12 minutes instead of
the ~3-4 you'd get natively. Incremental builds are quick.

## Installing ESP-IDF from scratch

Already done on this machine — repeat only on a new one. Everything is on `D:` because
`C:` had under 10 GB free.

```bat
:: 1. clone ESP-IDF v5.3.1 (matches the version this project is built against)
mkdir D:\Espressif
cd /d D:\Espressif
git clone --depth 1 -b v5.3.1 --recursive --shallow-submodules --jobs 4 ^
    https://github.com/espressif/esp-idf.git esp-idf-v5.3.1

:: 2. install the toolchain for esp32s3 only
set "IDF_TOOLS_PATH=D:\Espressif\tools"
cd /d D:\Espressif\esp-idf-v5.3.1
install.bat esp32s3
```

Installed layout:

- `D:\Espressif\esp-idf-v5.3.1\` — the framework (`export.bat` lives here)
- `D:\Espressif\tools\` — toolchains, cmake, ninja, openocd, and the Python venv

The script auto-discovers `D:\Espressif\esp-idf-*\export.bat`. To use a different
location, set `IDF_PATH` (and `IDF_TOOLS_PATH`) before running it.

### Corporate TLS proxy

The network re-signs HTTPS. Git is fine because Git for Windows uses `schannel`
(the Windows certificate store), but Python/pip use their own bundled `certifi` and
fail with `CERTIFICATE_VERIFY_FAILED: self signed certificate in certificate chain`.

Fix — export the Windows roots to a PEM and point Python at it:

```powershell
Add-Type -AssemblyName System.Security
$sb = New-Object System.Text.StringBuilder
foreach ($loc in 'LocalMachine','CurrentUser') {
  foreach ($nm in 'Root','CA') {
    $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($nm, $loc)
    $s.Open('ReadOnly')
    foreach ($c in $s.Certificates) {
      [void]$sb.AppendLine('-----BEGIN CERTIFICATE-----')
      [void]$sb.AppendLine([Convert]::ToBase64String($c.RawData, 'InsertLineBreaks'))
      [void]$sb.AppendLine('-----END CERTIFICATE-----')
    }
    $s.Close()
  }
}
Set-Content D:\Espressif\corp-ca-bundle.pem $sb.ToString() -Encoding ascii
```

Note `Get-ChildItem Cert:\LocalMachine\Root` returns nothing here (the PSDrive provider
is blocked), which is why this uses the .NET `X509Store` API directly.

The script sets `SSL_CERT_FILE`, `REQUESTS_CA_BUNDLE` and `PIP_CERT` to that bundle if it
exists, so `idf.py` can fetch `managed_components`. Override the location with
`IDF_CA_BUNDLE`.

## Credentials

`sdkconfig` and `sdkconfig.credentials` are gitignored, so a fresh clone has none and the
firmware will boot but loop on `W (…) main: Wi-Fi disconnected, reconnecting...`:

```bat
cd examples\basic_connect
copy sdkconfig.credentials.example sdkconfig.credentials
:: then edit it, or:
idf.py menuconfig    :: -> MicroLink V2 -> Credentials
```

## Troubleshooting

**`ERROR: MSys/Mingw is not supported`** — you launched the script from Git Bash / MSYS2,
which exports `MSYSTEM`, and `idf_tools.py` hard-refuses when it sees that. The script
clears `MSYSTEM`/`MSYS`/`MINGW_PREFIX` itself, so this only bites if you call `idf.py`
by hand. Use cmd.exe or PowerShell.

**`COM58 not present`** — the script prints the ports that do exist; pass the right one
with `-p`. If none are listed, replug the board and check Device Manager.

**Port busy / `could not open port`** — something still holds it. The script kills known
monitor processes, but a stray PuTTY or Arduino IDE serial window needs closing manually.

**`ESP-IDF not found`** — set `IDF_PATH` to the directory containing `export.bat`.

**Build fails right after a branch switch** — `build_and_flash_windows.cmd --clean`.
