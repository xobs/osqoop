set(Greater_SRCS Greater.cpp)
qt4_automoc(${Greater_SRCS})
include_directories (${CMAKE_BINARY_DIR}/processing/Greater)
add_library(Greater MODULE ${Greater_SRCS})
install(TARGETS Greater DESTINATION share/osqoop/processing)