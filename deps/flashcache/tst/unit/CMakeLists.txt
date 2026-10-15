# googletest/googlemock (set CMAKE_PREFIX_PATH for a non-system install)
find_package(GTest REQUIRED)

# Same allocator as the library
if(FC_USE_JEMALLOC)
    set(FC_JEMALLOC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../jemalloc")
    if(NOT EXISTS "${FC_JEMALLOC_DIR}/lib/libjemalloc.a")
        message(FATAL_ERROR "jemalloc not built: run 'make -C deps jemalloc' first")
    endif()
endif()

file(GLOB unit_test_SOURCES "*.cpp")
add_executable(unitTests ${unit_test_SOURCES})

set_target_properties(unitTests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

target_include_directories(unitTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(unitTests PRIVATE _GNU_SOURCE)
if(FC_USE_JEMALLOC)
    target_include_directories(unitTests PRIVATE ${FC_JEMALLOC_DIR}/include)
    target_compile_definitions(unitTests PRIVATE JEMALLOC_MANGLE)
    target_compile_options(unitTests PRIVATE -include jemalloc/jemalloc.h)
    set(FC_TEST_ALLOC_LIB ${FC_JEMALLOC_DIR}/lib/libjemalloc.a)
endif()

# Route flash IO and libaio through fio_mock.cpp and libaio_mock.cpp
target_link_options(unitTests PRIVATE
    -Wl,--wrap=fioCreateContext
    -Wl,--wrap=fioSubmit
    -Wl,--wrap=fioGetCompletedRequest
    -Wl,--wrap=fioReleaseContext
    -Wl,--wrap=fioAllocate
    -Wl,--wrap=io_submit
    -Wl,--wrap=io_getevents
    -Wl,--wrap=io_setup)

target_link_libraries(unitTests PRIVATE
    flashcache
    ${FC_TEST_ALLOC_LIB}
    GTest::gmock_main
    pthread rt dl m)
