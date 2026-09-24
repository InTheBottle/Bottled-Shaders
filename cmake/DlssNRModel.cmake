# DLSS 5 neural rendering model (nvngx_dlssnr.dll, 165 MB).
#
# GitHub refuses single files over 100 MB and the repository does not use LFS, so the model is
# committed as three 7-Zip volumes under tools/dlssnr_model (each under 50 MB). At configure time
# they are extracted into the Streamline runtime folder, where the feature payload, the AIO
# package and the feature zip pick the DLL up like any other file. The extracted DLL is
# git-ignored. The volumes live outside features/ so no package glob ships them.
#
# 7-Zip is needed for the extraction (CMake's own tar cannot read multi-volume archives). Without
# it the build still succeeds; the feature reports the missing DLL at runtime and the user copies
# it from an NVIDIA driver package instead.

set(DLSSNR_MODEL_VOLUME_DIR "${CMAKE_SOURCE_DIR}/tools/dlssnr_model")
set(DLSSNR_MODEL_FIRST_VOLUME "${DLSSNR_MODEL_VOLUME_DIR}/nvngx_dlssnr.7z.001")
set(DLSSNR_MODEL_TARGET_DIR "${CMAKE_SOURCE_DIR}/features/Upscaling/Shaders/Upscaling/Streamline")
set(DLSSNR_MODEL_TARGET "${DLSSNR_MODEL_TARGET_DIR}/nvngx_dlssnr.dll")

find_program(SEVENZ_EXECUTABLE NAMES 7z 7za
    PATHS "C:/Program Files/7-Zip" "C:/Program Files (x86)/7-Zip"
    DOC "7-Zip archiver for compressed packages"
)

function(dlssnr_model_extract)
    if(NOT EXISTS "${DLSSNR_MODEL_FIRST_VOLUME}")
        message(STATUS "DLSS-NR model volumes not found under tools/dlssnr_model; nvngx_dlssnr.dll must be supplied by hand")
        return()
    endif()

    # Up to date when the DLL is newer than every volume.
    if(EXISTS "${DLSSNR_MODEL_TARGET}")
        file(GLOB _volumes "${DLSSNR_MODEL_VOLUME_DIR}/nvngx_dlssnr.7z.*")
        set(_stale FALSE)
        foreach(_volume IN LISTS _volumes)
            if("${_volume}" IS_NEWER_THAN "${DLSSNR_MODEL_TARGET}")
                set(_stale TRUE)
            endif()
        endforeach()
        if(NOT _stale)
            return()
        endif()
    endif()

    if(NOT SEVENZ_EXECUTABLE)
        message(WARNING "7-Zip not found; cannot extract the DLSS-NR model from tools/dlssnr_model. Neural rendering will report the missing nvngx_dlssnr.dll at runtime.")
        return()
    endif()

    message(STATUS "Extracting the DLSS-NR model into ${DLSSNR_MODEL_TARGET_DIR}")
    execute_process(
        COMMAND "${SEVENZ_EXECUTABLE}" x -y "-o${DLSSNR_MODEL_TARGET_DIR}" "${DLSSNR_MODEL_FIRST_VOLUME}"
        RESULT_VARIABLE _result
        OUTPUT_QUIET
    )
    if(NOT _result EQUAL 0)
        message(WARNING "Extracting the DLSS-NR model failed (7-Zip exit code ${_result}); neural rendering will report the missing nvngx_dlssnr.dll at runtime.")
        file(REMOVE "${DLSSNR_MODEL_TARGET}")
        return()
    endif()
    # 7-Zip restores the archived timestamp, which predates the volumes; stamp the DLL as
    # current so the next configure sees it as up to date.
    file(TOUCH "${DLSSNR_MODEL_TARGET}")
endfunction()

dlssnr_model_extract()
