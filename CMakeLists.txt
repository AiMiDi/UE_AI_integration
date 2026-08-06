cmake_minimum_required(VERSION 3.24)

project(
    UEWorkflow
    VERSION 1.0.0
    DESCRIPTION "Portable UE Workflow DSL and short-operation CLIs"
    LANGUAGES CXX
)

option(UE_WORKFLOW_BUILD_TESTS "Build UE Workflow tests" ON)
option(UE_WORKFLOW_BUILD_CLI "Build the ue-cli and ue-workflow-cli executables" ON)

set(UE_SOURCE_REVISION "$ENV{UEAI_SOURCE_REVISION}")
if(UE_SOURCE_REVISION STREQUAL "")
    execute_process(
        COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --short=12 HEAD
        OUTPUT_VARIABLE UE_SOURCE_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE UE_SOURCE_REVISION_RESULT
        TIMEOUT 3
    )
    if(NOT UE_SOURCE_REVISION_RESULT EQUAL 0)
        set(UE_SOURCE_REVISION "unknown")
    endif()
endif()
string(REGEX REPLACE "[^0-9A-Za-z._-]" "_" UE_SOURCE_REVISION "${UE_SOURCE_REVISION}")
if(UE_SOURCE_REVISION STREQUAL "")
    set(UE_SOURCE_REVISION "unknown")
endif()

include(CTest)
include(GNUInstallDirs)

add_subdirectory(Workflow)
if(UE_WORKFLOW_BUILD_CLI)
    add_subdirectory(CLI)
endif()

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Capabilities/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow-cli/Capabilities"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Contracts/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow-cli/Contracts/Engineering"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/skills/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow-cli/Skills"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Recipes/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow-cli/Recipes"
)
