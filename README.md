

<img width="1920" height="1080" alt="renut logo" src="https://github.com/user-attachments/assets/273bee28-755f-4494-920f-9333af72091e" />




Created with <a href="https://github.com/rexglue/rexglue-sdk">Rexglue-SDK</a>





Credits
------------------------------------------------
Rexglue team for creating Rexglue-SDK
<br>
the Rexglue SDK discord for helping with any info they have
<br>
SolarCookies for midasm hooks and future use of CRT functions and the reNut Launcher
<br>
ValcomDrifty for the renut logo
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


REQUIREMENTS
-------------------------------------------------------------------
US version of banjo-kazooie: nuts and bolts default.xex


How to Use
-------------------------------------------
1. Download the reNut Launcher from <a href="https://github.com/masterspike52/reNut/releases/">Releases</a>
2. click file > set game folder to pick your prefered install location (if you dont the default will be in the root of the launcher folder)
3. open the launcher and select "select game iso" and select your banjo kazooie nuts and bolts (US) iso
4. when its done dumping your iso select download
5. after its done downloading select launch game

   HUGE CREDITS TO SOLAR COOKIES FOR MAKING THE LAUNCHER, HE PUT A LOT OF HARD WORK INTO IT, HE HAS ALSO APPROVED OF USING THE CODE TO MAKE YOUR OWN, YOU CAN FIND IT <a href="https://github.com/SolarCookies/reNut-Launcher">HERE</a> 



HOW TO BUILD
-----------------------------------------------------------------------------------
1. Install Rexglue-SDk following the <a href="https://github.com/rexglue/rexglue-sdk/wiki/Guide-%E2%80%90-Getting-Started">wiki</a>
2. install Visual Studio Community edition and ensure you install the desktop development with C++ and make sure you check the box that says C++ clang compiler for windows (note: if you are using Mac or linux you can skip this for you will have to follow the wiki linked in step 1 to build the game)
3. clone/download the repository
4. dump your copy of Banjo-Kazooie: Nuts&bolts and use a tool like ISO-Extract to dump the contents of the iso (INCLUDING THE DEFAULT.XEX FILE)
5. place the contents of the iso in the assets folder(INCLUDING THE DEFAULT.XEX FILE)
6. open the folder in visual studio, go into cmake targets view
7. change the configuration to win-amd64-relwithdebinfo
8. put rexglue.exe in your path environment variable and do ```rexglue codegen renut_config.toml``` in a terminal (visual studios works, or you can use windows default terminal/cmd/powershell)
10. right click reNut project and select build all
11. copy the assets folder with the dumped contents of the iso in out/build/win-amd64-relwithdebinfo




PREFFERED METHOD OF RUNNING THE GAME
-----------------------------------------------------------
open a terminal and do ```.\renut --gpu_allow_invalid_fetch_constants=true``` to prevent some hangs in game

if your using something like cmd or similar you only need to run ```renut --gpu_allow_invalid_fetch_constants=true``` 


CURRENT ISSUE!!!!
------------------------------------------------------------------------------
BANJOS WRISTS LIKE TO SHATTER AMONGST OTHER CHARACTERS, IT IS NOT A HORRENDOUS THING JUST FUNNILY NOTICEABLE BUT DOES NOT INHIBIT GAMEPLAY
SOME ANIMATIONS ARE UNFINISHED (I.E. SOME ATTACKS DONT GO FULL CIRCLE BUT ARE STILL ACTIVE THE SAME WAY, AGAIN DOESNT INHIBIT GAMEPLAY, JUST KINDA FUNNY)
SOMETIMES SAVING OVER A CART AND RENAMING IT CAUSES A CRASH, THERE IS NO FIX FOR THIS I CAN DO AT THIS TIME, MAY REQUIRE REXGLUE UPDATES.



MAKING ISSUES
--------------------------------------------------------------------------
Remember that this game and rexglue are in an experimental state, there are some issues i myself cannot fix and there may even be some that rexglue cant fix
if you make an issue you will need to be a bit specific 



