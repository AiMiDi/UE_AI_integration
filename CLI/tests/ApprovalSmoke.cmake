if(NOT DEFINED UE_WORKFLOW_EXECUTABLE OR
   NOT DEFINED UE_WORKFLOW_CONTRACT_ROOT OR
   NOT DEFINED UE_WORKFLOW_CAPABILITY_ROOT OR
   NOT DEFINED UE_WORKFLOW_FIXTURE OR
   NOT DEFINED UE_WORKFLOW_RECEIPT)
    message(FATAL_ERROR "Approval smoke arguments are incomplete")
endif()

file(REMOVE "${UE_WORKFLOW_RECEIPT}")

execute_process(
    COMMAND
        "${UE_WORKFLOW_EXECUTABLE}"
        --json
        --contract-root "${UE_WORKFLOW_CONTRACT_ROOT}"
        --capability-root "${UE_WORKFLOW_CAPABILITY_ROOT}"
        --endpoint "http://127.0.0.1:1"
        execute
        --file "${UE_WORKFLOW_FIXTURE}"
        --receipt "${UE_WORKFLOW_RECEIPT}"
    RESULT_VARIABLE missing_result
    OUTPUT_VARIABLE missing_output
    ERROR_VARIABLE missing_error
)
string(FIND "${missing_output}" "\"code\":\"approval_required\"" missing_code)
if(missing_result EQUAL 0 OR missing_code EQUAL -1)
    message(FATAL_ERROR "Missing digest was not rejected locally: ${missing_output}${missing_error}")
endif()

execute_process(
    COMMAND
        "${UE_WORKFLOW_EXECUTABLE}"
        --json
        --contract-root "${UE_WORKFLOW_CONTRACT_ROOT}"
        --capability-root "${UE_WORKFLOW_CAPABILITY_ROOT}"
        --endpoint "http://127.0.0.1:1"
        execute
        --file "${UE_WORKFLOW_FIXTURE}"
        --approve-plan "sha256:0000000000000000000000000000000000000000000000000000000000000000"
        --receipt "${UE_WORKFLOW_RECEIPT}"
    RESULT_VARIABLE wrong_result
    OUTPUT_VARIABLE wrong_output
    ERROR_VARIABLE wrong_error
)
string(FIND "${wrong_output}" "\"code\":\"editor_unreachable\"" wrong_code)
if(wrong_result EQUAL 0 OR wrong_code EQUAL -1)
    message(FATAL_ERROR "Execute did not require Editor plan binding: ${wrong_output}${wrong_error}")
endif()
if(EXISTS "${UE_WORKFLOW_RECEIPT}")
    message(FATAL_ERROR "Local approval rejection unexpectedly wrote a receipt")
endif()
