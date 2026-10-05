file(GLOB SRC "*.cpp")

SET(APP_NAME ${PROJECT_NAME}App)
SET(CRC32_LIB crc32)
SET(MONOTONIC_CLOCK_LIB monotonic_clock)

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lstdc++")

add_subdirectory(${CRC32_LIB})
add_subdirectory(${MONOTONIC_CLOCK_LIB})
add_executable(${APP_NAME} ${SRC})

# Build with C++17
set_target_properties(
  ${APP_NAME}
  PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_include_directories(${APP_NAME}
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}/monotonic_clock
    ${CMAKE_CURRENT_SOURCE_DIR}/crc32)

target_link_libraries(${APP_NAME} aio)
target_link_libraries(${APP_NAME} flashcache)
target_link_libraries(${APP_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/../../jemalloc/lib/libjemalloc.a)
target_link_libraries(${APP_NAME} pthread rt dl m)
target_link_libraries(${APP_NAME} ${CRC32_LIB})
target_link_libraries(${APP_NAME} ${MONOTONIC_CLOCK_LIB})
