if(
    NOT DEFINED UE_WORKFLOW_EXECUTABLE
    OR NOT DEFINED UE_SHORT_CLI_EXECUTABLE
    OR NOT DEFINED UE_WORKFLOW_INSTALL_ROOT
)
    message(FATAL_ERROR "Installed smoke requires both executables and install root")
endif()

execute_process(
    COMMAND "${UE_WORKFLOW_EXECUTABLE}" --json doctor
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed ue-workflow doctor failed: ${output}${error_output}")
endif()

file(TO_CMAKE_PATH "${UE_WORKFLOW_INSTALL_ROOT}" normalized_install_root)
string(FIND "${output}" "${normalized_install_root}/share/ue-workflow/Contracts" contract_index)
string(FIND "${output}" "${normalized_install_root}/bin/../share/ue-workflow/Contracts" relative_contract_index)
if(contract_index EQUAL -1 AND relative_contract_index EQUAL -1)
    message(FATAL_ERROR "Installed CLI did not resolve relocatable share contracts: ${output}")
endif()

execute_process(
    COMMAND "${UE_SHORT_CLI_EXECUTABLE}" --json --version
    RESULT_VARIABLE short_result
    OUTPUT_VARIABLE short_output
    ERROR_VARIABLE short_error
)
if(NOT short_result EQUAL 0)
    message(FATAL_ERROR "Installed ue version failed: ${short_output}${short_error}")
endif()
string(FIND "${short_output}" "\"executable\":\"ue\"" short_executable_index)
if(short_executable_index EQUAL -1)
    message(FATAL_ERROR "Installed short-operation CLI is invalid: ${short_output}")
endif()

execute_process(
    COMMAND "${UE_SHORT_CLI_EXECUTABLE}" capabilities --json --limit 1
    RESULT_VARIABLE catalog_result
    OUTPUT_VARIABLE catalog_output
    ERROR_VARIABLE catalog_error
)
if(NOT catalog_result EQUAL 0)
    message(FATAL_ERROR "Installed ue catalog failed: ${catalog_output}${catalog_error}")
endif()
string(FIND "${catalog_output}" "\"source\":\"local\"" local_source_index)
string(FIND "${catalog_output}" "\"total\":" total_index)
string(FIND "${catalog_output}" "share/ue-workflow/Capabilities" root_index)
if(
    local_source_index EQUAL -1
    OR total_index EQUAL -1
    OR root_index EQUAL -1
)
    message(FATAL_ERROR "Installed ue did not resolve its local catalog: ${catalog_output}")
endif()
