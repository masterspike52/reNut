<img width="1920" height="1080" alt="renut logo" src="https://github.com/user-attachments/assets/273bee28-755f-4494-920f-9333af72091e" />



Created using the <a href="https://github.com/rexglue/rexglue-sdk">Rexglue-SDK</a>


Discord
----------------------------------------------
https://discord.gg/D5Bz2ZsHdY - you can join the discord here


Issues
-------------------------------------------
* the games physics are funky above 60fps, please set your frame limit to 60 in F4 > Performance to avoid physics issues like banjo being flung through space. i will not be changing this because fucking with the physics engine is scary and it scared me to mess with it for 60.

About
--------------------------------------------
* Renut is a Recompilation of Banjo-Kazooie: Nuts and Bolts using the Rexglue-SDK

* the code has heavy ai useage, your opinion on ai useage means very little to me, if you have a problem with it you're free to fork the repo and redo it your own way (this is the purpose of open source projects)

* Renut does have a native renderer in which the games D3D functions are natively hooked to run through plume as well as compiles its recompiled shaders from xenosrecomp into its executables which makes the latest release different from previous releases which used the xenos emulation pipeline that rexglue provides for debugging



What's currently supported?
-----------------------------------------
currently the only devices supported are linux devices and windows devices, i have not gotten around to adding metal support for mac nor do i know if itll be worth it (if you use a mac like i said above, its open source, you're free to fork it)




 How to play
 ---------------------------------------------------
 * acquire your banjo-kazooie: nuts and bolts (usa) iso as well as the titleupdate

* download <a href="https://github.com/etonedemid/NutStaller/releases">nutstaller</a>

* open nutstaller, and click do everything and select your iso from the first step, the titleupdate in the second step

* click play

* you can also use dlc by taking the package for the dlc (E41F20AAF7094B9AFCA8CC90489D689D72A5062B4D) and place it in a folder named dlc next to the exe(appimage for linux)



Theres an android port
------------------------------------------------------
EtoneDemid has made an android port found in this fork https://github.com/etonedemid/reNut-android, the reason it isn't in main is because i myself couldn't get it to work well on lower end phones. if you wish to play the android port, it should run pretty well 



Online play
--------------------------------------
* online play now uses <a href="https://github.com/masterspike52/RexLiveless">rexliveless</a>, this allows for things like matchmaking and leaderboards as well as P2P lobby play

* rexliveless adds a blades-like menu by either pressing F10 or the guide/guide equivalent on your controller. the menu allows for things like custom gamer profile pictures and themes (made by trusted users, you are also free to suggest a theme you created)

* the menu is setup the same way it was setup on the xbox 360 which allows you to add your friends via username

* this is an example of a custom theme
<img width="1561" height="862" alt="image" src="https://github.com/user-attachments/assets/d3c2a9a7-304d-406f-a649-6dcd6d6916c4" />

* this is an example of a custom gamer profile pic

<img width="1630" height="1107" alt="image" src="https://github.com/user-attachments/assets/bbcdef35-0007-42e6-ba53-da89b478f862" />




things i havent tested for online
-------------------------------------
* haven't tested on linux yet, i implore users on linux to make sure they can sign-in/make an account on rexliveless and inform me or SolarCookies of any issues that may arise


Mods
--------------------------------------------
* mods made with https://github.com/weighta/NB-Studio-Banjo-Kazooie-Nuts-and-Bolts-World-Editor- made by XephyrCraft, they make .nbpatch files.

* to use the nbpatch files all you have to do is make a mods folder and place them in (its that simple)

* depending on the size of the mod the game could take a while to open (this only happens once) and not all mods work (this will be trial and error)

* we can now add custom parts following <a href="https://github.com/masterspike52/reNut/blob/main/docs/modding/custom-parts.md">custom-parts.md</a>



What's different
---------------------------------------------------
* aside from whats above, i had to scrap some things that weren't necessary, this includes the graphics options that were really in there for coping with the xenos emulation pipeline as well as mouse and keyboard support *(you can still use rexglues mnk which should work better than before)

* in return we now have a slew of new graphical options that work natively with the game like 4K resolution, widescreen, increased shadows, anisotropic filtering, and more. 



Warning
------------------------------------------------
if you have any issues please make an issue after ensuring you have covered things such as 

* updating your graphics drivers
* making sure the game is using your dedicated GPU and not your APU
* Making sure it happens more than once (by closing and reopening the game)

* if you have ensured the above the best way to go about it is to use <a href="https://renderdoc.org/">Renderdoc</a> and collect me render captures that i can then use to solve the issue

* if a crash happens please provide the crash dump and the log file associated with it 

Credits
------------------------------------------------
<a href="https://github.com/rexglue/rexglue-sdk">Rexglue team</a> for creating Rexglue-SDK
<br>
the <a href="https://discord.gg/ksm4vVbBf">Rexglue SDK discord</a> for helping with any info they have
<br>
SolarCookies for a handful of midasm hooks 
<br>
ValcomDrifty for the renut logo
<br>
OlieGamerTV for the information and documentations made for nuts and bolts specifically as well as a multitude of other things as well as assisting with other things about nuts and bolts
<br>
XephyrCraft for helping test mods made with his tool as well as testing the game
<br>
Leia and eli for testing renut (as well as anyone else who tested renut on windows)
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
