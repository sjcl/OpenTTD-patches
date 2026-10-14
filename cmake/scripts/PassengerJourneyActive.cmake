cmake_minimum_required(VERSION 3.17)

# Exercise the actual migration/setting/afterload hooks in fresh game processes.
file(MAKE_DIRECTORY "${WORK_DIR}/scripts")
if(EDITBIN_EXECUTABLE)
    get_filename_component(binary_dir "${OPENTTD_EXECUTABLE}" DIRECTORY)
    set(console_binary "${binary_dir}/passenger_journey_active_test.exe")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${OPENTTD_EXECUTABLE}" "${console_binary}" RESULT_VARIABLE copied)
    execute_process(COMMAND "${EDITBIN_EXECUTABLE}" /nologo /subsystem:console "${console_binary}" RESULT_VARIABLE edited)
    if(NOT copied EQUAL 0 OR NOT edited EQUAL 0)
        message(FATAL_ERROR "Could not prepare active production test executable")
    endif()
    set(OPENTTD_EXECUTABLE "${console_binary}")
endif()
set(setting "linkgraph.passenger_journey_routing")

function(load_game stage input config_enabled feature enabled commands)
    set(gui_preferences "")
    if(ARGC GREATER 6)
        set(gui_preferences "${ARGV6}")
    endif()
    file(WRITE "${WORK_DIR}/openttd.cfg"
        "[misc]\nlanguage = english.lng\n[gui]\nautosave = off\nautosave_on_exit = false\nthreaded_saves = false\n${gui_preferences}[linkgraph]\npassenger_journey_routing = ${config_enabled}\n")
    file(WRITE "${WORK_DIR}/${stage}-console.log" "")
    file(WRITE "${WORK_DIR}/scripts/game_start.scr"
        "script \"${WORK_DIR}/${stage}-console.log\"\nsetting ${setting}\n${commands}\nquit\n")
    execute_process(COMMAND "${OPENTTD_EXECUTABLE}" -x -Q -c "${WORK_DIR}/openttd.cfg" -g "${input}"
        -vnull:ticks=10 -snull -mnull -d sl=2 WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics TIMEOUT 15)
    file(WRITE "${WORK_DIR}/${stage}-process.log" "${output}\n${diagnostics}")
    if(NOT result EQUAL 0 OR NOT diagnostics MATCHES "Passenger journey afterload: feature ${feature}, enabled ${enabled}")
        message(FATAL_ERROR "${stage}: production migration/validation failed: ${result} ${diagnostics}")
    endif()
    file(READ "${WORK_DIR}/${stage}-console.log" console)
    if(NOT console MATCHES "Current value for '${setting}' is: '${enabled}'")
        message(FATAL_ERROR "${stage}: incorrect persisted routing setting: ${console}")
    endif()
endfunction()

load_game(legacy "${LEGACY_SAVE}" true 0 false
    "setting ${setting} true\nsave \"${WORK_DIR}/enabled\"")
if(NOT EXISTS "${WORK_DIR}/enabled.sav")
    message(FATAL_ERROR "Active routing current save was not written")
endif()
load_game(enabled "${WORK_DIR}/enabled.sav" false 4 true
    "save \"${WORK_DIR}/without-window\"")
load_game(window "${WORK_DIR}/enabled.sav" false 4 true
    "show_station_window 0\nsave \"${WORK_DIR}/with-window\"")
file(SHA256 "${WORK_DIR}/without-window.sav" without_window)
file(SHA256 "${WORK_DIR}/with-window.sav" with_window)
if(NOT without_window STREQUAL with_window)
    message(FATAL_ERROR "Opening the station GUI changed the saved game state")
endif()
load_game(diagnostics "${WORK_DIR}/enabled.sav" false 4 true
    "passenger_journey_stats total\npassenger_journey_stats tick\npassenger_journey_stats inventory\ndump_linkgraph_jobs\npassenger_journey_stats reset\nsave \"${WORK_DIR}/with-diagnostics\"")
file(SHA256 "${WORK_DIR}/with-diagnostics.sav" with_diagnostics)
if(NOT without_window STREQUAL with_diagnostics)
    message(FATAL_ERROR "Reading/resetting passenger diagnostics changed the saved game state")
endif()
load_game(planned "${WORK_DIR}/enabled.sav" false 4 true
    "show_station_window 0\nsave \"${WORK_DIR}/planned-window\""
    "station_gui_sort_by = 3\nstation_gui_sort_order = 1\nstation_gui_group_order = 5\n")
file(SHA256 "${WORK_DIR}/planned-window.sav" planned_window)
if(NOT without_window STREQUAL planned_window)
    message(FATAL_ERROR "Changing station GUI grouping/sorting/planned mode changed the saved game state")
endif()
load_game(disable "${WORK_DIR}/enabled.sav" false 4 true
    "setting ${setting} false\nsave \"${WORK_DIR}/disabled\"")
load_game(disabled "${WORK_DIR}/disabled.sav" true 4 false "")
message(STATUS "Active routing legacy migration, setting round trips, afterload hook and GUI state purity passed")
