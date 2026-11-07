
file(GLOB_RECURSE Audio_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
	"${PROJECT_SOURCE_DIR}/Utils/Common/*.cpp"
	"${PROJECT_SOURCE_DIR}/Utils/Audio/*.cpp"
)

if(EMSCRIPTEN)
	add_library(Audio STATIC ${Audio_SOURCES})
else()
	add_library(Audio SHARED ${Audio_SOURCES})
endif()
target_include_directories(Audio PUBLIC
	"${PROJECT_SOURCE_DIR}/Utils"
	"${PROJECT_SOURCE_DIR}/Utils/Audio"
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(CMAKE_DL_LIBS)
	target_link_libraries(Audio PRIVATE "${CMAKE_DL_LIBS}" nlohmann_json)
else()
	target_link_libraries(Audio PRIVATE nlohmann_json)
endif()
configure_engine_target(Audio)
if(WIN32)
copy_to_game(Audio "bin/audio.dll")
else()
copy_to_game(Audio "bin/audio.so")
endif()

if(EMSCRIPTEN)
else()
set(LIBTYPE STATIC CACHE STRING "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
include(FetchContent)

FetchContent_Declare(
    openal
    GIT_REPOSITORY https://github.com/kcat/openal-soft
    GIT_TAG master
)

FetchContent_MakeAvailable(openal)

FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/TabbyForgeEngine/miniaudio
    GIT_TAG master
)

FetchContent_MakeAvailable(miniaudio)

    target_link_libraries(Audio PRIVATE
        OpenAL miniaudio Filesystem
    )
endif()
target_compile_definitions(Audio PRIVATE Audio_EXPORTS)