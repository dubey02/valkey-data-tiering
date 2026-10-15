file(GLOB SRC "*.cpp")

add_library(${CRC32_LIB} STATIC ${SRC})

# Build with C++17
set_target_properties(
  ${CRC32_LIB}
  PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)
