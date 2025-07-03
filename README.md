# Nocturne Firmware by Lucid Studios

Welcome to **Nocturne**, a custom Flipper Zero firmware built from the ground up by **Lucid Studios** — optimized for performance, packed with powerful tools, and themed for stealth and clarity.

---

## 🚀 Features

- 🛠️ Fully modular and categorized plugin system
  - **Tools**, **Wireless**, **Fun**, **Dev** folders
- 📡 Enhanced RogueMaster features (Sub-GHz, NFC, RF, fuzzers)
- 🎮 50+ curated plugins and games
- 🎨 Lucid Studios boot animation and branding
- 📦 Ready to build with Flipper’s official toolchain (`fbt`)

---

## 🧱 Project Structure

```bash
applications/        # Core firmware apps
applications_user/   # Placeholder for user-built apps
assets/              # Splash screen, icons, colors
firmware/            # Core firmware logic
furi/                # System primitives & RTOS
lib/                 # Libraries (Sub-GHz, NFC, etc.)
scripts/             # Developer tools and automation
targets/             # Device/platform config
site_scons/          # Build system scripts
firmware.scons       # Top-level build config
fbt                  # Flipper Build Tool
```

---

## ⚙️ Build Instructions

> Make sure you have `git`, `python3`, `cmake`, `ninja`, and `gcc-arm-none-eabi`.

### Clone the Repo

```bash
git clone --recursive https://github.com/YOUR-USERNAME/Nocturne-Firmware.git
cd Nocturne-Firmware
```

### Build the Firmware

```bash
./fbt
```

### Flash via USB

```bash
./fbt flash_usb
```

---

## 🧠 Philosophy

**Lucid Studios** believes in crafting firmware that’s more than just functional — it should feel **intentional, powerful, and smooth**. Nocturne is a **sleeper-class build** that combines Momentum’s speed with RogueMaster’s raw capabilities, wrapped in minimal design.

---

## 🤝 Contributing

Please see [`CONTRIBUTING.md`](./CONTRIBUTING.md)  
We accept PRs for plugin enhancements, bug fixes, script modules, and more!

---

## 📜 License

This is a **proprietary firmware** licensed by **Lucid Studios**.  
Usage is allowed only with permission from the project owner.  
See [`LICENSE`](./LICENSE) for more information.

---

## 🔗 Contact

- ✉️ Email: [support@lucidstudios.com](mailto:support@lucidstudios.com)
- 🛠️ Discord: coming soon
