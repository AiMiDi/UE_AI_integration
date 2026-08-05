if(
    NOT DEFINED UE_WORKFLOW_EXECUTABLE
    OR NOT DEFINED UE_SHORT_CLI_EXECUTABLE
    OR NOT DEFINED UE_WORKFLOW_INSTALL_ROOT
)
    message(FATAL_ERROR "Installed smoke requires both executables and install root")
endif()

get_filename_component(
    installed_bin
    "${UE_SHORT_CLI_EXECUTABLE}"
    DIRECTORY
)
get_filename_component(
    short_cli_command
    "${UE_SHORT_CLI_EXECUTABLE}"
    NAME_WE
)
set(path_launch_directory
    "${UE_WORKFLOW_INSTALL_ROOT}/../installed-path-launch-smoke"
)
file(MAKE_DIRECTORY "${path_launch_directory}")

execute_process(
    COMMAND "${UE_WORKFLOW_EXECUTABLE}" --json doctor
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error_output
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed ue-workflow-cli doctor failed: ${output}${error_output}")
endif()

file(TO_CMAKE_PATH "${UE_WORKFLOW_INSTALL_ROOT}" normalized_install_root)
string(FIND "${output}" "${normalized_install_root}/share/ue-workflow-cli/Contracts" contract_index)
string(FIND "${output}" "${normalized_install_root}/bin/../share/ue-workflow-cli/Contracts" relative_contract_index)
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
    message(FATAL_ERROR "Installed ue-cli version failed: ${short_output}${short_error}")
endif()
string(FIND "${short_output}" "\"executable\":\"ue-cli\"" short_executable_index)
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
    message(FATAL_ERROR "Installed ue-cli catalog failed: ${catalog_output}${catalog_error}")
endif()
string(FIND "${catalog_output}" "\"source\":\"local\"" local_source_index)
string(FIND "${catalog_output}" "\"total\":" total_index)
string(FIND "${catalog_output}" "share/ue-workflow-cli/Capabilities" root_index)
if(
    local_source_index EQUAL -1
    OR total_index EQUAL -1
    OR root_index EQUAL -1
)
    message(FATAL_ERROR "Installed ue-cli did not resolve its local catalog: ${catalog_output}")
endif()

execute_process(
    COMMAND "${UE_SHORT_CLI_EXECUTABLE}" skills --json --limit 1
    RESULT_VARIABLE skills_result
    OUTPUT_VARIABLE skills_output
    ERROR_VARIABLE skills_error
)
if(NOT skills_result EQUAL 0)
    message(FATAL_ERROR "Installed ue-cli skills failed: ${skills_output}${skills_error}")
endif()
string(FIND "${skills_output}" "\"source\":\"local\"" skills_source_index)
string(FIND "${skills_output}" "\"total\":" skills_total_index)
string(FIND "${skills_output}" "share/ue-workflow-cli/Skills" skills_root_index)
if(
    skills_source_index EQUAL -1
    OR skills_total_index EQUAL -1
    OR skills_root_index EQUAL -1
)
    message(FATAL_ERROR "Installed ue did not resolve Agent Skills: ${skills_output}")
endif()

if(WIN32)
    set(path_separator ";")
    set(path_shell cmd.exe /d /c)
else()
    set(path_separator ":")
    set(path_shell /bin/sh -c)
endif()
set(installed_path "${installed_bin}${path_separator}$ENV{PATH}")
if(WIN32)
    string(REPLACE ";" "\\;" installed_path "${installed_path}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "PATH=${installed_path}"
        ${path_shell}
        "${short_cli_command} capabilities --json --limit 1"
    WORKING_DIRECTORY "${path_launch_directory}"
    RESULT_VARIABLE path_catalog_result
    OUTPUT_VARIABLE path_catalog_output
    ERROR_VARIABLE path_catalog_error
)
if(NOT path_catalog_result EQUAL 0)
    message(FATAL_ERROR
        "PATH-launched ue-cli catalog failed: "
        "${path_catalog_output}${path_catalog_error}"
    )
endif()
string(FIND
    "${path_catalog_output}"
    "${normalized_install_root}/share/ue-workflow-cli/Capabilities"
    path_catalog_root_index
)
if(path_catalog_root_index EQUAL -1)
    message(FATAL_ERROR
        "PATH-launched ue-cli did not resolve installed capabilities: "
        "${path_catalog_output}"
    )
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env
        "PATH=${installed_path}"
        ${path_shell}
        "${short_cli_command} skills --json --limit 1"
    WORKING_DIRECTORY "${path_launch_directory}"
    RESULT_VARIABLE path_skills_result
    OUTPUT_VARIABLE path_skills_output
    ERROR_VARIABLE path_skills_error
)
if(NOT path_skills_result EQUAL 0)
    message(FATAL_ERROR
        "PATH-launched ue-cli skills failed: "
        "${path_skills_output}${path_skills_error}"
    )
endif()
string(FIND
    "${path_skills_output}"
    "${normalized_install_root}/share/ue-workflow-cli/Skills"
    path_skills_root_index
)
if(path_skills_root_index EQUAL -1)
    message(FATAL_ERROR
        "PATH-launched ue-cli did not resolve installed skills: "
        "${path_skills_output}"
    )
endif()
