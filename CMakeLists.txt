# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 17)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)

# Removed: an execute_process() call to update_device_list.sh with a DEPENDS on
# xyz.txt. Neither file has ever been in the repository, so on every clean
# checkout this ran a command that did not exist. It was invisible because
# execute_process() without RESULT_VARIABLE or COMMAND_ERROR_IS_FATAL discards
# the exit status -- configure still reported success -- and because
# execute_process() has no DEPENDS option at all, so that argument was ignored.
# The device list in ezlopi-core-devices is maintained by hand.

# Scans directories in 'curdir' and returns the list in 'result'
MACRO(SUBDIRLIST result curdir)
    FILE(GLOB children ${curdir}/*) # puts the list of all the components(files and/or folders) of 'curdir' in 'children'
    SET(dirlist "") # clear the list 'dirlist'

    FOREACH(child ${children}) # runs for loop for each components in 'children' and puts in 'child' one by one
        IF(IS_DIRECTORY ${child}) # check if the 'child' is directory or file
            LIST(APPEND dirlist ${child}) # if the 'child' is directory, append it to 'dirlist'
            message(STATUS "linking '${child}'.")
        ENDIF()
    ENDFOREACH()

    SET(${result} ${dirlist}) # copy the components of 'dirlist' into 'result'(return variable)
ENDMACRO()

# list of external components
set(external_components_list
                                ezlopi-components
                                ezlopi-sensors-devices
                                ezlopi-lua-modules
                                ezlopi-services
                                ezlopi-cloud
                                ezlopi-core
                                ezlopi-hal
                                ezlopi-util
                                )

                                #
# set_target_properties(your_subdir2 PROPERTIES EXCLUDE_FROM_ALL TRUE)

set(EXTRA_COMPONENT_DIRS "") # clearing variable 'EXTRA_COMPONENT_DIRS'
set(ALL_COMPONENTS "") # clearing 'ALL_COMPONENTS' variable

foreach(component ${external_components_list})
    message(STATUS "  \r\n")
    message(STATUS "linking '${component}:: ")
    set(SUBDIRS, "") # clearing 'SUBDIRS'
    SUBDIRLIST(SUBDIRS ${component}) # scans and gets the list of directories from 'services' in 'SUBDIRS'
    LIST(APPEND ALL_COMPONENTS ${SUBDIRS})
endforeach()

LIST(APPEND ALL_COMPONENTS ezlopi-user-config ezlopi-main)

set(EXTRA_COMPONENT_DIRS ${ALL_COMPONENTS} ) # add the list of directories 'SUBDIRS' to 'EXTRA_COMPONENT_DIRS'(ESP32 VARIABLES), links the external components
message(STATUS "EXTRA_COMPONENTS_DIRS: ${EXTRA_COMPONENT_DIRS}\r\n")

# Add AddressSanitizer flags
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")

# # Set linker flags for AddressSanitizer
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")

# Add components that needs to be excluded
set(EXCLUDE_COMPONENTS freemodbus)

## ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ ##
project(Ezlo_Pi_v4x)

# -Wshadow=local, applied to this project's own components and to nothing else.
#
# Scoped deliberately, not out of caution. A shadow flag set globally -- via
# idf_build_set_property(COMPILE_OPTIONS ...) -- also applies to ESP-IDF's own
# sources, and ESP-IDF does not build clean under it: at v4.4.6 there are six
# distinct -Wshadow=local reports inside the framework (bt/common/osi/config.c,
# esp_https_ota.c, esp_ipc.c, essl_sdio.c, btm_ble_gap.c, esp_wps.c), and that
# count grows to sixteen under plain -Wshadow. ESP-IDF already passes
# -Werror=all on every translation unit, so a global flag would simply fail the
# build on upstream code we do not own.
#
# Those six are all in ESP-IDF's .c files rather than its headers, which is why
# per-target scoping removes the noise completely instead of only reducing it --
# our translation units include ESP-IDF's headers but never compile its sources.
# A header-borne warning could not have been escaped this way.
#
# -Werror=shadow is belt-and-braces: -Werror=all would promote this anyway. It
# is here because it is what the compiler then names in the diagnostic, so the
# promotion stops depending on ESP-IDF continuing to pass -Werror=all.
#
# =local, not plain -Wshadow: =local fires only when a local or parameter is
# hidden by another local or parameter in the same function, so it is decided
# entirely by code in our own function bodies. Plain -Wshadow additionally fires
# when a local happens to share a name with any global, typedef or function
# declared by something we include, so an upstream header adding a name could
# break our build for a non-defect.
#
# The targets are named __idf_<component>, where <component> is the directory
# basename. Two guards, and they are not equally load-bearing:
#
#   TARGET      Has never fired. ALL_COMPONENTS is built by globbing
#               directories, so it can name a directory that is not a
#               registered component, but today all 176 it collects do have a
#               CMakeLists.txt. Kept because adding a directory without one is
#               an ordinary thing to do and target_compile_options() on a
#               non-existent target is fatal rather than quiet.
#   INTERFACE   Required: the first version of this loop without it failed to
#               configure. ESP-IDF registers a component that contributes no
#               source file as an INTERFACE library, and PRIVATE options on one
#               of those are a hard error -- "target_compile_options may only
#               set INTERFACE properties on INTERFACE targets". Four of ours
#               are header-only this way: pt, ezlopi-core-boards,
#               ezlopi-core-config, ezlopi-user-config. Skipping them excludes
#               no code from the check: between them they contain five headers,
#               zero .c files, and no function bodies at all, and their headers
#               are still compiled -- with the flag -- inside every translation
#               unit that includes them.
foreach(component_path ${ALL_COMPONENTS})
    get_filename_component(component_name ${component_path} NAME)

    if(TARGET __idf_${component_name})
        get_target_property(component_target_type __idf_${component_name} TYPE)

        if(NOT component_target_type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(__idf_${component_name} PRIVATE -Wshadow=local -Werror=shadow)
        endif()
    endif()
endforeach()
