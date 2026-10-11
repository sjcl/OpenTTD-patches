cmake_minimum_required(VERSION 3.17)

# Use console scripts with the null video driver: no GUI or multiplayer server.
# A fresh process for each load also tests persisted settings without prior state.
file(MAKE_DIRECTORY "${WORK_DIR}/scripts")
if(EDITBIN_EXECUTABLE)
    # Match the other Windows regression runners, avoiding GUI warning dialogs.
    get_filename_component(binary_dir "${OPENTTD_EXECUTABLE}" DIRECTORY)
    set(console_binary "${binary_dir}/passenger_od_load.exe")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${OPENTTD_EXECUTABLE}" "${console_binary}" RESULT_VARIABLE copied)
    execute_process(COMMAND "${EDITBIN_EXECUTABLE}" /nologo /subsystem:console "${console_binary}" RESULT_VARIABLE edited)
    if(NOT copied EQUAL 0 OR NOT edited EQUAL 0)
        message(FATAL_ERROR "Could not prepare the Windows console test executable")
    endif()
    set(OPENTTD_EXECUTABLE "${console_binary}")
endif()
set(setting "linkgraph.passenger_od_assignment")

function(load_game stage save config_enabled expected_feature expected_enabled commands)
    file(WRITE "${WORK_DIR}/openttd.cfg"
        "[misc]\nlanguage = english.lng\n"
        "[gui]\nautosave = off\nautosave_on_exit = false\nthreaded_saves = false\n"
        "[linkgraph]\npassenger_od_assignment = ${config_enabled}\n")
    file(WRITE "${WORK_DIR}/scripts/game_start.scr"
        "script \"${WORK_DIR}/${stage}-console.log\"\nsetting ${setting}\n${commands}\nquit\n")
    # Console logs append, so reset only this test's known output file.
    file(WRITE "${WORK_DIR}/${stage}-console.log" "")
    execute_process(COMMAND "${OPENTTD_EXECUTABLE}" -x -Q
        -c "${WORK_DIR}/openttd.cfg" -g "${save}"
        -vnull:ticks=10 -snull -mnull -d sl=2
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics TIMEOUT 15)
    file(WRITE "${WORK_DIR}/${stage}-load.log" "${output}\n${diagnostics}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${stage}: game load failed (${result}): ${diagnostics}")
    endif()
    # Checking the hook's trace makes a disconnected AfterLoadPassengerOd fail,
    # even if the setting's default coincidentally matches a legacy save.
    if(NOT diagnostics MATCHES "Passenger OD afterload: feature ${expected_feature}, enabled ${expected_enabled}, revision [0-9]+")
        message(FATAL_ERROR "${stage}: OD validation did not run as expected: ${diagnostics}")
    endif()
    file(READ "${WORK_DIR}/${stage}-console.log" console)
    if(NOT console MATCHES "Current value for '${setting}' is: '${expected_enabled}'")
        message(FATAL_ERROR "${stage}: wrong loaded setting or game-start script failed: ${console}")
    endif()
endfunction()

load_game(legacy "${LEGACY_SAVE}" true 0 false
    "setting ${setting} true\nsave \"${WORK_DIR}/enabled\"")
if(NOT EXISTS "${WORK_DIR}/enabled.sav")
    message(FATAL_ERROR "Saving a current-format game failed")
endif()
load_game(enabled "${WORK_DIR}/enabled.sav" false 3 true
    "setting ${setting} false\nsave \"${WORK_DIR}/disabled\"")
load_game(disabled "${WORK_DIR}/disabled.sav" true 3 false "")
message(STATUS "Passenger OD production save/load and legacy migration passed")
