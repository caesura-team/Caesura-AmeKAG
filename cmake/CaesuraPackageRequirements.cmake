# Build-side input for final-package verification. Deliberately NOT installed:
# a package cannot lower its own mandatory feature/runtime requirements.
if(NOT CAESURA_CAPABILITY_PLATFORM MATCHES "^(windows|linux|macos)$")
    return() # Mobile bundle identities have their own validation lanes.
endif()

function(_caesura_package_json_quote output value)
    string(REPLACE "\\" "\\\\" _quoted "${value}")
    string(REPLACE "\"" "\\\"" _quoted "${_quoted}")
    string(REPLACE "\n" "\\n" _quoted "${_quoted}")
    string(REPLACE "\r" "\\r" _quoted "${_quoted}")
    string(REPLACE "\t" "\\t" _quoted "${_quoted}")
    set(${output} "\"${_quoted}\"" PARENT_SCOPE)
endfunction()

function(_caesura_package_json_array output)
    set(_items "")
    foreach(_item IN LISTS ARGN)
        _caesura_package_json_quote(_encoded "${_item}")
        list(APPEND _items "${_encoded}")
    endforeach()
    list(JOIN _items ", " _joined)
    set(${output} "[${_joined}]" PARENT_SCOPE)
endfunction()

get_target_property(_package_sdl_type SDL3::SDL3 TYPE)
if(_package_sdl_type STREQUAL "STATIC_LIBRARY")
    set(_package_sdl_linkage "static")
    set(_package_sdl_libraries "[]")
elseif(_package_sdl_type STREQUAL "SHARED_LIBRARY")
    set(_package_sdl_linkage "shared")
    if(WIN32)
        set(_package_sdl_name "$<TARGET_FILE_NAME:SDL3::SDL3>")
    else()
        set(_package_sdl_name "$<TARGET_SONAME_FILE_NAME:SDL3::SDL3>")
    endif()
    _caesura_package_json_array(_package_sdl_libraries "${_package_sdl_name}")
else()
    # Keep unsupported target shapes visible for a required package check.
    set(_package_sdl_linkage "unspecified")
    set(_package_sdl_libraries "[]")
endif()

set(_package_sdk_config "")
if(NOT WIN32)
    foreach(_sdk FFMPEG STEAM)
        if(CAESURA_CAPABILITY_${_sdk}_JSON)
            set(CAESURA_PACKAGE_${_sdk}_LINKAGE "unspecified" CACHE STRING
                "Final package ${_sdk} linkage: shared or static; required when enabled")
            set_property(CACHE CAESURA_PACKAGE_${_sdk}_LINKAGE PROPERTY STRINGS unspecified shared static)
            set(CAESURA_PACKAGE_${_sdk}_LIBRARIES "" CACHE STRING
                "Exact package-relative ${_sdk} runtime library paths for shared linkage")
            string(TOLOWER "${_sdk}" _sdk_key)
            _caesura_package_json_quote(_linkage "${CAESURA_PACKAGE_${_sdk}_LINKAGE}")
            _caesura_package_json_array(_libraries ${CAESURA_PACKAGE_${_sdk}_LIBRARIES})
            string(APPEND _package_sdk_config
                ",\n    \"${_sdk_key}_linkage\": ${_linkage},\n    \"${_sdk_key}_libraries\": ${_libraries}")
        endif()
    endforeach()
endif()

