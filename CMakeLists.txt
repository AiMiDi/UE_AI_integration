cmake_minimum_required(VERSION 3.24)

project(
    UEWorkflow
    VERSION 1.0.0
    DESCRIPTION "Portable UE Workflow DSL and short-operation CLIs"
    LANGUAGES CXX
)

option(UE_WORKFLOW_BUILD_TESTS "Build UE Workflow tests" ON)
option(UE_WORKFLOW_BUILD_CLI "Build the ue and ue-workflow executables" ON)

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
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Contracts/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow/Contracts/Engineering"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/skills/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow/Skills"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Recipes/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow/Recipes"
)
