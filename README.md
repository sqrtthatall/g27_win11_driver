<div align="center">

# 🏎️ Logitech G27 // APEX Control

**A modern, ultra-lightweight tuning utility and real-time telemetry dashboard for the Logitech G27 Racing Wheel on Windows 11.**

[![Windows 11](https://img.shields.io/badge/Windows_11-Ready-0078D4?style=for-the-badge&logo=windows11&logoColor=white)](https://github.com/sqrtthatall/g27_win11_driver)
[![C++17](https://img.shields.io/badge/C%2B%2B17-Win32-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](https://github.com/sqrtthatall/g27_win11_driver)
[![Binary Size](https://img.shields.io/badge/Size-~300_KB-00E676?style=for-the-badge)](https://github.com/sqrtthatall/g27_win11_driver/releases)
[![License: Non-Commercial](https://img.shields.io/badge/License-Non--Commercial-red?style=for-the-badge)](LICENSE)

</div>

---

## 🌟 Overview

The legacy **Logitech Gaming Software / Logitech Profiler (v5.10)** is outdated, looks broken on modern high-DPI Windows 11 displays, and carries unnecessary background bloatware.

**APEX Control** is a clean, standalone replacement written in pure modern C++ and Win32 API. It communicates with the steering wheel directly via low-level USB HID reports without relying on bloatware. It runs as a completely portable, single `.exe` file with zero background performance penalty.

---

## ⚡ Key Features

* **Complete Wheel Tuning (Full Logitech Profiler Replacement)**:
  * **Degrees of Rotation**: Smooth interactive slider from 40° to 900° + instant quick-presets (`900° SIM`, `540° DRIFT`, `360° ARCADE`).
  * **Force Feedback Tuning**: Global toggle, **Overall Effects Strength**, **Spring Effect Strength**, and **Damper Effect Strength** sliders.
  * **Centering Spring**: Dedicated toggle and strength slider for games with or without native FFB.
  * **Pedals Mode**: Toggle **Combined Pedals** (single-axis mode for legacy arcade titles).
  * **Game Override**: Allow or restrict games from changing hardware settings on the fly.
* **Live Hardware Telemetry**:
  * Precision digital angle gauge displaying real-time steering rotation down to tenths of a degree.
  * Responsive travel meters for all three pedals (Clutch, Brake, Throttle) or combined axis.
  * **H-Shifter Detector**: Live gear display badge (`1`–`6`, `R`, `N`).
  * **RPM Shift LEDs**: Physical test sequence for wheel-mounted rev lights.
* **System Tray Integration**:
  * Minimizes quietly to the Windows Notification Area (System Tray) via titlebar minimize or dedicated button.
  * Double-click tray icon to restore; right-click context menu to quickly open or exit.
* **Windows 11 Dark Aesthetic**:
  * Custom dark dashboard UI inspired by modern sim-racing cockpits.
  * Double-buffered GDI rendering running smoothly at 60 FPS with zero screen tearing or input latency.
* **Zero Dependencies**:
  * Standalone binary (~300 KB). Does not require the Visual C++ Redistributable or third-party frameworks.

---

## 🚀 Download & Quick Start

1. Head over to the [**Releases**](https://github.com/sqrtthatall/g27_win11_driver/releases) section.
2. Download the latest `g27_driver_win11.exe`.
3. Plug in your Logitech G27 wheel via USB and launch the executable.

> **Note:** If your wheel inputs are exclusively captured by another process or anti-cheat service, run the application once via **"Run as Administrator"**.

---

## 🛠️ Building from Source

### Prerequisites
* **Visual Studio 2019 / 2022** with the **"Desktop development with C++"** workload.

### Option A: Command Line (MSVC)
Open the **x64 Native Tools Command Prompt for VS** and run:

```cmd
git clone https://github.com/sqrtthatall/g27_win11_driver.git
cd g27_win11_driver
cl.exe /O2 /MT /DUNICODE /D_UNICODE main.cpp /link /SUBSYSTEM:WINDOWS /OUT:g27_driver_win11.exe

### Option B: MinGW-w64 (GCC)

git clone https://github.com/sqrtthatall/g27_win11_driver.git
cd g27_win11_driver
g++ -O3 -municode -mwindows -static main.cpp -lsetupapi -lhid -lshell32 -lgdi32 -luser32 -ldwmapi -o g27_driver_win11.exe

(The /MT and -static flags embed the runtime library directly into the
executable, allowing it to run out-of-the-box on clean Windows 11 installs
without missing DLL errors).

## ☕ Support the Project (Donations)

If this tool made your sim-racing sessions on Windows 11 smoother, feel free to
buy the developer a coffee or support future updates!

### 🪙 Cryptocurrency (Crypto)

You can send donations directly to any of the wallet addresses below:

| Network / Asset              | Wallet Address                                   |
| :--------------------------- | :----------------------------------------------- |
| **USDT (TRC-20)**            | `TQ13pa4zqXeyc6ZXMXxVbc5DBZieTEnEf2`             |
| **Bitcoin (BTC)**            | `bc1qs0mj0gyrm8pl7tznvllxlah7rgjc74x23uhvu3`     |
| **Ethereum**                 | `0x0C333D9cDD282C883389Ae6Ef435f4A059EF7957`     |


## 📄 License

This project is licensed under a Custom Non-Commercial License.

  - ✅ Allowed: Free personal use, learning, compiling from source, and
    non-commercial community contributions.
  - ❌ Forbidden: Selling, monetizing, closed-source re-bundling, or any
    commercial exploitation without prior written consent from the author.

See the LICENSE file for the full legal text. For commercial inquiries, contact
the repository owner.

