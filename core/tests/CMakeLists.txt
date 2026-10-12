add_library(test_support STATIC
    test_main.c
    h264_gen.c
    fake_sender.c)
target_link_libraries(test_support PUBLIC airplay_core m)
target_include_directories(test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_support PRIVATE ${COMMON_FLAGS})

# One runner with every unit and loopback test. Test files register themselves,
# so they are linked as object files rather than through a static library.
add_executable(airplaytv_tests
    test_crypto.c
    test_loopback.c
    test_media.c
    test_parsers.c)
target_link_libraries(airplaytv_tests PRIVATE test_support)
target_compile_options(airplaytv_tests PRIVATE ${COMMON_FLAGS})
add_test(NAME airplaytv_tests COMMAND airplaytv_tests)

# Sends a generated test stream to a running receiver (e.g. the app on an emulator).
add_executable(airplaytv_sender fake_sender_main.c h264_gen.c fake_sender.c)
target_link_libraries(airplaytv_sender PRIVATE airplay_core m)
target_compile_options(airplaytv_sender PRIVATE ${COMMON_FLAGS})

if(AIRPLAYTV_FUZZ)
    target_compile_definitions(airplay_core PUBLIC AIRPLAYTV_FUZZING)
    foreach(target bplist rtsp mirror alac dmap server)
        add_executable(fuzz_${target} fuzz/fuzz_${target}.c)
        target_link_libraries(fuzz_${target} PRIVATE airplay_core)
        target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
        target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
    endforeach()
endif()
