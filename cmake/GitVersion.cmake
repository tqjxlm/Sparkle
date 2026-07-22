# Stamps the working tree's git state into OUTPUT from TEMPLATE. Runs at build time so
# the stamp tracks the tree, not the last configure. configure_file only rewrites the
# file when its content changes, so an unchanged tree recompiles nothing.

set(GIT_COMMIT "unknown")
set(GIT_BRANCH "unknown")

find_program(GIT_EXECUTABLE git)

if(GIT_EXECUTABLE)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty --abbrev=12
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE COMMIT_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE COMMIT_RESULT
        ERROR_QUIET)
    if(COMMIT_RESULT EQUAL 0)
        set(GIT_COMMIT ${COMMIT_OUTPUT})
    endif()

    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${SOURCE_DIR}
        OUTPUT_VARIABLE BRANCH_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE BRANCH_RESULT
        ERROR_QUIET)
    if(BRANCH_RESULT EQUAL 0)
        set(GIT_BRANCH ${BRANCH_OUTPUT})
    endif()
endif()

configure_file(${TEMPLATE} ${OUTPUT} @ONLY)
