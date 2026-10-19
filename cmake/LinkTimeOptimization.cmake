function(configure_lto)
    if(OPTION_NO_LTO)
        return()
    endif()
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # VS maps CMake's IPO property to WholeProgramOptimization; ClangCL does
        # not reliably translate that into ThinLTO. Specify it on every TU.
        set(targets ${OPENTTD_LIB} openttd)
        if(TARGET openttd_test)
            list(APPEND targets openttd_test)
        endif()
        list(REMOVE_DUPLICATES targets)
        foreach(target IN LISTS targets)
            set_target_properties(${target} PROPERTIES
                INTERPROCEDURAL_OPTIMIZATION_RELEASE FALSE
                INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL FALSE
                INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO FALSE)
            target_compile_options(${target} PRIVATE
                "$<$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>,$<CONFIG:RelWithDebInfo>>:-flto=thin>")
        endforeach()
        # LLD identifies ThinLTO from the bitcode summaries; do not pass Clang
        # driver flags to lld-link itself.
    elseif(IPO_FOUND)
        set_target_properties(openttd PROPERTIES
            INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
            INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL TRUE
            INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    endif()
endfunction()
