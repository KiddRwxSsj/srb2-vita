# Sonic Robo Blast 2 for PS Vita

This is a port of [Sonic Robo Blast 2](https://www.srb2.org/) v2.2.15 to the PlayStation Vita. It uses SRB2's own OpenGL renderer running on top of [vitaGL](https://github.com/Rinnegatamante/vitaGL). Menus, levels, saves, music and sound all work on real hardware, and most of the effort went into getting it to run at a playable frame rate.

## Features and mods

The full single player game is playable, and so are addons. Put `.pk3`/`.wad` files in `ux0:data/srb2/addons` and load them from the Addons menu as usual. I tested the Sonic Adventure and Sonic Heroes character mods and both worked great.

Multiplayer is built in (netplay needs curl in 2.2.15, so it is compiled with it), but I never tested it, so consider it unsupported for now. Rumble and the on-screen keyboard are not implemented, which means you can't type names or console commands with the system keyboard.

Recommended settings are the defaults: OpenGL renderer at 960x544, the frame cap at 30 ("Match refresh rate" with vsync on), the render thread and culling on, and texture filter Nearest. Far clip at 3072 with fog gives the best frame rate in big outdoor levels. If you prefer the full view distance, turn far clip off in the debug menu and expect lower FPS in wide areas.

## Controls

The Vita buttons are mapped to the gamepad layout SRB2 expects, so the default controls work and everything can be rebound in Options. Cross, Circle, Square and Triangle are A, B, X and Y, L and R are the bumpers, Start and Select are Start and Back. In menus, Cross confirms, Circle goes back and Triangle answers "no". Menu prompts and the tutorial show the Vita button names.

Touching the top right corner of the screen opens the debug menu over the bottom half of the screen, while the game keeps running. From there you can change the FPS limit (30 or 60), show the FPS counter and SRB2's perfstats, and toggle far clip, fog, culling, the skybox, the render thread, the geometry thread, batching and a few things used for testing (walls, floors, sprites, draw submission). It also shows the real CPU/GPU clocks and free memory. Tap above the panel or on Close to hide it.

## Performance

Most maps run at 30 to 50 FPS. Some busy spots drop to around 22 FPS with fog and far clip disabled. A few maps are still slow, for example Deep Sea Zone Act 1 stays around 12 FPS because it has over 4000 active objects and the game logic itself is the bottleneck there, not the rendering. Those maps would need manual optimization one by one, which I didn't get to.

## What was changed

All SRB2 source changes are wrapped in `#ifdef __vita__`, so `grep -rln __vita__ SRB2-master/src` finds them. The Vita-only files are in `SRB2-master/src/vita/`. The build uses its own `CMakeLists.txt` and not SRB2's CMake.

**Platform.** `vita/i_video.c` replaces SDL's video code: it starts vitaGL, handles video modes, joystick input with the button remap, frame pacing, and shows errors with the system message dialog (SDL message boxes start their own GXM renderer and crash). The game sets the clocks to 444/222/222/166 MHz and checks them again every 60 frames, because the system restores lower clocks after suspend. Data is read from `ux0:data/srb2` (or `uma0:data/srb2`), configs and saves go there too, and logs go to `ux0:data/srb2/logs`. The heap is 256 MB with extended memory enabled.

**Rendering.** GLSL shaders are disabled and the game uses the fixed function path. The biggest early problems were GPU stalls: SRB2 copies the screen into textures with `glCopyTexSubImage2D`, which on vitaGL reads the frame back to the CPU. Now everything renders into an offscreen framebuffer and those copies are framebuffer blits, which fixed the slowdown underwater. SRB2 also keeps blending and alpha test on for every draw, which kills the Vita GPU's hidden surface removal, so opaque surfaces with opaque textures now draw with both turned off. Vertices are copied into vitaGL's per-frame scratch memory instead of being sent as client arrays, which Rinnegatamante recommended.

**CPU work.** After the GPU fixes the game was CPU bound. The Cortex-A9 has no integer divide, so the fixed point divisions used everywhere now go through the VFP in double precision with an exact correction (tested on PC against the original over hundreds of millions of cases, so game logic and demos stay identical). Other changes found with a sampling profiler in Vita3K: cached sort keys for sprites, a merge instead of a sort for translucent surfaces, cached lighting values, fewer repeated angle calculations, prefetching thinkers on the slow memory, and splitting the mobj interpolation work with a second core. Boot time on the console went from 3-5 minutes to under a minute, mostly by caching reads from the memory card and skipping work on music-only files.

**Threads.** A render thread on the second core replays the frame's GL calls through vitaGL while the game prepares the next frame. This took the GFZ2 start from about 23 to 30 FPS on hardware, at the cost of one frame of input lag. First-time texture uploads are also done by that thread now, which removed small hitches in menus. A level geometry thread that builds walls and floors on the third core is also in, but I could not prove that it really helps on hardware, so it can be turned off in the debug menu.

**Culling.** SRB2's OpenGL renderer has no real occlusion culling: it only checks left and right with a generous margin and only treats solid walls as blocking. I tightened the view check, added vertical "clip windows" (the same idea as the software renderer's floor and ceiling clipping) so geometry hidden behind wall openings is skipped, and made the skybox render only when sky is actually visible. In Vita3K this cut the geometry per frame by half or more in some maps (for example the Techno Hill Act 2 start, where a big skybox was drawn without being seen). On real hardware the gains were smaller than in the emulator.

