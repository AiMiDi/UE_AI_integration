if(
    NOT DEFINED UE_WORKFLOW_EXECUTABLE
    OR NOT DEFINED UE_SHORT_CLI_EXECUTABLE
    OR NOT DEFINED UE_WORKFLOW_SOURCE_ROOT
    OR NOT DEFINED UE_WORKFLOW_PACKAGE_ROOT
)
    message(FATAL_ERROR "Packaged layout smoke requires executable, source root, and package root")
endif()

string(FIND "${UE_WORKFLOW_PACKAGE_ROOT}" "packaged-layout-smoke" safe_root_index)
if(safe_root_index EQUAL -1)
    message(FATAL_ERROR "Refusing to replace unexpected package root: ${UE_WORKFLOW_PACKAGE_ROOT}")
endif()

file(REMOVE_RECURSE "${UE_WORKFLOW_PACKAGE_ROOT}")
file(MAKE_DIRECTORY "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin")
file(COPY "${UE_WORKFLOW_EXECUTABLE}" DESTINATION "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin")
file(COPY "${UE_SHORT_CLI_EXECUTABLE}" DESTINATION "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin")
file(
    COPY "${UE_WORKFLOW_SOURCE_ROOT}/Workflow/Contracts"
    DESTINATION "${UE_WORKFLOW_PACKAGE_ROOT}/Workflow"
)
file(
    COPY "${UE_WORKFLOW_SOURCE_ROOT}/Resources/Capabilities"
    DESTINATION "${UE_WORKFLOW_PACKAGE_ROOT}/Resources"
)
file(
    COPY "${UE_WORKFLOW_SOURCE_ROOT}/skills"
    DESTINATION "${UE_WORKFLOW_PACKAGE_ROOT}"
)

get_filename_component(executable_name "${UE_WORKFLOW_EXECUTABLE}" NAME)
set(packaged_executable "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin/${executable_name}")
execute_process(
    COMMAND "${packaged_executable}" --json doctor
    WORKING_DIRECTORY "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Packaged CLI doctor failed: ${result}\n${output}\n${error}")
endif()

file(TO_CMAKE_PATH "${UE_WORKFLOW_PACKAGE_ROOT}" normalized_package_root)
string(
    FIND
    "${output}"
    "${normalized_package_root}/CLI/bin/../../Workflow/Contracts"
    contract_index
)
if(contract_index EQUAL -1)
    message(FATAL_ERROR "Packaged CLI did not resolve plugin-root contracts: ${output}")
endif()
get_filename_component(short_executable_name "${UE_SHORT_CLI_EXECUTABLE}" NAME)
set(packaged_short_executable
    "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin/${short_executable_name}"
)
execute_process(
    COMMAND "${packaged_short_executable}" --json --version
    RESULT_VARIABLE short_result
    OUTPUT_VARIABLE short_output
    ERROR_VARIABLE short_error
)
if(NOT short_result EQUAL 0)
    message(FATAL_ERROR "Packaged ue-cli version failed: ${short_output}${short_error}")
endif()
string(FIND "${short_output}" "\"executable\":\"ue-cli\"" short_index)
if(short_index EQUAL -1)
    message(FATAL_ERROR "Packaged short-operation CLI is invalid: ${short_output}")
endif()

execute_process(
    COMMAND "${packaged_short_executable}" capabilities --json --limit 1
    WORKING_DIRECTORY "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin"
    RESULT_VARIABLE catalog_result
    OUTPUT_VARIABLE catalog_output
    ERROR_VARIABLE catalog_error
)
if(NOT catalog_result EQUAL 0)
    message(FATAL_ERROR "Packaged ue-cli catalog failed: ${catalog_output}${catalog_error}")
endif()
string(FIND "${catalog_output}" "\"source\":\"local\"" local_source_index)
string(FIND "${catalog_output}" "\"total\":" total_index)
string(FIND "${catalog_output}" "${normalized_package_root}/Resources/Capabilities" root_index)
if(
    local_source_index EQUAL -1
    OR total_index EQUAL -1
    OR root_index EQUAL -1
)
    message(FATAL_ERROR "Packaged ue-cli did not resolve its local catalog: ${catalog_output}")
endif()

execute_process(
    COMMAND "${packaged_short_executable}" skills --json --limit 1
    WORKING_DIRECTORY "${UE_WORKFLOW_PACKAGE_ROOT}/CLI/bin"
    RESULT_VARIABLE skills_result
    OUTPUT_VARIABLE skills_output
    ERROR_VARIABLE skills_error
)
if(NOT skills_result EQUAL 0)
    message(FATAL_ERROR "Packaged ue-cli skills failed: ${skills_output}${skills_error}")
endif()
string(FIND "${skills_output}" "\"source\":\"local\"" skills_source_index)
string(FIND "${skills_output}" "\"total\":" skills_total_index)
string(FIND "${skills_output}" "${normalized_package_root}/skills" skills_root_index)
if(
    skills_source_index EQUAL -1
    OR skills_total_index EQUAL -1
    OR skills_root_index EQUAL -1
)
    message(FATAL_ERROR "Packaged ue-cli did not resolve Agent Skills: ${skills_output}")
endif()
