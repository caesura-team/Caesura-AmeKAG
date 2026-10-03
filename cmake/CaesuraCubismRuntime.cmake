include_guard(GLOBAL)

# Cubism ships separate static archives for each MSVC CRT. Read the actual
# Framework target property; do not change the CRT of the application or SDK.
function(caesura_add_windows_cubism_core target core_dir runtime_target)
    if(NOT WIN32 OR NOT MSVC)
        message(FATAL_ERROR "Windows Cubism Core selection requires MSVC")
    endif()
    if(NOT TARGET "${runtime_target}")
        message(FATAL_ERROR "Cubism CRT selection requires an existing runtime target")
    endif()

    get_property(_runtime_set TARGET "${runtime_target}" PROPERTY MSVC_RUNTIME_LIBRARY SET)
    if(_runtime_set)
        get_target_property(_declared_runtime "${runtime_target}" MSVC_RUNTIME_LIBRARY)
        if(_declared_runtime STREQUAL "")
            message(FATAL_ERROR
                "Cubism Core needs an explicit MSVC_RUNTIME_LIBRARY when the property is set; an empty value leaves the compiler CRT unspecified")
        endif()
        set(_runtime "$<TARGET_GENEX_EVAL:${runtime_target},$<TARGET_PROPERTY:${runtime_target},MSVC_RUNTIME_LIBRARY>>")
    else()
        # This is CMake's documented default when the target property is unset.
        set(_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    # Keep the existing target name while selecting one archive after CMake
    # evaluates the Framework target's per-configuration CRT value.
    add_library("${target}" INTERFACE IMPORTED)
    set_target_properties("${target}" PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${core_dir}/include")
    foreach(_pair IN ITEMS
            "MultiThreadedDLL|MD" "MultiThreadedDebugDLL|MDd"
            "MultiThreaded|MT" "MultiThreadedDebug|MTd")
        string(REPLACE "|" ";" _parts "${_pair}")
        list(GET _parts 0 _value)
        list(GET _parts 1 _suffix)
        target_link_libraries("${target}" INTERFACE
            "$<$<STREQUAL:${_runtime},${_value}>:${core_dir}/lib/windows/x86_64/143/Live2DCubismCore_${_suffix}.lib>")
    endforeach()
endfunction()
