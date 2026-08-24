# Resolve the pinned DWM3000 driver in ordinary checkouts and T3 worktrees.
# T3 creates a Git worktree without initializing its submodules, while Zephyr
# still runs from the canonical west workspace.  Reusing that workspace's
# exact pinned SDK keeps worktree builds self-contained at the source level
# without silently accepting a different driver revision.
function(imec_resolve_dwm3000_sdk output_variable)
    set(_imec_dwm3000_revision
        "70231425cbadc83e1d1a8b526868e3461391dd9b")
    get_filename_component(_imec_repo_root
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    set(_imec_local_sdk "${_imec_repo_root}/dwm3000 examples and sdk")
    set(_imec_sdk "${_imec_local_sdk}")

    if(NOT EXISTS "${_imec_sdk}/decadriver/deca_device.c")
        if(NOT DEFINED ZEPHYR_BASE OR NOT IS_DIRECTORY "${ZEPHYR_BASE}")
            message(FATAL_ERROR
                "DWM3000 SDK is absent and ZEPHYR_BASE cannot identify the west workspace")
        endif()
        get_filename_component(_imec_west_topdir "${ZEPHYR_BASE}/.." ABSOLUTE)
        set(_imec_sdk "${_imec_west_topdir}/dwm3000 examples and sdk")
    endif()

    if(NOT EXISTS "${_imec_sdk}/decadriver/deca_device.c")
        message(FATAL_ERROR
            "Pinned DWM3000 SDK is absent from both this checkout and the west workspace")
    endif()
    execute_process(
        COMMAND git -C "${_imec_sdk}" rev-parse HEAD
        RESULT_VARIABLE _imec_git_result
        OUTPUT_VARIABLE _imec_git_head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _imec_git_result EQUAL 0 OR
       NOT _imec_git_head STREQUAL _imec_dwm3000_revision)
        message(FATAL_ERROR
            "DWM3000 SDK must be the pinned revision ${_imec_dwm3000_revision}; found ${_imec_git_head}")
    endif()

    set(DWM3000_SDK_DIR "${_imec_sdk}" CACHE PATH
        "Exact DWM3000 SDK used by this build" FORCE)
    set(${output_variable} "${_imec_sdk}" PARENT_SCOPE)
endfunction()
