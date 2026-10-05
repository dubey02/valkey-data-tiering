# googletest/googlemock (set CMAKE_PREFIX_PATH for a non-system install)
find_package(GTest REQUIRED)

# In-tree jemalloc, same as the library
set(FC_JEMALLOC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../jemalloc")
if(NOT EXISTS "${FC_JEMALLOC_DIR}/lib/libjemalloc.a")
    message(FATAL_ERROR "jemalloc not built: run 'make -C deps jemalloc' first")
endif()

file(GLOB unit_test_SOURCES "*.cpp")
add_executable(unitTests ${unit_test_SOURCES})

set_target_properties(unitTests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

target_include_directories(unitTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${FC_JEMALLOC_DIR}/include)
target_compile_definitions(unitTests PRIVATE JEMALLOC_MANGLE _GNU_SOURCE)
target_compile_options(unitTests PRIVATE -include jemalloc/jemalloc.h)

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
    ${FC_JEMALLOC_DIR}/lib/libjemalloc.a
    GTest::gmock_main
    pthread rt dl m)
