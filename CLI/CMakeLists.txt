add_executable(ue-workflow
    src/main.cpp
)

target_compile_features(ue-workflow PRIVATE cxx_std_20)
target_link_libraries(ue-workflow PRIVATE UEWorkflow::Core)
target_include_directories(ue-workflow PRIVATE
    "${CMAKE_SOURCE_DIR}/Workflow/ThirdParty"
)
target_compile_definitions(ue-workflow PRIVATE
    UE_WORKFLOW_VERSION="${PROJECT_VERSION}"
    UE_WORKFLOW_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)

install(
    TARGETS ue-workflow
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(WIN32)
    target_link_libraries(ue-workflow PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(ue-workflow PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(ue-workflow PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(UE_WORKFLOW_BUILD_TESTS AND BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    add_test(
        NAME ue_workflow_cli_help
        COMMAND ue-workflow --json --help
    )
    add_test(
        NAME ue_workflow_cli_doctor
        COMMAND ue-workflow
            --json
            --contract-root "${CMAKE_SOURCE_DIR}/Workflow/Contracts"
            --capability-root "${CMAKE_SOURCE_DIR}/Resources/Capabilities"
            doctor
    )
    add_test(
        NAME ue_workflow_cli_capabilities
        COMMAND ue-workflow
            --json
            --contract-root "${CMAKE_SOURCE_DIR}/Workflow/Contracts"
            --capability-root "${CMAKE_SOURCE_DIR}/Resources/Capabilities"
            capabilities
            --query widget
            --domain content
            --limit 5
    )
    set_tests_properties(
        ue_workflow_cli_capabilities
        PROPERTIES
            PASS_REGULAR_EXPRESSION
                "\"limit\":5.*\"schema\":\"ue.workflow-capabilities.v1\""
    )
    add_test(
        NAME ue_workflow_cli_plan
        COMMAND ue-workflow
            --json
            --contract-root "${CMAKE_SOURCE_DIR}/Workflow/Contracts"
            --capability-root "${CMAKE_SOURCE_DIR}/Resources/Capabilities"
            plan
            --file "${CMAKE_SOURCE_DIR}/Workflow/tests/fixtures/widget.workflow.json"
    )
    add_test(
        NAME ue_workflow_cli_approval_gate
        COMMAND "${CMAKE_COMMAND}"
            "-DUE_WORKFLOW_EXECUTABLE=$<TARGET_FILE:ue-workflow>"
            "-DUE_WORKFLOW_CONTRACT_ROOT=${CMAKE_SOURCE_DIR}/Workflow/Contracts"
            "-DUE_WORKFLOW_CAPABILITY_ROOT=${CMAKE_SOURCE_DIR}/Resources/Capabilities"
            "-DUE_WORKFLOW_FIXTURE=${CMAKE_SOURCE_DIR}/Workflow/tests/fixtures/widget.workflow.json"
            "-DUE_WORKFLOW_RECEIPT=${CMAKE_BINARY_DIR}/approval-smoke-receipt.json"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ApprovalSmoke.cmake"
    )
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME ue_workflow_cli_http_integration
            COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_http_integration.py"
                --cli "$<TARGET_FILE:ue-workflow>"
                --contract-root "${CMAKE_SOURCE_DIR}/Workflow/Contracts"
                --capability-root "${CMAKE_SOURCE_DIR}/Resources/Capabilities"
                --fixture "${CMAKE_SOURCE_DIR}/Workflow/tests/fixtures/widget.workflow.json"
        )
    endif()
    add_test(
        NAME ue_workflow_cli_install
        COMMAND "${CMAKE_COMMAND}"
            --install "${CMAKE_BINARY_DIR}"
            --prefix "${CMAKE_BINARY_DIR}/install-smoke"
            --config $<CONFIG>
    )
    add_test(
        NAME ue_workflow_cli_installed_doctor
        COMMAND "${CMAKE_COMMAND}"
            "-DUE_WORKFLOW_EXECUTABLE=${CMAKE_BINARY_DIR}/install-smoke/${CMAKE_INSTALL_BINDIR}/ue-workflow${CMAKE_EXECUTABLE_SUFFIX}"
            "-DUE_WORKFLOW_INSTALL_ROOT=${CMAKE_BINARY_DIR}/install-smoke"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/InstalledSmoke.cmake"
    )
    set_tests_properties(
        ue_workflow_cli_installed_doctor
        PROPERTIES DEPENDS ue_workflow_cli_install
    )
endif()