# OpenSSL's imported targets are commonly UNKNOWN, so target TYPE alone cannot
# declare static linkage. Read CMake's selected imported LOCATION_<CONFIG>,
# including MAP_IMPORTED_CONFIG, then inspect those exact ordinary file bytes.
# This selection table is build evidence only; installation/signing is separate.
set(_package_openssl_config "")
if(CAESURA_CAPABILITY_PLATFORM STREQUAL "macos")
    if(NOT APPLE OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
        message(FATAL_ERROR "macOS package requirements need desktop Apple selection")
    endif()
    foreach(_component SSL Crypto)
        if(NOT TARGET OpenSSL::${_component})
            message(FATAL_ERROR "Required OpenSSL component target is missing: ${_component}")
        endif()
        get_target_property(_imported OpenSSL::${_component} IMPORTED)
        get_target_property(_type OpenSSL::${_component} TYPE)
        if(NOT _imported OR NOT _type MATCHES "^(UNKNOWN_LIBRARY|STATIC_LIBRARY|SHARED_LIBRARY)$")
            message(FATAL_ERROR "OpenSSL selection requires an imported library target: ${_component}")
        endif()
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_selector "${CMAKE_CURRENT_LIST_DIR}/../scripts/macos_runtime_selection.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_selector}" "${CMAKE_CURRENT_LIST_DIR}/../scripts/macho_dependencies.py")
    if(CMAKE_CONFIGURATION_TYPES)
        set(_openssl_configs ${CMAKE_CONFIGURATION_TYPES})
    elseif(DEFINED CMAKE_BUILD_TYPE AND NOT "${CMAKE_BUILD_TYPE}" STREQUAL "")
        set(_openssl_configs "${CMAKE_BUILD_TYPE}")
    else()
        message(FATAL_ERROR "OpenSSL package selection requires an explicit build configuration")
    endif()
    set(_selected_configs "")
    foreach(_config IN LISTS _openssl_configs)
        if(NOT _config MATCHES "^[A-Za-z0-9][A-Za-z0-9._+-]*$")
            message(FATAL_ERROR "Unsafe OpenSSL configuration name: ${_config}")
        endif()
        string(TOUPPER "${_config}" _upper_config)
        if(_upper_config IN_LIST _selected_configs)
            message(FATAL_ERROR "Ambiguous duplicate OpenSSL configuration: ${_config}")
        endif()
        list(APPEND _selected_configs "${_upper_config}")
        get_target_property(_ssl_file OpenSSL::SSL LOCATION_${_upper_config})
        get_target_property(_crypto_file OpenSSL::Crypto LOCATION_${_upper_config})
        if(NOT _ssl_file OR NOT _crypto_file)
            message(FATAL_ERROR "OpenSSL location is missing for ${_config}")
        endif()
        set(_selection_base "${CMAKE_BINARY_DIR}/openssl-selection-${_config}")
        # Invalidate a prior table before the new attempt; failed configure must
        # never leave an older successful selection looking like this attempt.
        file(WRITE "${_selection_base}.json" "{\"status\":\"NOT_SELECTED\"}\n")
        execute_process(COMMAND "${Python3_EXECUTABLE}" -B "${_selector}"
            --ssl-file "${_ssl_file}" --crypto-file "${_crypto_file}" --configuration "${_config}"
            RESULT_VARIABLE _selection_exit OUTPUT_VARIABLE _selection_json ERROR_VARIABLE _selection_error
            TIMEOUT 60)
        file(WRITE "${_selection_base}.stdout.log" "${_selection_json}")
        file(WRITE "${_selection_base}.stderr.log" "${_selection_error}")
        if(NOT "${_selection_exit}" STREQUAL "0")
            message(FATAL_ERROR "OpenSSL ${_config} selection failed (${_selection_exit}): ${_selection_error}")
        endif()
        string(JSON _linkage GET "${_selection_json}" linkage)
        string(JSON _libraries GET "${_selection_json}" libraries)
        # Check known STATIC/SHARED declarations too; UNKNOWN is classified by
        # the selector. Do not make an explicit target-type contradiction true.
        foreach(_component SSL Crypto)
            get_target_property(_type OpenSSL::${_component} TYPE)
            if((_type STREQUAL "STATIC_LIBRARY" AND NOT _linkage STREQUAL "static")
               OR (_type STREQUAL "SHARED_LIBRARY" AND NOT _linkage STREQUAL "shared"))
                message(FATAL_ERROR "OpenSSL ${_component} target type contradicts selected bytes")
            endif()
        endforeach()
        file(WRITE "${_selection_base}.json" "${_selection_json}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ssl_file}" "${_crypto_file}")
        foreach(_index 0 1)
            string(JSON _resolved GET "${_selection_json}" components ${_index} resolved_path)
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_resolved}")
        endforeach()
        # Values here are a checked plain linkage and checked literal names;
        # only the generated requirements field fragment varies by config.
        string(APPEND _package_openssl_config
            "$<$<CONFIG:${_config}>:,\n    \"openssl_linkage\": \"${_linkage}\",\n    \"openssl_libraries\": ${_libraries}>")
    endforeach()
endif()

_caesura_package_json_quote(_package_stem "${CPACK_PACKAGE_FILE_NAME}")
_caesura_package_json_quote(_package_version "${PROJECT_VERSION}")
foreach(_format zip tgz dmg appimage)
    set(_extension "${_format}")
    if(_format STREQUAL "tgz")
        set(_extension "tar.gz")
    elseif(_format STREQUAL "appimage")
        set(_extension "AppImage")
    endif()
    _caesura_package_json_quote(_package_${_format} "${CPACK_PACKAGE_FILE_NAME}.${_extension}")
endforeach()

file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/package-requirements-$<CONFIG>.json" CONTENT
"{
  \"schema\": \"caesura.package-build.v1\",
  \"platform\": \"${CAESURA_CAPABILITY_PLATFORM}\",
  \"configuration\": \"$<CONFIG>\",
  \"version\": ${_package_version},
  \"archive_basename\": ${_package_stem},
  \"engine_relative_path\": \"$<TARGET_FILE_NAME:${PROJECT_NAME}>\",
  \"lua_relative_path\": \"external/lua/$<TARGET_FILE_NAME:lua_cli>\",
  \"artifacts\": {\"zip\": ${_package_zip}, \"tgz\": ${_package_tgz}, \"dmg\": ${_package_dmg}, \"appimage\": ${_package_appimage}},
  \"required_configuration\": {
    \"schema\": 1,
    \"sdl_linkage\": \"${_package_sdl_linkage}\",
    \"sdl_libraries\": ${_package_sdl_libraries},
    \"ffmpeg\": ${CAESURA_CAPABILITY_FFMPEG_JSON},
    \"steam\": ${CAESURA_CAPABILITY_STEAM_JSON},
    \"live2d\": ${CAESURA_CAPABILITY_LIVE2D_JSON}${_package_sdk_config}${_package_openssl_config}
  }
}
")
