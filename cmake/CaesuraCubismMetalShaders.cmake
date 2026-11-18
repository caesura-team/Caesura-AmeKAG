# Cubism 5 Metal selects fragment libraries by the color/alpha mode suffix.
# Keep the names and numeric shader definitions in the SDK's enum order.
function(caesura_compile_cubism_metallib source name)
    set(air "${cubism_metal_build}/Intermediates/${name}.air")
    set(metallib "${CUBISM_METALLIB_DIR}/${name}.metallib")
    add_custom_command(
        OUTPUT "${metallib}"
        BYPRODUCTS "${air}"
        COMMAND xcrun -sdk macosx metal ${ARGN}
            -c "${cubism_metal_source}/${source}.metal" -o "${air}"
        COMMAND xcrun -sdk macosx metallib "${air}" -o "${metallib}"
        DEPENDS "${cubism_metal_source}/${source}.metal" ${cubism_metal_includes}
        VERBATIM
    )
    set(CUBISM_METALLIB_FILES ${CUBISM_METALLIB_FILES} "${metallib}" PARENT_SCOPE)
endfunction()

function(caesura_build_cubism_metallibs target shader_dir)
    set(cubism_metal_source "${shader_dir}")
    set(cubism_metal_build "${CMAKE_CURRENT_BINARY_DIR}/cubism-metal")
    set(CUBISM_METALLIB_DIR "${cubism_metal_build}/FrameworkMetallibs")
    file(MAKE_DIRECTORY "${cubism_metal_build}/Intermediates" "${CUBISM_METALLIB_DIR}")
    file(GLOB cubism_metal_includes "${cubism_metal_source}/*.h")
    list(APPEND cubism_metal_includes
        "${cubism_metal_source}/FragShaderSrcAlphaBlend.metal"
        "${cubism_metal_source}/FragShaderSrcColorBlend.metal")

    set(contract "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripts/cubism_metal_shaders.txt")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${contract}")
    file(STRINGS "${contract}" cubism_shader_groups)
    list(GET cubism_shader_groups 0 base)
    list(GET cubism_shader_groups 1 fragments)
    list(GET cubism_shader_groups 2 colors)
    list(GET cubism_shader_groups 3 alphas)
    foreach(group IN ITEMS base fragments colors alphas)
        string(REPLACE " " ";" ${group} "${${group}}")
    endforeach()

    set(CUBISM_METALLIB_FILES)
    foreach(shader IN LISTS base)
        caesura_compile_cubism_metallib("${shader}" "${shader}")
    endforeach()

    foreach(shader IN LISTS fragments)
        foreach(color IN LISTS colors)
            list(FIND colors "${color}" color_index)
            foreach(alpha IN LISTS alphas)
                if(color STREQUAL "Normal" AND alpha STREQUAL "Over")
                    continue()
                endif()
                list(FIND alphas "${alpha}" alpha_index)
                caesura_compile_cubism_metallib("${shader}" "${shader}${color}${alpha}"
                    "-DCSM_COLOR_BLEND_MODE=${color_index}" "-DCSM_ALPHA_BLEND_MODE=${alpha_index}")
            endforeach()
        endforeach()
    endforeach()

    set(resource_dir "$<TARGET_FILE_DIR:${target}>/FrameworkMetallibs")
    add_custom_target(CaesuraCubismMetallibs
        COMMAND ${CMAKE_COMMAND} -E make_directory "${resource_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CUBISM_METALLIB_FILES} "${resource_dir}"
        DEPENDS ${CUBISM_METALLIB_FILES}
        VERBATIM)
    add_dependencies("${target}" CaesuraCubismMetallibs)
    set(CUBISM_METALLIB_FILES "${CUBISM_METALLIB_FILES}" PARENT_SCOPE)
endfunction()
