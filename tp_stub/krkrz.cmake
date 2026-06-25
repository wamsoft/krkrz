# krkrz.cmake - 吉里吉里プラグイン用共通CMakeマクロ

cmake_minimum_required(VERSION 3.16)

set(TPSTUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tp_stub CACHE PATH "Path to tp_stub library")
set(NCBIND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ncbind CACHE PATH "Path to ncbind library")
set(SIMPLEBIND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../simplebinder CACHE PATH "Path to simplebinder library")

function(krkrz_plugin PROJECT_NAME)
    set(options STATIC NCBIND SIMPLEBIND)
    set(oneValueArgs VERSION)
    set(multiValueArgs SOURCES INCLUDES LIBRARIES DEFINITIONS)
    cmake_parse_arguments(KRKRZ "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    
    if(NOT KRKRZ_VERSION)
        set(KRKRZ_VERSION "1.00")
    endif()
    project(${PROJECT_NAME} VERSION ${KRKRZ_VERSION})

    if(TVP_STATIC_PLUGIN OR KRKRZ_STATIC)
        set(TVP_LIBRARY_TYPE STATIC)
        list(APPEND KRKRZ_INCLUDES ${TPSTUB_DIR})
    else()
        set(TVP_LIBRARY_TYPE SHARED)        
        list(APPEND KRKRZ_SOURCES ${TPSTUB_DIR}/tp_stub.cpp)
        list(APPEND KRKRZ_INCLUDES ${TPSTUB_DIR})
    endif()

    if(KRKRZ_NCBIND)
        list(APPEND KRKRZ_SOURCES ${NCBIND_DIR}/ncbind.cpp)
        list(APPEND KRKRZ_INCLUDES ${NCBIND_DIR})
    endif()

    if(KRKRZ_SIMPLEBIND)
        list(APPEND KRKRZ_SOURCES ${SIMPLEBIND_DIR}/v2link.cpp)
        list(APPEND KRKRZ_INCLUDES ${SIMPLEBIND_DIR})
    endif()
        
    add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:C>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/utf-8>")
    add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<COMPILE_LANGUAGE:CXX>>:/Zc:__cplusplus>")

    add_library(${PROJECT_NAME} ${TVP_LIBRARY_TYPE} ${KRKRZ_SOURCES})

    if (TVP_STATIC_PLUGIN OR KRKRZ_STATIC)
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            TVP_STATIC_PLUGIN
            TVP_PLUGIN_NAME=${PROJECT_NAME}
        )
        set_property(TARGET ${PROJECT_NAME} PROPERTY
            INTERFACE_LINK_OPTIONS
            $<$<CXX_COMPILER_ID:MSVC>:/WHOLEARCHIVE:$<TARGET_FILE:${PROJECT_NAME}>>
            $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:-Wl,--whole-archive,$<TARGET_FILE:${PROJECT_NAME}>,--no-whole-archive>
        )
    endif()

    if (KRKRZ_DEFINITIONS)
        target_compile_definitions(${PROJECT_NAME} PRIVATE ${KRKRZ_DEFINITIONS})
    endif()

    if(KRKRZ_INCLUDES)
        target_include_directories(${PROJECT_NAME} PRIVATE ${KRKRZ_INCLUDES})
    endif()

    if(KRKRZ_LIBRARIES)
        target_link_libraries(${PROJECT_NAME} PUBLIC ${KRKRZ_LIBRARIES})
    endif()

    # プラグインの挙動調整用
    if (KRKRZ_VARIANT STREQUAL "LIB" OR KRKRZ_VARIANT STREQUAL "SDL")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            __GENERIC__
        )
    endif()

    if (MSVC)
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            UNICODE
            _UNICODE
        )
    endif()

endfunction()
