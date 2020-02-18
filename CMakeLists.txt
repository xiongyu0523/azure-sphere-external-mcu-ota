#  Copyright (c) Microsoft Corporation. All rights reserved.
#  Licensed under the MIT License.

CMAKE_MINIMUM_REQUIRED(VERSION 3.8)
PROJECT(azure-sphere-external-mcu-ota C)

# Create executable
ADD_EXECUTABLE(${PROJECT_NAME} main.c epoll_timerfd_utilities.c parson.c ota/ota.c ota/extmcu_hal.c sha256/mark2/sha256.c)
TARGET_INCLUDE_DIRECTORIES(${PROJECT_NAME} PUBLIC ${AZURE_SPHERE_API_SET_DIR}/usr/include/azureiot)
TARGET_COMPILE_DEFINITIONS(${PROJECT_NAME} PUBLIC AZURE_IOT_HUB_CONFIGURED)
TARGET_LINK_LIBRARIES(${PROJECT_NAME} m azureiot applibs pthread gcc_s c curl)

find_program(POWERSHELL powershell.exe)

if (POWERSHELL)
    # Run validate_manifest script during build
    ADD_CUSTOM_TARGET(ValidateManifest ALL
        COMMAND ${POWERSHELL} -ExecutionPolicy Bypass -NoProfile -NonInteractive -File ${CMAKE_SOURCE_DIR}/script/validate_manifest.ps1
    DEPENDS ${CMAKE_SOURCE_DIR}/app_manifest.json)
else()
    # Warn users without PowerShell to update their manifest
    ADD_CUSTOM_TARGET(ValidateManifest ALL
        COMMAND echo "Please ensure that you have updated app_manifest.json as described in IoTCentral.md or IoTHub.md, as appropriate."
    DEPENDS ${CMAKE_SOURCE_DIR}/app_manifest.json)
endif()

ADD_DEPENDENCIES(ValidateManifest ${PROJECT_NAME})

SET(ADDITIONAL_APPROOT_INCLUDES "certs/root.pem")

# Add MakeImage post-build command
INCLUDE("${AZURE_SPHERE_MAKE_IMAGE_FILE}")
