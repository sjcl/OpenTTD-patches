cmake_minimum_required(VERSION 3.17)

# Local console diagnostics must leave the complete production save identical.
# LEGACY_SAVE may also name a user's passenger map for a repeatable real-map comparison.
file(MAKE_DIRECTORY "${WORK_DIR}")
if(NOT RUN_TIMEOUT)
    set(RUN_TIMEOUT 90)
endif()
if(NOT MONTHS)
    set(MONTHS 3)
endif()
if(NOT VIDEO_DRIVER)
    set(VIDEO_DRIVER "null:until_exit")
endif()
if(MONTHS LESS 1 OR MONTHS GREATER 12)
    message(FATAL_ERROR "MONTHS must be between 1 and 12")
endif()
set(scan_options -Q)
if(NEWGRF_DIR)
    # -Q deliberately skips the scan that registers TAR entries/checksums. Real
    # maps need that scan; use copies in the test's private data directories.
    set(scan_options)
endif()
function(prepare_newgrfs directory)
    if(NEWGRF_DIR)
        file(MAKE_DIRECTORY "${directory}/newgrf")
        file(GLOB assets "${NEWGRF_DIR}/*.grf" "${NEWGRF_DIR}/*.tar")
        file(COPY ${assets} DESTINATION "${directory}/newgrf")
    endif()
endfunction()
if(EDITBIN_EXECUTABLE)
    get_filename_component(binary_dir "${OPENTTD_EXECUTABLE}" DIRECTORY)
    set(console_binary "${binary_dir}/passenger_shadow_test.exe")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${OPENTTD_EXECUTABLE}" "${console_binary}" RESULT_VARIABLE copied)
    execute_process(COMMAND "${EDITBIN_EXECUTABLE}" /nologo /subsystem:console "${console_binary}" RESULT_VARIABLE edited)
    if(NOT copied EQUAL 0 OR NOT edited EQUAL 0)
        message(FATAL_ERROR "Could not prepare shadow production test executable")
    endif()
    set(OPENTTD_EXECUTABLE "${console_binary}")
endif()

# Migrate once before forking. Legacy loads generate a fresh savegame UID and
# generation_unique_id, independently of diagnostics; both runs must share them.
# -d also initializes the Windows console, keeping font notices out of modal dialogs.
set(initial "${WORK_DIR}/initial")
file(MAKE_DIRECTORY "${initial}/scripts")
prepare_newgrfs("${initial}")
file(WRITE "${initial}/console.log" "")
file(WRITE "${initial}/openttd.cfg"
    "[misc]\nlanguage = english.lng\n[gui]\nautosave = off\nautosave_on_exit = false\nthreaded_saves = false\n")
file(WRITE "${initial}/scripts/game_start.scr"
    "script \"${initial}/console.log\"\ngetfulldate\ndump_veh_stats\nsave \"${initial}/current\"\nunpause\nquit\n")
