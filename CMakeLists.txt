cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
	if(DEFINED ENV{VITASDK})
		set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
	else()
		message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
	endif()
endif()

project(srb2-vita C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(SRB2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/SRB2-master")
set(VITAGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/vitaGL")

if(NOT EXISTS "${VITAGL_DIR}/libvitaGL.a")
	message(FATAL_ERROR "vitaGL not built, run tools/build_vitagl.sh first")
endif()
set(SRB2_SRC "${SRB2_ROOT}/src")

file(STRINGS "${SRB2_SRC}/version.h" SRB2_VERSION_LINE LIMIT_COUNT 1)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" SRB2_VERSION "${SRB2_VERSION_LINE}")

set(VITA_APP_NAME "Sonic Robo Blast 2")
set(VITA_TITLEID  "SRBV00001")
# param.sfo wants XX.YY, so 2.2.15 becomes 22.15
string(REGEX REPLACE "^([0-9])\\.([0-9])\\.([0-9]+)$" "\\1\\2.\\3" VITA_VERSION "${SRB2_VERSION}")
if(NOT VITA_VERSION MATCHES "^[0-9][0-9]\\.[0-9][0-9]$")
	set(VITA_VERSION "01.00")
endif()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(SRB2_COMP_REVISION "${SRB2_VERSION}")
set(SRB2_COMP_BRANCH "vita")
set(SRB2_COMP_NOTE "PS Vita")
set(SRB2_COMP_OPTIMIZED 1)
configure_file("${SRB2_SRC}/config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/config.h")

set(SRB2_CORE
	comptime.c md5.c string.c
	d_main.c dehacked.c deh_soc.c deh_lua.c deh_tables.c z_zone.c
	f_finale.c f_wipe.c g_demo.c g_game.c g_input.c am_map.c
	command.c console.c hu_stuff.c i_time.c y_inter.c st_stuff.c
	m_aatree.c m_anigif.c m_argv.c m_bbox.c m_cheat.c m_cond.c m_easing.c
	m_fixed.c m_menu.c m_misc.c m_perfstats.c m_random.c m_tokenizer.c
	m_queue.c m_vector.c info.c
	p_ceilng.c p_enemy.c p_floor.c p_inter.c p_lights.c p_map.c p_maputl.c
	p_mobj.c p_polyobj.c p_saveg.c p_setup.c p_sight.c p_spec.c p_telept.c
	p_tick.c p_user.c p_slopes.c tables.c
	r_bsp.c r_data.c r_draw.c r_fps.c r_main.c r_plane.c r_segs.c r_skins.c
	r_sky.c r_splats.c r_things.c r_bbox.c r_textures.c r_translation.c
	r_patch.c r_patchrotation.c r_picformats.c r_portal.c
	screen.c taglist.c v_video.c s_sound.c sounds.c w_wad.c filesrch.c
	lzf.c b_bot.c u_list.c snake.c apng.c
	lua_script.c lua_baselib.c lua_mathlib.c lua_hooklib.c lua_consolelib.c
	lua_infolib.c lua_mobjlib.c lua_playerlib.c lua_skinlib.c
	lua_thinkerlib.c lua_maplib.c lua_taglib.c lua_polyobjlib.c
	lua_blockmaplib.c lua_hudlib.c lua_hudlib_drawlist.c lua_colorlib.c
	lua_inputlib.c

	blua/lapi.c blua/lbaselib.c blua/ldo.c blua/lfunc.c blua/linit.c
	blua/liolib.c blua/llex.c blua/lmem.c blua/lobject.c blua/lstate.c
	blua/lstrlib.c blua/ltablib.c blua/lundump.c blua/lzio.c blua/lauxlib.c
	blua/lcode.c blua/ldebug.c blua/ldump.c blua/lgc.c blua/lopcodes.c
	blua/lparser.c blua/lstring.c blua/ltable.c blua/ltm.c blua/lvm.c
	blua/loslib.c

	netcode/d_clisrv.c netcode/server_connection.c netcode/client_connection.c
	netcode/tic_command.c netcode/net_command.c netcode/gamestate.c
	netcode/commands.c netcode/d_net.c netcode/d_netcmd.c netcode/d_netfil.c
	netcode/http-mserv.c netcode/i_tcp.c netcode/mserv.c

	hardware/hw_bsp.c hardware/hw_draw.c hardware/hw_light.c hardware/hw_main.c
	hardware/hw_clip.c hardware/hw_md2.c hardware/hw_cache.c
	hardware/hw_md2load.c hardware/hw_md3load.c hardware/hw_model.c
	hardware/hw_batching.c hardware/hw_shaders.c hardware/r_opengl/r_opengl.c

	sdl/i_threads.c sdl/i_net.c sdl/i_system.c sdl/i_main.c
	sdl/dosstr.c sdl/endtxt.c sdl/hwsym_sdl.c sdl/mixer_sound.c

	vita/i_video.c vita/ogl_vita.c vita/vita_io.c vita/vita_debug.c vita/vita_rthread.c
)
list(TRANSFORM SRB2_CORE PREPEND "${SRB2_SRC}/")

add_executable(srb2 ${SRB2_CORE})

target_include_directories(srb2 PRIVATE
	"${VITAGL_DIR}/source"
	"${CMAKE_CURRENT_BINARY_DIR}"
	"${SRB2_SRC}"
	"$ENV{VITASDK}/arm-vita-eabi/include/SDL2"
)

target_compile_definitions(srb2 PRIVATE
	CMAKECONFIG
	HAVE_SDL
	HWRENDER
	HAVE_MIXER
	SOUND=SOUND_MIXER
	HAVE_ZLIB
	HAVE_PNG
	HAVE_OPENMPT
	HAVE_CURL
	NOLOADSO
	NOMUMBLE
	NOEXECINFO
	NOTERMIOS
	NOCWD
	NOHOME
	NO_IPV6
	_LARGEFILE64_SOURCE
	NDEBUG
)

target_compile_options(srb2 PRIVATE
	$<$<COMPILE_LANGUAGE:C>:-std=gnu11>
	-O3 -flto=auto
	-mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
	-fsigned-char
	-fwrapv
	-fno-strict-aliasing
	-fno-common
	-ffunction-sections -fdata-sections
	-Wall -Wno-unused -Wno-unused-parameter -Wno-sign-compare
	-Wno-misleading-indentation -Wno-format-truncation -Wno-stringop-truncation
	-Wno-address-of-packed-member -Wno-format-overflow -Wno-stringop-overflow
	-Wno-dangling-pointer -Wno-incompatible-pointer-types
)

target_link_options(srb2 PRIVATE -Wl,-q -Wl,--gc-sections -flto=auto -O3 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb)

target_link_libraries(srb2
	SDL2_mixer
	SDL2
	"${VITAGL_DIR}/libvitaGL.a"
	vitashark
	SceShaccCgExt
	mathneon
	taihen_stub
	curl
	ssl
	crypto
	zstd
	openmpt
	mpg123
	modplug
	xmp-lite
	opusfile
	opus
	vorbisfile
	vorbis
	ogg
	png16
	z
	stdc++
	pthread
	m
	SceAppMgr_stub
	SceAppUtil_stub
	SceAudio_stub
	SceAudioIn_stub
	SceCommonDialog_stub
	SceCtrl_stub
	SceDisplay_stub
	SceGxm_stub
	SceHid_stub
	SceIme_stub
	SceKernelDmacMgr_stub
	SceMotion_stub
	SceNet_stub
	SceNetCtl_stub
	ScePower_stub
	SceRtc_stub
	SceShaccCg_stub
	SceSysmodule_stub
	SceTouch_stub
	SceIofilemgr_stub
	SceProcessmgr_stub
)

# Extended memory budget for vitaGL's RAM pool
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

vita_create_self(eboot.bin srb2 UNSAFE)

vita_create_vpk(srb2.vpk ${VITA_TITLEID} eboot.bin
	VERSION ${VITA_VERSION}
	NAME ${VITA_APP_NAME}
	FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png" sce_sys/icon0.png
	FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png" sce_sys/livearea/contents/bg.png
	FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png" sce_sys/livearea/contents/startup.png
	FILE "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml" sce_sys/livearea/contents/template.xml
)
