# UCC Compiler — Windows Setup Guide

Everything here uses free tools. Pick **one** route. Total time: ~15 minutes.

The zip contains the full C11 source (`src/`), the web IDE (`web/`),
the architecture doc (`ARCHITECTURE.md`), a `Makefile`, and a `Dockerfile`.
There is no prebuilt `.exe` inside — you build it once on your machine
(it takes a few seconds).

---

## Route 1 — MSYS2 + MinGW (recommended, native speed)

1. Download MSYS2 from **https://www.msys2.org** and install it
   (accept the default `C:\msys64`).
2. From the Start menu open **MSYS2 UCRT64**
   (important: UCRT64, not plain "MSYS", not CMD, not PowerShell).
3. Update the package database:
   `pacman -Syu`
   If it tells you to close the terminal, close it, reopen **MSYS2 UCRT64**,
   and run `pacman -Syu` again.
4. Install the C toolchain:
   `pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain`
   (press Enter to confirm the install).
5. Extract `ucc-1.0.0.zip` to `C:\ucc`
   (right-click the zip → Extract All).
6. In the UCRT64 terminal:
   ```
   cd /c/ucc
   make
   ```
   You should see gcc compile every file with no warnings, producing
   `build/ucc.exe`.
7. Test it. Save this as `C:\ucc\hello.c`:
   ```c
   #include <stdio.h>
   int main() {
       int a = 3, b = 7;
       swap(a, b);
       printf("%d %d\n", a, b);
       return 0;
   }
   ```
   Run: `./build/ucc.exe hello.c`
   Expected output: `7 3` (swap worked), exit code 0.

**Using the compiler:** `./build/ucc.exe program.c`
- Exit code 0 = success (the exit code is `main`'s return value).
- Exit code 1 = compile error, shown as `file:line:col: error: message`.
- Exit code 2 = runtime error (e.g. division by zero, step limit hit).
- Useful flags: `--help`, `--dump-tokens`, `--dump-ast`,
  `--dump-bytecode`, `--json-diagnostics`, `--max-steps N`.

---

## Route 2 — Web IDE (the OnlineGDB-style editor in your browser)

1. Finish Route 1 steps 1–6 first (the IDE needs the compiled `ucc.exe`).
2. Install Node.js — either in the UCRT64 terminal:
   `pacman -S mingw-w64-ucrt-x86_64-nodejs`
   or the LTS installer from **https://nodejs.org**.
3. In the terminal:
   ```
   cd /c/ucc/web
   npm install
   npm start
   ```
4. Open **http://localhost:3000** in your browser.
   Write C code, press Run, and type into the terminal panel at the
   bottom whenever `scanf` waits for input.
5. If the IDE reports it can't find the compiler, stop it (Ctrl+C) and
   restart with the binary path set explicitly:
   - In UCRT64: `UCC_BIN=/c/ucc/build/ucc.exe npm start`
   - In PowerShell: `$env:UCC_BIN="C:\ucc\build\ucc.exe"; npm start`

---

## Route 3 — Docker (no toolchain to install at all)

1. Install **Docker Desktop for Windows** (free) from
   **https://www.docker.com/products/docker-desktop**.
2. Extract the zip, open a terminal in that folder, and run:
   ```
   docker build -t ucc-ide .
   docker run -p 3000:3000 ucc-ide
   ```
3. Open **http://localhost:3000**. The container builds the compiler
   and serves the IDE by itself.

---

## Route 4 — WSL2 (only if you already use it)

1. In an admin PowerShell: `wsl --install`, reboot, open Ubuntu.
2. `sudo apt update && sudo apt install -y gcc make nodejs npm unzip`
3. Unzip the archive, `cd` into it, run `make`.
4. For the IDE: `cd web && npm install && npm start`,
   then open http://localhost:3000 in Windows.

---

## Troubleshooting

- `'make' is not recognized` → you are in CMD/PowerShell, not the
  **MSYS2 UCRT64** terminal. Building must happen inside MSYS2.
- `gcc: command not found` → step 4 of Route 1 didn't finish; rerun it.
- Windows antivirus quarantines `ucc.exe` → it is your own local build;
  add `C:\ucc` as an exclusion in Windows Security.
- `npm start` says the compiler is missing → use the `UCC_BIN` step
  in Route 2, step 5.
- `scanf` in the IDE seems stuck → click the terminal panel at the
  bottom, type your input, press Enter.
- Programs read from files, not pasted stdin:
  `./build/ucc.exe program.c` (there is no REPL/stdin mode).
