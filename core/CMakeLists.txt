cmake_minimum_required(VERSION 3.22)
project(airplaytv C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

option(AIRPLAYTV_ANDROID "Build the Android JNI library" OFF)
option(AIRPLAYTV_TESTS "Build host unit tests" OFF)
option(AIRPLAYTV_FUZZ "Build libFuzzer targets (clang only)" OFF)
option(AIRPLAYTV_SANITIZE "Build with address and undefined behaviour sanitizers" OFF)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Thin LTO for release builds of the Android library (lld handles the bitcode).
if(AIRPLAYTV_ANDROID AND CMAKE_BUILD_TYPE MATCHES "Rel")
    add_compile_options(-flto=thin)
    add_link_options(-flto=thin)
endif()

set(COMMON_FLAGS -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -ffunction-sections -fdata-sections)

# Sanitizer builds: AddressSanitizer everywhere, UndefinedBehaviorSanitizer on our
# own code (the vendored FairPlay code relies on type punning by design).
if(AIRPLAYTV_FUZZ OR AIRPLAYTV_SANITIZE)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    list(APPEND COMMON_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined)
endif()
if(AIRPLAYTV_FUZZ)
    add_compile_options(-fsanitize=fuzzer-no-link)
endif()

# --- third party --------------------------------------------------------------

add_library(tp_mbedtls STATIC
    ${TP}/mbedtls/library/aes.c
    ${TP}/mbedtls/library/aesce.c
    ${TP}/mbedtls/library/aesni.c
    ${TP}/mbedtls/library/bignum.c
    ${TP}/mbedtls/library/bignum_core.c
    ${TP}/mbedtls/library/block_cipher.c
    ${TP}/mbedtls/library/constant_time.c
    ${TP}/mbedtls/library/gcm.c
    ${TP}/mbedtls/library/md5.c
    ${TP}/mbedtls/library/platform_util.c
    ${TP}/mbedtls/library/sha1.c
    ${TP}/mbedtls/library/sha512.c)
target_include_directories(tp_mbedtls PUBLIC ${TP}/mbedtls/include ${TP}/mbedtls)
target_include_directories(tp_mbedtls PRIVATE ${TP}/mbedtls/library)
target_compile_definitions(tp_mbedtls PUBLIC MBEDTLS_CONFIG_FILE="airplaytv_mbedtls_config.h")
target_compile_options(tp_mbedtls PRIVATE -O2 -w -ffunction-sections -fdata-sections)

add_library(tp_monocypher STATIC
    ${TP}/monocypher/monocypher.c
    ${TP}/monocypher/monocypher-ed25519.c)
target_include_directories(tp_monocypher PUBLIC ${TP}/monocypher)
target_compile_options(tp_monocypher PRIVATE -O2 -w -ffunction-sections -fdata-sections)

add_library(tp_playfair STATIC
    ${TP}/playfair/playfair.c
    ${TP}/playfair/omg_hax.c
    ${TP}/playfair/hand_garble.c
    ${TP}/playfair/modified_md5.c
    ${TP}/playfair/sap_hash.c)
target_include_directories(tp_playfair PUBLIC ${TP}/playfair)
target_compile_options(tp_playfair PRIVATE -O2 -w -ffunction-sections -fdata-sections)

add_library(tp_alac STATIC ${TP}/alac/alac.c)
target_include_directories(tp_alac PUBLIC ${TP}/alac)
target_compile_options(tp_alac PRIVATE -O2 -ffunction-sections -fdata-sections)

# --- protocol core ------------------------------------------------------------

add_library(airplay_core STATIC
    src/airplay.c
    src/audio_rtp.c
    src/bplist.c
    src/crypto.c
    src/dmap.c
    src/fairplay.c
    src/log.c
    src/mirror.c
    src/netutil.c
    src/ntp.c
    src/pairing.c
    src/rtsp.c
    src/srp.c
    src/stats.c
    src/util.c)
target_include_directories(airplay_core PUBLIC src)
target_compile_definitions(airplay_core PUBLIC _GNU_SOURCE)
target_compile_options(airplay_core PRIVATE ${COMMON_FLAGS})
target_link_libraries(airplay_core PUBLIC tp_mbedtls tp_monocypher tp_playfair tp_alac)
if(NOT ANDROID)
    find_package(Threads REQUIRED)
    target_link_libraries(airplay_core PUBLIC Threads::Threads)
endif()

# --- Android JNI library ------------------------------------------------------

if(AIRPLAYTV_ANDROID)
    add_library(airplaytv SHARED
        android/jni_main.c
        android/audio_pipeline.c
        android/video_decoder.c)
    target_compile_options(airplaytv PRIVATE ${COMMON_FLAGS} -fvisibility=hidden)
    target_link_libraries(airplaytv PRIVATE airplay_core log mediandk android)
    target_link_options(airplaytv PRIVATE
        -Wl,--gc-sections
        -Wl,--exclude-libs,ALL
        -Wl,--as-needed
        -Wl,-z,max-page-size=16384)
endif()

# --- tests --------------------------------------------------------------------

if(AIRPLAYTV_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
