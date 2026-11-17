# Lossless Scaling frame generation: lsfg-vk's framegen (MIT, PancakeTAS and contributors;
# GameNative/lsfg-vk-android 437208a, the Android single-device mode), DXVK's DXBC translator
# (zlib, its 78ab59a extraction), pe-parse (MIT, 31ac596) and volk (MIT, be3dbd4).
#
# Built as liblsfg_framegen.so, which the presenter loads by name when the player turns
# LSFG on: framegen's volk defines every Vulkan entry point as a global, kept out of the
# runtime by the library boundary. The shaders come at run time from the player's own
# Lossless.dll (Lossless Scaling on Steam), never from this repository.

file(GLOB LSFG_FRAMEGEN_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/src/common/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/src/pool/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/v3.1_src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/v3.1_src/shaders/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/v3.1p_src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/v3.1p_src/shaders/*.cpp")
file(GLOB LSFG_DXBC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxbc/src/dxbc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxbc/src/spirv/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxbc/src/util/*.cpp")
set(LSFG_PEPARSE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pe-parse/pe-parser-library")

add_library(lsfg_framegen SHARED
    ${LSFG_FRAMEGEN_SOURCES}
    ${LSFG_DXBC_SOURCES}
    "${LSFG_PEPARSE_DIR}/src/buffer.cpp"
    "${LSFG_PEPARSE_DIR}/src/parse.cpp"
    "${LSFG_PEPARSE_DIR}/src/unicode_codecvt.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/volk/volk.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/extract/trans.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge/lsfg_bridge.cpp")
set_target_properties(lsfg_framegen PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)
target_include_directories(lsfg_framegen PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/public"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/v3.1_include"
    "${CMAKE_CURRENT_SOURCE_DIR}/framegen/v3.1p_include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/extract"
    "${CMAKE_CURRENT_SOURCE_DIR}/bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/volk"
    "${LSFG_PEPARSE_DIR}/include")
target_include_directories(lsfg_framegen SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxbc/include/dxbc"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxbc/include/spirv"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxbc/include/util"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dxbc/include/dxvk")
target_compile_definitions(lsfg_framegen PRIVATE VK_NO_PROTOTYPES $<$<BOOL:${ANDROID}>:VK_USE_PLATFORM_ANDROID_KHR>)
target_compile_options(lsfg_framegen PRIVATE
    -w -fexceptions -frtti)
target_link_libraries(lsfg_framegen PRIVATE Vulkan::Headers)
if(ANDROID)
    target_link_libraries(lsfg_framegen PRIVATE log android dl)
endif()
