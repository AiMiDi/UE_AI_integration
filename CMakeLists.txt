cmake_minimum_required(VERSION 3.24)

project(
    UEWorkflow
    VERSION 0.3.1
    DESCRIPTION "Portable UE Workflow DSL core and CLI"
    LANGUAGES CXX
)

option(UE_WORKFLOW_BUILD_TESTS "Build UE Workflow tests" ON)
option(UE_WORKFLOW_BUILD_CLI "Build the ue-workflow executable" ON)

include(CTest)
include(GNUInstallDirs)

add_subdirectory(Workflow)
if(UE_WORKFLOW_BUILD_CLI)
    add_subdirectory(CLI)
endif()

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Capabilities/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow/Capabilities"
)
