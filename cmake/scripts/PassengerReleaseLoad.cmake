cmake_minimum_required(VERSION 3.17)

# An actual jgrpp-ya-0.73.3+2 save, rather than a simulated development version.
file(SHA256 "${LEGACY_SAVE}" release_save_hash)
if(NOT release_save_hash STREQUAL "102196929c429c595987afaa58a1b7cfa880985a974c5952544991f64fc8b6fe")
    message(FATAL_ERROR "The previous-release compatibility fixture changed")
endif()
set(release_work_dir "${WORK_DIR}")
set(WORK_DIR "${release_work_dir}/od")
include("${CMAKE_CURRENT_LIST_DIR}/PassengerOdLoad.cmake")
file(READ "${WORK_DIR}/legacy-load.log" released_load)
foreach(release_marker
    "SLXI version label: jgrpp-ya-0.73.3+2"
    "'linkgraph_route_penalty', version: 1,"
    "'passenger_via', version: 1,")
    string(FIND "${released_load}" "${release_marker}" release_marker_at)
    if(release_marker_at EQUAL -1)
        message(FATAL_ERROR "Previous-release feature was not loaded: ${release_marker}")
    endif()
endforeach()
set(WORK_DIR "${release_work_dir}/journey")
include("${CMAKE_CURRENT_LIST_DIR}/PassengerJourneyActive.cmake")
message(STATUS "jgrpp-ya-0.73.3+2 saves load, migrate and survive current-format round trips")
