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
    \"live2d\": ${CAESURA_CAPABILITY_LIVE2D_JSON}${_package_sdk_config}
  }
}
")
