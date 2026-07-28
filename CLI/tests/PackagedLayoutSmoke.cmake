if(
    NOT DEFINED UE_WORKFLOW_EXECUTABLE
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
file(
    COPY "${UE_WORKFLOW_SOURCE_ROOT}/Workflow/Contracts"
    DESTINATION "${UE_WORKFLOW_PACKAGE_ROOT}/Workflow"
)
file(
    COPY "${UE_WORKFLOW_SOURCE_ROOT}/Resources/Capabilities"
    DESTINATION "${UE_WORKFLOW_PACKAGE_ROOT}/Resources"
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
string(FIND "${output}" "\"capabilityCount\":303" capability_index)
if(capability_index EQUAL -1)
    message(FATAL_ERROR "Packaged CLI did not load the 303-capability catalog: ${output}")
endif()
