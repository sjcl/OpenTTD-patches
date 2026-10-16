function(add_table_build_benchmark)
  target_sources(openttd_test PRIVATE
    "C:/Users/bebe/.codex/worktrees/c430/OpenTTD-patches/build-msvc/split-range-counted/table-build/PassengerTableReference.cpp"
    "C:/Users/bebe/.codex/worktrees/c430/OpenTTD-patches/build-msvc/split-range-counted/table-build/PassengerTableCandidate.cpp"
    "C:/Users/bebe/.codex/worktrees/c430/OpenTTD-patches/utils/passenger_table_build_benchmark.cpp"
    "C:/Users/bebe/.codex/worktrees/c430/OpenTTD-patches/build-msvc/split-range-counted/table-build/PassengerTableBeforeSIMD.cpp"
    "C:/Users/bebe/.codex/worktrees/c430/OpenTTD-patches/build-msvc/split-range-counted/table-build/PassengerTableSplitSIMD.cpp"
)
  target_include_directories(openttd_test PRIVATE "C:/Users/bebe/.codex/worktrees/c430/OpenTTD-patches/build-msvc/split-range-counted/table-build")
  target_compile_definitions(openttd_test PRIVATE WITH_TABLE_BUILD_RANGE_SPLIT)
  target_compile_definitions(openttd_test PRIVATE PASSENGER_TABLE_COUNT=0)
endfunction()
cmake_language(DEFER CALL add_table_build_benchmark)
