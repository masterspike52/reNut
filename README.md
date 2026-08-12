

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
OlieGamerTV for the information and documentations made for nuts and bolts specifically
<br>
.
<br>
.
<br>
.
<br>
.
<br>
.
<br>
.
<br>
you, the player?
<br>
.


Requirements
----------------------------------------------
the US version of Banjo-Kazooie: Nuts and Bolts (do not get the update, the recomp doesn't use it)

How to play on Linux
----------------------------------------------
1. Download Goopie Launcher from https://goopie.xyz/#/downloads
2. run goopie launcher
3. find banjo-kazooie: nuts and bolts in goopie launcher and click select game
4. select your iso for banjo-kazooie nuts and bolts, this will extract the contents to the default location for goopie (you can change it in goopie's settings)
5. click install (this should install the latest release which comes with the game, the dll's needed, and renut.toml)
6. click play when the game opens itll open a gui for setting your paths, you dont have to worry about it just click continue. it was placed there for me so when i deleted builds to rebuild i didnt have to run a command line.

or you can do the following.

- go to <a href=https://github.com/masterspike52/reNut/releases>releases</a> and download the appimage file from the latest release
- open the appimage file by double clicking it.

- when opening the appimage you will be met with a dialog box for dumping your iso's assets, selecting yes will let you select your iso for Banjo-Kazooie: Nuts and Bolts (US Version) with extract-iso.
- if you already have your iso's assets dumped you can click no and use the in game gui that appears to set your assets folder. the dialog will only show up once

How to play on Windows
--------------------------------------------
1. download <a href="https://github.com/etonedemid/NutStaller">NutStaller</a>
2. open NutStaller and select Do Everything which will do things like extract your assets and download the game to your selected directory

    Alternatives are
   - use goopie
   - download directly from releases
   

Where everything is in Linux
--------------------------------
- renut.toml and renut.cfg will be located in home/.config/renut/ they are written and read from here because appimages are weird about reading external stuff and renut.cfg will be specific to your locations for game_data_root and such
- saves are located wherever you choose to set the save location in the gui (by default its home/.local/shared/renut/)
- amd users currently cannot run the linux port (its being worked on right now) if you use a nvidia card you should be fine


copy and paste the following into renut.toml
------------------------------------------------------
- log_level = "off"
- fullscreen = false
- disable_shadows = true
- disable_cao = true
- disable_msaa = true
- disable_motion_blur = true
- target_refreshRate = 1
- frame_cap = "Display"
- sync_shader_compile = false
- mnk_controls = true
- gpu_plugin = "xenos"
- gpu_allow_invalid_fetch_constants = true
- native_2x_msaa = false
- readback_memexport = false
- readback_memexport_fast = false


NOTICE
--------------------
you will not have access to the texture replacement stuff due to rexglue-ostentation not working on linux at this time, you will however still have access to the extra menu for the cvars, it'll just show up as the unused town map icon.
