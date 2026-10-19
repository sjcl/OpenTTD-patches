set(CMAKE_CXX_COMPILER_ID "${CHECK_COMPILER}")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/ProfileGuidedOptimization.cmake")
configure_pgo()
