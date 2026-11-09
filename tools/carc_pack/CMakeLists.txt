# carc_pack - standalone CARC packing tool
cmake_minimum_required(VERSION 3.25)

set(TOOL_NAME carc_pack)

add_executable(${TOOL_NAME} main.cpp ExtractionOutput.cpp)
target_link_libraries(${TOOL_NAME} PRIVATE
    Caesura::Archive
    Caesura::Debug
    Caesura::BuildOptions
)

if(MSVC)
    set_target_properties(${TOOL_NAME} PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
endif()

if(MINGW)
    target_link_options(${TOOL_NAME} PRIVATE -municode)
endif()
if(WIN32)
    target_link_libraries(${TOOL_NAME} PRIVATE advapi32)
endif()

set_target_properties(${TOOL_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
)
