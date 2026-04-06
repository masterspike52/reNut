

<img width="1920" height="1080" alt="renut logo" src="https://github.com/user-attachments/assets/273bee28-755f-4494-920f-9333af72091e" />




Created with <a href="https://github.com/rexglue/rexglue-sdk">Rexglue-SDK</a>


DISCORD
--------------------------------------------
we have a discord, please join and direct any questions there, me or someone will happily answer them

https://discord.gg/D5Bz2ZsHdY



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
etonedemid for the linux build/fixes
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


HOW TO BUILD
------------------------------------------------------
NOTE: YOU MUST DELETE ALL INSTANCES OF .gitignore OTHERWISE WHATEVER YOU COMPILE WITH WONT SEE THE FILES IN THE AREAS THEY ARE IN
<br>
NOTE: YOU MUST INSTALL <a href="https://git-scm.com/install/windows">GIT</a> BEFORE INSTALLING THE REXGLUE-SDK OR BUILDING THIS REPO
<br>
NOTE: YOU MUST INSTALL THE REXGLUE-SDK TO BUILD PLEASE FOLLOW THE <a href="https://github.com/rexglue/rexglue-sdk/wiki/Getting-Started">REXGLUE-SDK WIKI</a> BEFORE CONTINUING 

1. Clone the repository with ```git clone https://github.com/masterspike52/reNut.git```
2. inside the assets folder you need to extract your banjo-kazooie: Nuts&Bolts iso's contents and the default.xex. i reccomend using <a href="https://consolemods.org/wiki/images/5/5f/XBOX360_ISO_Extract.zip">iso extract</a>. (i dont know what linux users use for i use windows, however iso extract does work on linux through wine)
3. inside your cloned git open a terminal and run ```rexglue migrate --app_root .``` for this ensures if anything with codegen changes on rexglue you can codegen properly
   3b. you must then either delete your out folder or if your on windows you can open VS, right click your cmakelists.txt, and delete cache and reconfigure so you codegen with the version your using
4. you can now open a terminal and run ```rexglue codegen renut_config.toml``` which will give you the ppc files to recompile in the generated folder
5. if your on windows you can open the project in VS, change the build type to win-amd64-relwithdebinfo then build all
   5b. if your on linux you wont have access to VS so you will need to use a terminal and run ```cmake --preset linux-amd64-relwithdebinfo``` and then ```cmake --build --preset linux-amd64-relwithdebinfo``` (you can do this on windows as well, just replace linux with windows)
6. once its compiled you need to have the built exe in the same directory as the assets otherwise the game wont open

IF YOU DONT WANT TO BUILD
--------------------------------------------
building is mainly for those who either would rather build or want to help develope the game with me and the others. if you dont want to build you can download the latest release for your platform then in a folder make an assets folder, in that assets folder dumpe your iso's contents and the default.xex then in the root directory where you put those assets put the executable/application

KNOWN ISSUES
-----------------------------------------------
1. animations are a little jank (theres jitter, banjos and others bones break, some of the animations are half done and some other littel ansilaries) but they do not inhibit gameplay, its just kidna funny to see happen
2. nutty acres act 5 humbas mission causes a crash, this may require a rexglue update for it may have something to do with how rexglue handles certain opcodes and things like enableflushbuffer (it will be worked on, for now just dont do it, i do apologize to the completionists out there)

MAKING AN ISSUE
--------------------------
the issues tab is a place for things like crashes that happen in game that arent already noted, please refrain from making issues like "game wont open" or "have to use ISOs?" you must also use the crash template for any issues that pertain to crashes (ill probably make other templates) for i dont need it flooded with things that are mostly user error. please join the discord and use the #help channel if you have an issue unrelated to a crash 