**Far clip and fog.** Far clip stops drawing level geometry beyond a distance, which helps a lot in open levels (the worst GFZ2 spot went from 23 to 43 FPS at 3072). The cut looked bad, so there is now a linear fog that fades into the sky's horizon color.

**Menus.** Screens without a level run at 60 Hz when possible, because their animations step 35 times a second and 30 Hz drops a step every few frames. If the Vita can't keep up it goes back to 30 Hz automatically.

## Building

You need [VitaSDK](https://vitasdk.org/) (with `VITASDK` set, default `/usr/local/vitasdk`), CMake, git, curl and unzip. Then run:

```sh
tools/build.sh
```

This clones and builds vitaGL into `tools/vitaGL` at a pinned commit (newer vitaGL needs a newer vitashark than the SDK has), generates the LiveArea images and builds `build/srb2.vpk`.

## Installing

Install `srb2.vpk` with VitaShell. Then run `tools/make_data.sh`, which downloads the official 2.2.15 release and unpacks the game data into `data/srb2`, and copy that folder to `ux0:data/srb2`. Copying `data/shader_cache` to `ux0:data/shader_cache` is optional, it contains precompiled shaders so the first frames don't stall. You also need `libshacccg.suprx` in `ur0:data/`, since vitaGL compiles shaders at runtime. A console with HENkaku or Ensō is required.

## Known issues

Netplay is untested. The software renderer works but the intro scenes show a white background without the story text. There is no system keyboard for text input. Some menu animations and level title cards can still hitch slightly the first time new shaders are compiled. Heavy maps like Deep Sea Zone Act 1 are limited by game logic.

## Credits and notes

Sonic Robo Blast 2 is made by Sonic Team Junior. vitaGL is by Rinnegatamante, who also gave advice on vertex submission and culling.

Claude was a major help on this project. SRB2's code is big and complicated, and finding where the time actually goes on the Vita, and changing it without breaking the game logic, was a lot harder than I expected.

Parts of the code were written or improved with the assistance of AI (Claude, by Anthropic). All AI-assisted code was reviewed, tested, and manually validated, with the overall implementation and debugging process remaining under human supervision.

## License

SRB2 is © Sonic Team Junior and is distributed under the GNU General Public License v2 (see `SRB2-master/LICENSE`). The changes in this port are released under the same license. Sonic the Hedgehog and related characters are trademarks of SEGA. The game data is not included in this repository; `tools/make_data.sh` downloads it from the official release.
