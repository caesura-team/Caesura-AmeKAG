# One mandatory finalization transaction after Engine/SDL installation.
# No code is emitted for iOS or non-Apple targets. Both OpenSSL linkage kinds
# run this coordinator. Diagnostic requests are created only by Python.
function(caesura_install_macos_runtime target)
    if(NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        return()
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Mac install coordinator requires its Engine target")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_selection_hash "")
    if(CMAKE_CONFIGURATION_TYPES)
        set(_configs ${CMAKE_CONFIGURATION_TYPES})
    elseif(CMAKE_BUILD_TYPE)
        set(_configs "${CMAKE_BUILD_TYPE}")
    else()
        message(FATAL_ERROR "Mac install coordinator needs an explicit configuration")
    endif()
    foreach(_config IN LISTS _configs)
        if(NOT _config MATCHES "^[A-Za-z0-9][A-Za-z0-9._+-]*$")
            message(FATAL_ERROR "Unsafe Mac install configuration")
        endif()
        file(SHA256 "${CMAKE_BINARY_DIR}/openssl-selection-${_config}.json" _hash)
        string(APPEND _selection_hash "$<$<CONFIG:${_config}>:${_hash}>")
    endforeach()
    # Cache-provided Windows interpreter paths may retain backslashes; normalize
    # before embedding them in the generated CMake string.
    file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" _coordinator_python)
    set(_coordinator "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/install_macos_runtime.py")
    set(_code [=[
set(_physical_stage "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
execute_process(COMMAND "@_coordinator_python@" -B "@_coordinator@" --cmake-install
    --selection-file "@CMAKE_BINARY_DIR@/openssl-selection-$<CONFIG>.json"
    --selection-sha256 "@_selection_hash@"
    --requirements-file "@CMAKE_BINARY_DIR@/package-requirements-$<CONFIG>.json"
    --configuration "$<CONFIG>"
    --stage-root "${_physical_stage}"
    --build-engine "$<TARGET_FILE:@target@>"
    --report-parent "@CMAKE_BINARY_DIR@-macos-install-reports"
    RESULT_VARIABLE _runtime_exit TIMEOUT 600)
if(NOT "${_runtime_exit}" STREQUAL "0")
    message(FATAL_ERROR "Mac runtime install rejected (${_runtime_exit})")
endif()
]=])
    string(CONFIGURE "${_code}" _configured @ONLY)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/macos-runtime-install-$<CONFIG>.cmake"
         CONTENT "${_configured}")
    install(SCRIPT "${CMAKE_BINARY_DIR}/macos-runtime-install-$<CONFIG>.cmake")
endfunction()
