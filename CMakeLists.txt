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
set(SRB2_VITAGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/vitaGL" CACHE PATH "vitaGL source tree with libvitaGL.a")
set(SRB2_VITA_SCE_SYS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys" CACHE PATH "LiveArea assets")

if(NOT EXISTS "${SRB2_VITAGL_DIR}/libvitaGL.a")
	message(FATAL_ERROR "vitaGL not built, run tools/build_vitagl.sh first")
endif()
set(VITAGL_DIR "${SRB2_VITAGL_DIR}")
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

include("${SRB2_ROOT}/cmake/SRB2CoreSources.cmake")

set(SRB2_CORE
	${SRB2_SHARED_CORE_BASE}
	apng.c
	${SRB2_SHARED_CORE_LUA}
)

set(SRB2_VITA_BLUA_SOURCES
	blua/lapi.c blua/lbaselib.c blua/ldo.c blua/lfunc.c blua/linit.c
	blua/liolib.c blua/llex.c blua/lmem.c blua/lobject.c blua/lstate.c
	blua/lstrlib.c blua/ltablib.c blua/lundump.c blua/lzio.c blua/lauxlib.c
	blua/lcode.c blua/ldebug.c blua/ldump.c blua/lgc.c blua/lopcodes.c
	blua/lparser.c blua/lstring.c blua/ltable.c blua/ltm.c blua/lvm.c
	blua/loslib.c
)

set(SRB2_VITA_NET_SOURCES
	netcode/d_clisrv.c netcode/server_connection.c netcode/client_connection.c
	netcode/tic_command.c netcode/net_command.c netcode/gamestate.c
	netcode/commands.c netcode/d_net.c netcode/d_netcmd.c netcode/d_netfil.c
	netcode/http-mserv.c netcode/i_tcp.c netcode/mserv.c
)

set(SRB2_VITA_RENDER_SOURCES
	hardware/hw_bsp.c hardware/hw_draw.c hardware/hw_light.c hardware/hw_main.c
	hardware/hw_clip.c hardware/hw_md2.c hardware/hw_cache.c
	hardware/hw_md2load.c hardware/hw_md3load.c hardware/hw_model.c
	hardware/hw_batching.c hardware/hw_shaders.c hardware/r_opengl/r_opengl.c
)

set(SRB2_VITA_SDL_SOURCES
	sdl/i_threads.c sdl/i_net.c sdl/i_system.c sdl/i_main.c
	sdl/dosstr.c sdl/endtxt.c sdl/hwsym_sdl.c sdl/mixer_sound.c
)

set(SRB2_VITA_BACKEND_SOURCES
	vita/i_video.c vita/ogl_vita.c vita/vita_io.c vita/vita_debug.c vita/vita_rthread.c vita/vita_framelog.c vita/vita_bench.c vita/vita_prof.c
)

list(APPEND SRB2_CORE
	${SRB2_VITA_BLUA_SOURCES}
	${SRB2_VITA_NET_SOURCES}
	${SRB2_VITA_RENDER_SOURCES}
	${SRB2_VITA_SDL_SOURCES}
	${SRB2_VITA_BACKEND_SOURCES}
)
list(TRANSFORM SRB2_CORE PREPEND "${SRB2_SRC}/")

add_executable(srb2 ${SRB2_CORE})
option(SRB2_PERF_COUNT "Enable opt-in collision-query counting diagnostics" OFF)
if(SRB2_PERF_COUNT)
	target_sources(srb2 PRIVATE "${SRB2_SRC}/perf_count.c")
	target_compile_definitions(srb2 PRIVATE SRB2_PERF_COUNT)
endif()

option(SRB2_REWRITE_DIAGNOSTICS "Enable rewrite diagnostics" OFF)
if(SRB2_REWRITE_DIAGNOSTICS)
	target_sources(srb2 PRIVATE "${SRB2_SRC}/rewrite/diagnostics.c" "${SRB2_SRC}/rewrite/diagnostics_console.c")
	target_compile_definitions(srb2 PRIVATE SRB2_REWRITE_DIAGNOSTICS)
endif()

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
	FILE "${SRB2_VITA_SCE_SYS_DIR}/icon0.png" sce_sys/icon0.png
	FILE "${SRB2_VITA_SCE_SYS_DIR}/livearea/contents/bg.png" sce_sys/livearea/contents/bg.png
	FILE "${SRB2_VITA_SCE_SYS_DIR}/livearea/contents/startup.png" sce_sys/livearea/contents/startup.png
	FILE "${SRB2_VITA_SCE_SYS_DIR}/livearea/contents/template.xml" sce_sys/livearea/contents/template.xml
)
