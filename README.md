

<img width="1920" height="1080" alt="renut logo" src="https://github.com/user-attachments/assets/273bee28-755f-4494-920f-9333af72091e" />




Originally created with <a href="https://github.com/rexglue/rexglue-sdk">Rexglue-SDK</a>



DISCORD
--------------------------------------------
We have a discord, please join and direct any questions there. Myself or someone else will happily answer them.

https://discord.gg/D5Bz2ZsHdY



Credits
------------------------------------------------
<a href="https://github.com/rexglue/rexglue-sdk">Rexglue team</a> for creating Rexglue-SDK
<br>
the Rexglue SDK discord for helping with any info they have
<br>
SolarCookies for midasm hooks and future use of CRT functions and the reNut Launcher
<br>
ValcomDrifty for the renut logo
<br>
etonedemid for the linux build/fixes
<br>

<br>

<br>

<br>

<br>

<br>

<br>
you, the player?
<br>



REQUIREMENTS
-------------------------------------------------------------------
US version of banjo-kazooie: nuts and bolts default.xex


HOW TO BUILD
------------------------------------------------------
NOTE: YOU MUST DELETE ALL INSTANCES OF .gitignore OTHERWISE WHATEVER YOU COMPILE WITH WONT SEE THE FILES IN THE AREAS THEY ARE IN.
<br>
NOTE: YOU MUST INSTALL <a href="https://git-scm.com/install/windows">GIT</a> BEFORE INSTALLING THE REXGLUE-SDK, OR BUILDING THIS REPO.
<br>
NOTE: YOU MUST BUILD and INSTALL THIS FORK OF <a href="https://github.com/etonedemid/rexglue-sdk">REXGLUE-SDK</a> BEFORE CONTINUING.

1. Clone the repository with ```git clone https://github.com/masterspike52/reNut.git```
2. Inside the assets folder you need to extract your banjo-kazooie: Nuts&Bolts iso's contents and the default.xex. I reccomend using <a href="https://consolemods.org/wiki/images/5/5f/XBOX360_ISO_Extract.zip">iso extract</a>. (linux releases contain xiso-extract)
3. Inside your cloned git open a terminal and run ```rexglue migrate --app_root .``` This ensures that if anything with codegen changes on Rexglue, you can codegen properly.
   3b. You must then either delete your out folder, or if your on Windows you can open VS, right click your cmakelists.txt, and delete cache and reconfigure so you codegen with the version your using.
4. You can now open a terminal and run ```rexglue codegen renut_config.toml``` which will give you the ppc files to recompile in the generated folder.
5. If you're on Windows, you can open the project in VS, change the build type to `win-amd64-relwithdebinfo`, then build all.
   5b. If you're on Linux, and don't have access to VS, you will need to use a terminal and run ```cmake --preset linux-amd64-relwithdebinfo``` and then ```cmake --build --preset linux-amd64-relwithdebinfo```. (You can do this on Windows as well, just replace `linux` with `windows`.)
6. Once its compiled, you need to have the built exe in the same directory as the assets, otherwise the game won't open.


IF YOU DONT WANT TO BUILD
--------------------------------------------
Building is mainly for those who either would rather build or want to help develop the game with myself and others. If you don't want to build;
* Go to https://goopie.xyz/ and download the goopie launcher 
* In the launcher select banjo-kazooie: nuts and bolts
* Click Select ISO and select your iso for banjo-kazooie: nuts and bolts (must be the north american release) and wait for extraction to finish
* After extraction is finished click the update button, the launcher will then download the latest release of the windows version of renut
* Click play


If you are on linux
--------------------------------------

Prerequisites

- US version of **Banjo-Kazooie: Nuts & Bolts** `default.xex`
- Git


```curl -sSL https://raw.githubusercontent.com/masterspike52/reNut/main/build-linux.sh | bash```

or

1. Install dependencies

 Arch / Manjaro / SteamOS

```bash
sudo pacman -S --needed base-devel clang lld ninja cmake pkgconf python3 git \
    gtk3 pango harfbuzz fontconfig freetype2 cairo gdk-pixbuf2 glib2 \
    at-spi2-core libx11 libxcb libpipewire vulkan-headers vulkan-icd-loader \
    alsa-lib libpulse libusb libunwind dbus ibus systemd systemd-libs
```

 Ubuntu / Debian

```bash
sudo apt install build-essential clang lld ninja-build cmake pkg-config python3 git \
    libgtk-3-dev libpango1.0-dev libharfbuzz-dev libfontconfig-dev libfreetype-dev \
    libcairo2-dev libgdk-pixbuf-2.0-dev libglib2.0-dev libatspi2.0-dev \
    libx11-dev libxcb1-dev libpipewire-0.3-dev vulkan-headers libvulkan-dev \
    libasound2-dev libpulse-dev libusb-1.0-0-dev libunwind-dev libdbus-1-dev \
    libibus-1.0-dev libsystemd-dev libudev-dev
```

 Fedora

```bash
sudo dnf install @development-tools clang lld ninja-build cmake pkgconf python3 git \
    gtk3-devel pango-devel harfbuzz-devel fontconfig-devel freetype-devel \
    cairo-devel gdk-pixbuf2-devel glib2-devel at-spi2-core-devel \
    libX11-devel libxcb-devel pipewire-devel vulkan-headers vulkan-loader-devel \
    alsa-lib-devel pulseaudio-libs-devel libusb1-devel libunwind-devel dbus-devel \
    ibus-devel systemd-devel
```

> **Note:** CMake ≥ 3.25 is required. If your distro ships an older version, install a newer one from [cmake.org](https://cmake.org/download/) or via pip (`pip install cmake`).

 2. Clone the repositories

Clone reNut and the rexglue-sdk **side-by-side** (the build system auto-detects the SDK when it's in `../rexglue-sdk`):

```bash
git clone https://github.com/masterspike52/reNut.git
git clone https://github.com/etonedemid/rexglue-sdk.git
```

Your directory layout should look like:

```
parent/
├── reNut/
└── rexglue-sdk/
```

 3. Set up game assets

Extract your Banjo-Kazooie: Nuts & Bolts ISO contents and `default.xex` into the `reNut/assets/` folder.

 4. Build

```bash
cd reNut
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo -j$(nproc)
```

The binary will be at `out/build/linux-amd64-relwithdebinfo/renut`.

 Other build types

| Preset | Use case |
|---|---|
| `linux-amd64-debug` | Debug build |
| `linux-amd64-release` | Optimized release |
| `linux-amd64-relwithdebinfo` | Release with debug symbols (recommended) |

 5. Run

```bash
cd out/build/linux-amd64-relwithdebinfo
./launch.sh
```

Point it at your Banjo-Kazooie: Nuts & Bolts (US) ISO.



KNOWN ISSUES
-----------------------------------------------
1. Animations are a little jank (there's jitter, banjo's and others bones break, some of the animations are half done, and some other little ancillaries) but they do not inhibit gameplay, it's just funny to see happen.
2. The Jiggy Challenge "Old Dog, New Tricks" in Nutty Acres Act 5 will crash the game when starting it.
3. Water doesn't render (Atleast on linux)
4. The Jiggy Challenge Even Older Dog, Newer Tricks crashes similarly to 2.

MAKING AN ISSUE
--------------------------
The Issues tab is a place for things like crashes that happen in game that arent already noted, please refrain from making issues like "Game wont open" or "Have to use ISOs?".
You must use the crash template for any issues that pertain to crashes (I'll probably make other templates), for I don't need it flooded with mostly user error posts. Please join the discord and use the #help channel if you have an issue unrelated to a crash.
