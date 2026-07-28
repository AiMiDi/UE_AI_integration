if(NOT DEFINED UE_WORKFLOW_EXECUTABLE OR NOT DEFINED UE_WORKFLOW_INSTALL_ROOT)
    message(FATAL_ERROR "Installed smoke requires executable and install root")
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
