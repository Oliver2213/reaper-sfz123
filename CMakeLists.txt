cmake_minimum_required(VERSION 3.21)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architectures (arm64;x86_64 for universal)")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

project(sfz123 VERSION 1.0.0
    DESCRIPTION "Accessible SF2, SF3 and SFZ player for REAPER"
    HOMEPAGE_URL "https://github.com/Oliver2213/reaper-sfz123"
    LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------- sfizz (SFZ engine)
set(SFIZZ_JACK OFF CACHE BOOL "" FORCE)
set(SFIZZ_RENDER OFF CACHE BOOL "" FORCE)
set(SFIZZ_SHARED OFF CACHE BOOL "" FORCE)
set(SFIZZ_TESTS OFF CACHE BOOL "" FORCE)
set(SFIZZ_DEMOS OFF CACHE BOOL "" FORCE)
set(SFIZZ_DEVTOOLS OFF CACHE BOOL "" FORCE)
set(SFIZZ_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO OFF CACHE BOOL "" FORCE)
# sfizz is archived upstream; our changes (mostly more opcodes) live in patches/sfizz.patch, which
# is applied when the submodule is clean. After editing sfizz, regenerate it with
#   git -C third_party/sfizz diff > patches/sfizz.patch
set(SFIZZ_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/patches/sfizz.patch)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SFIZZ_PATCH})
set(_sfizz_dir ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sfizz)
execute_process(COMMAND git diff --quiet WORKING_DIRECTORY ${_sfizz_dir} RESULT_VARIABLE _sfizz_dirty)
if(_sfizz_dirty EQUAL 0)
    execute_process(COMMAND git apply ${SFIZZ_PATCH} WORKING_DIRECTORY ${_sfizz_dir} RESULT_VARIABLE _sfizz_apply)
    if(NOT _sfizz_apply EQUAL 0)
        message(FATAL_ERROR "patches/sfizz.patch doesn't apply to third_party/sfizz")
    endif()
    message(STATUS "Applied patches/sfizz.patch")
else()
    execute_process(COMMAND git apply --reverse --check ${SFIZZ_PATCH}
        WORKING_DIRECTORY ${_sfizz_dir} RESULT_VARIABLE _sfizz_matches OUTPUT_QUIET ERROR_QUIET)
    if(NOT _sfizz_matches EQUAL 0)
        message(WARNING "third_party/sfizz has changes that aren't in patches/sfizz.patch; building them as they are")
    endif()
endif()

# newer clang rejects a construct in sfizz's vendored atomic_queue header
set(_sfizz_saved_flags "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-missing-template-arg-list-after-template-kw -w")
add_subdirectory(third_party/sfizz EXCLUDE_FROM_ALL)
set(CMAKE_CXX_FLAGS "${_sfizz_saved_flags}")

# ---------------------------------------------------------------- Rust SF2/SF3 engine (SFLT sfl2 core)
set(SF2ENGINE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/engine-sf2)
set(SF2ENGINE_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo)

set(_rust_libs "")
foreach(arch ${CMAKE_OSX_ARCHITECTURES})
    if(arch STREQUAL "arm64")
        set(triple aarch64-apple-darwin)
    else()
        set(triple x86_64-apple-darwin)
    endif()
    set(lib ${SF2ENGINE_TARGET_DIR}/${triple}/release/libsf2engine.a)
    add_custom_command(
        OUTPUT ${lib}
        COMMAND ${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
                cargo build --release --target ${triple} --target-dir ${SF2ENGINE_TARGET_DIR}
        WORKING_DIRECTORY ${SF2ENGINE_DIR}
        DEPENDS ${SF2ENGINE_DIR}/Cargo.toml
                ${SF2ENGINE_DIR}/src/lib.rs ${SF2ENGINE_DIR}/src/synth.rs ${SF2ENGINE_DIR}/src/fx.rs
                ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sfl2/src/lib.rs ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sfl2/src/riff.rs
        COMMENT "Building Rust SF2 engine for ${triple}"
        VERBATIM)
    list(APPEND _rust_libs ${lib})
endforeach()

set(SF2ENGINE_LIB ${CMAKE_CURRENT_BINARY_DIR}/libsf2engine_universal.a)
add_custom_command(
    OUTPUT ${SF2ENGINE_LIB}
    COMMAND lipo -create ${_rust_libs} -output ${SF2ENGINE_LIB}
    DEPENDS ${_rust_libs}
    VERBATIM)
add_custom_target(sf2engine_build DEPENDS ${SF2ENGINE_LIB})

# ---------------------------------------------------------------- plugin
add_library(sfz123 MODULE
    src/entry.cpp
    src/plugin.cpp
    src/params.cpp
    src/engine_sf2.cpp
    src/engine_sfz.cpp
    src/state.cpp
    src/reaper.cpp
    src/sfz_files.cpp
    src/prefs_mac.mm
    src/gui_mac.mm
)
add_dependencies(sfz123 sf2engine_build)

target_include_directories(sfz123 PRIVATE
    src
    include
    third_party/clap/include
)
target_compile_definitions(sfz123 PRIVATE SFZ123_VERSION="${PROJECT_VERSION}")
target_compile_options(sfz123 PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fvisibility=hidden)
set_source_files_properties(src/gui_mac.mm src/prefs_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

target_link_libraries(sfz123 PRIVATE
    sfizz::sfizz
    ${SF2ENGINE_LIB}
    "-framework Cocoa"
    "-framework UniformTypeIdentifiers"
    "-framework CoreFoundation"
    "-framework Security"
)

set_target_properties(sfz123 PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION clap
    OUTPUT_NAME "sfz123"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.smoll.sfz123
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    XCODE_ATTRIBUTE_GENERATE_PKGINFO_FILE YES
)

# ad-hoc sign so macOS will load the bundle
add_custom_command(TARGET sfz123 POST_BUILD
    COMMAND codesign --force --sign - --timestamp=none "$<TARGET_BUNDLE_DIR:sfz123>"
    VERBATIM)

# ---------------------------------------------------------------- headless test host
add_executable(sfz123_test tests/host_test.cpp)
target_include_directories(sfz123_test PRIVATE third_party/clap/include)
add_dependencies(sfz123_test sfz123)

add_executable(sfz123_gui_test tests/gui_test.mm)
target_include_directories(sfz123_gui_test PRIVATE third_party/clap/include)
target_link_libraries(sfz123_gui_test PRIVATE "-framework Cocoa")
set_source_files_properties(tests/gui_test.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
add_dependencies(sfz123_gui_test sfz123)

# SFZ opcodes added by patches/sfizz.patch
add_executable(sfz123_opcode_test tests/opcode_test.cpp)
target_link_libraries(sfz123_opcode_test PRIVATE sfizz::sfizz)
