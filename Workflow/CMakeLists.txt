add_library(UEWorkflowCore STATIC
    src/WorkflowCore.cpp
)

add_library(UEWorkflow::Core ALIAS UEWorkflowCore)

target_compile_features(UEWorkflowCore PUBLIC cxx_std_20)
target_include_directories(UEWorkflowCore
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
)

install(
    TARGETS UEWorkflowCore
    EXPORT UEWorkflowTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Contracts/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ue-workflow/Contracts"
)

if(MSVC)
    target_compile_options(UEWorkflowCore PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(UEWorkflowCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(UE_WORKFLOW_BUILD_TESTS AND BUILD_TESTING)
    add_executable(ue_workflow_core_tests
        tests/WorkflowCoreTests.cpp
    )
    target_link_libraries(ue_workflow_core_tests PRIVATE UEWorkflow::Core)
    target_compile_features(ue_workflow_core_tests PRIVATE cxx_std_20)
    target_include_directories(ue_workflow_core_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty"
    )
    target_compile_definitions(ue_workflow_core_tests PRIVATE
        UE_WORKFLOW_TEST_ROOT="${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME ue_workflow_core_tests COMMAND ue_workflow_core_tests)
endif()