execute_process(COMMAND "${OPENTTD_EXECUTABLE}" -x ${scan_options} -c "${initial}/openttd.cfg" -g "${LEGACY_SAVE}"
    -vnull:ticks=10 -snull -mnull -d 0 WORKING_DIRECTORY "${initial}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics TIMEOUT 30)
file(WRITE "${initial}/process.log" "${output}\n${diagnostics}")
if(NOT result EQUAL 0 OR NOT EXISTS "${initial}/current.sav" OR diagnostics MATCHES "not found; checksum|Removing [0-9]+ vehicles found to be uncorrectably invalid")
    message(FATAL_ERROR "Shadow preflight migration failed: ${output} ${diagnostics}")
endif()
set(input_save "${initial}/current.sav")
file(READ "${initial}/console.log" initial_console)
if(initial_console MATCHES "Game is in error state" AND NOT ALLOW_ERROR_PAUSE)
    message(FATAL_ERROR "Shadow comparison cannot advance this save: ${initial_console}")
endif()

foreach(mode IN ITEMS off on)
    set(run "${WORK_DIR}/${mode}")
    file(MAKE_DIRECTORY "${run}/scripts")
    prepare_newgrfs("${run}")
    file(WRITE "${run}/console.log" "")
    file(WRITE "${run}/openttd.cfg"
        "[misc]\nlanguage = english.lng\n[gui]\nautosave = off\nautosave_on_exit = false\nthreaded_saves = false\n")
    set(next_script "finish")
    math(EXPR intermediate_months "${MONTHS} - 1")
    while(intermediate_months GREATER 0)
        file(WRITE "${run}/scripts/month${intermediate_months}.scr"
            "schedule on-next-calendar-month \"${run}/scripts/${next_script}.scr\"\n")
        set(next_script "month${intermediate_months}")
        math(EXPR intermediate_months "${intermediate_months} - 1")
    endwhile()
    file(WRITE "${run}/scripts/game_start.scr"
        "script \"${run}/console.log\"\ngetfulldate\nsetting linkgraph.distribution_pax 2\nsetting linkgraph.passenger_od_assignment true\npassenger_journey_shadow ${mode}\nunpause\nschedule on-next-calendar-month \"${run}/scripts/${next_script}.scr\"\n")
    if(ALLOW_ERROR_PAUSE)
        # This only asks the game's own confirmation; it does not bypass it.
        # A human or UI automation must acknowledge the warning in win32 mode.
        file(APPEND "${run}/scripts/game_start.scr" "step 0\n")
    endif()
    file(WRITE "${run}/scripts/finish.scr"
        "getfulldate\ndump_veh_stats\npassenger_journey_shadow stats\npassenger_journey_shadow details\npassenger_journey_shadow inventory\nsave \"${run}/result\"\nquit\n")
    string(TIMESTAMP started "%s" UTC)
    if(WIN32 AND VIDEO_DRIVER STREQUAL "win32")
        # Captured children inherit hidden startup windows. Start an explicitly
        # visible game only for this requested interactive confirmation mode.
        string(REPLACE "'" "''" quoted_binary "${OPENTTD_EXECUTABLE}")
        string(REPLACE "'" "''" quoted_run "${run}")
        string(REPLACE "'" "''" quoted_save "${input_save}")
        set(launch_command "$gameProcess = Start-Process -FilePath '${quoted_binary}' -ArgumentList @('-x', '-c', '\"${quoted_run}/openttd.cfg\"', '-g', '\"${quoted_save}\"', '-vwin32', '-snull', '-mnull') -WorkingDirectory '${quoted_run}' -WindowStyle Normal -Wait -PassThru -RedirectStandardOutput '${quoted_run}/stdout.log' -RedirectStandardError '${quoted_run}/stderr.log'; exit $gameProcess.ExitCode")
        execute_process(COMMAND powershell -NoProfile -NonInteractive -Command "${launch_command}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics TIMEOUT ${RUN_TIMEOUT})
        if(EXISTS "${run}/stderr.log")
            file(READ "${run}/stderr.log" game_diagnostics)
            string(APPEND diagnostics "\n${game_diagnostics}")
        endif()
    else()
        execute_process(COMMAND "${OPENTTD_EXECUTABLE}" -x ${scan_options} -c "${run}/openttd.cfg" -g "${input_save}"
            "-v${VIDEO_DRIVER}" -snull -mnull -d 0 WORKING_DIRECTORY "${run}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics TIMEOUT ${RUN_TIMEOUT})
    endif()
    string(TIMESTAMP finished "%s" UTC)
    math(EXPR elapsed "${finished} - ${started}")
    file(WRITE "${run}/wall-seconds.txt" "${elapsed}\n")
    file(WRITE "${run}/process.log" "${output}\n${diagnostics}")
    if(NOT result EQUAL 0 OR NOT EXISTS "${run}/result.sav")
        message(FATAL_ERROR "Shadow ${mode}: production run failed (${result}): ${output} ${diagnostics}")
    endif()
endforeach()

file(SHA256 "${WORK_DIR}/off/result.sav" off_hash)
file(SHA256 "${WORK_DIR}/on/result.sav" on_hash)
if(NOT off_hash STREQUAL on_hash)
    message(FATAL_ERROR "Shadow diagnostics changed the production save: off=${off_hash} on=${on_hash}")
endif()

# A new process loading the shadow-enabled run still starts with empty, disabled diagnostics.
set(reload "${WORK_DIR}/reload")
file(MAKE_DIRECTORY "${reload}/scripts")
prepare_newgrfs("${reload}")
configure_file("${WORK_DIR}/off/openttd.cfg" "${reload}/openttd.cfg" COPYONLY)
file(WRITE "${reload}/console.log" "")
file(WRITE "${reload}/scripts/game_start.scr"
    "script \"${reload}/console.log\"\npassenger_journey_shadow stats\nquit\n")
execute_process(COMMAND "${OPENTTD_EXECUTABLE}" -x ${scan_options} -c "${reload}/openttd.cfg"
    -g "${WORK_DIR}/on/result.sav" -vnull:ticks=10 -snull -mnull -d 0 WORKING_DIRECTORY "${reload}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE diagnostics TIMEOUT 30)
file(WRITE "${reload}/process.log" "${output}\n${diagnostics}")
file(READ "${reload}/console.log" console)
if(NOT result EQUAL 0 OR NOT console MATCHES "Passenger journey shadow: off; observations=0 people/0 packets")
    message(FATAL_ERROR "Shadow state/statistics leaked into save/load: ${console} ${diagnostics}")
endif()
message(STATUS "Shadow off/on full production saves identical: ${on_hash}; reload diagnostics disabled and empty")
