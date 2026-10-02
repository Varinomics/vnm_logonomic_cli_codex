cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
# A build that adds this package as a subproject sets the normal variable
# LOGONOMIC_CLI_ADDON_EMBEDDED. That build owns the cache, try_compile()
# settings, the SDK and testing (the SDK's docs/package_contract.md,
# "Embedded build"), so only a standalone build sets them up here.
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture the configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_codex VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR
            "The embedding build must add the Logonomic CLI add-on SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

function(codex_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    # The Windows API selection belongs to the platform, so a MinGW build gets
    # the same one as MSVC.
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(codex_provider STATIC
    src/codex_account_refresh.cpp
    src/codex_accounts.cpp
    src/codex_current_account.cpp
    src/codex_jsonl.cpp
    src/codex_jsonl_projector.cpp
    src/codex_launcher.cpp
    src/codex_lifecycle.cpp
    src/codex_preparation.cpp
    src/codex_probes.cpp
    src/codex_quota_exchange.cpp
    src/codex_startup_observation.cpp
    src/codex_startup_probe.cpp
    src/codex_usage.cpp
    src/codex_native_usage.cpp
    src/codex_usage_records.cpp)
codex_target(codex_provider)
target_include_directories(codex_provider PUBLIC src)
target_link_libraries(codex_provider PUBLIC logonomic_cli_addon_sdk::sdk)
if(WIN32)
    target_link_libraries(codex_provider PRIVATE ws2_32)
endif()

add_executable(codex_adapter src/adapter.cpp)
target_link_libraries(codex_adapter PRIVATE codex_provider)
codex_target(codex_adapter)
add_executable(codex_helper src/codex_helper.cpp)
codex_target(codex_helper)
logonomic_cli_addon_add_launch_bridge(codex_launch_bridge)
codex_target(codex_launch_bridge)
add_executable(codex_quota_helper src/codex_quota_helper.cpp src/codex_quota_exchange.cpp
    src/codex_current_account.cpp src/codex_account_refresh.cpp)
codex_target(codex_quota_helper)
if(MINGW)
    target_link_options(codex_quota_helper PRIVATE -municode)
endif()

logonomic_cli_addon_package_platform(CODEX_PLATFORM)
set(CODEX_EXECUTABLE_SUFFIX "${CMAKE_EXECUTABLE_SUFFIX}")
logonomic_cli_addon_package_directory(logonomic.cli.codex CODEX_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/*")
# Directory membership is an input too: removing a resource, or adding one
# with an older timestamp, must rebuild the indexed package. file(GENERATE)
# changes this file only when membership changes, preserving no-op builds.
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt"
    CONTENT "${package_files}\n")
add_custom_command(
    OUTPUT "${CODEX_PACKAGE_DIRECTORY}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${CODEX_PACKAGE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${CODEX_PACKAGE_DIRECTORY}/adapter" "${CODEX_PACKAGE_DIRECTORY}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/assets" "${CODEX_PACKAGE_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities" "${CODEX_PACKAGE_DIRECTORY}/capabilities"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance" "${CODEX_PACKAGE_DIRECTORY}/provenance"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/notices" "${CODEX_PACKAGE_DIRECTORY}/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CODEX_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:codex_adapter>" "${CODEX_PACKAGE_DIRECTORY}/adapter/codex_adapter${CODEX_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:codex_helper>" "${CODEX_PACKAGE_DIRECTORY}/helpers/codex_helper${CODEX_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:codex_launch_bridge>" "${CODEX_PACKAGE_DIRECTORY}/helpers/codex_launch_bridge${CODEX_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:codex_quota_helper>" "${CODEX_PACKAGE_DIRECTORY}/helpers/codex_quota_helper${CODEX_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${CODEX_PACKAGE_DIRECTORY}"
        --third-party "assets/openai.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
        --third-party "assets/openai-white.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
    DEPENDS codex_adapter codex_helper codex_launch_bridge codex_quota_helper logonomic_cli_addon_index
        "${CMAKE_CURRENT_BINARY_DIR}/generated/logonomic-cli-addon.json" "${CMAKE_CURRENT_BINARY_DIR}/package-inputs.txt" ${package_files}
    VERBATIM)
add_custom_target(codex_package ALL DEPENDS "${CODEX_PACKAGE_DIRECTORY}/package.files.json")
logonomic_cli_addon_install_package("${CODEX_PACKAGE_DIRECTORY}" logonomic.cli.codex TARGET codex_package)

# Everything below is the package's own test suite, which only its standalone
# build configures; an embedding build stages the package and stops here.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_executable(codex_fake_cli tests/codex_fake_cli.cpp)
codex_target(codex_fake_cli)
set_target_properties(codex_fake_cli PROPERTIES OUTPUT_NAME codex)
if(WIN32)
    target_link_libraries(codex_fake_cli PRIVATE ws2_32 shell32)
endif()
add_executable(codex_preparation_tests tests/codex_preparation_tests.cpp)
target_link_libraries(codex_preparation_tests PRIVATE codex_provider)
codex_target(codex_preparation_tests)
add_dependencies(codex_preparation_tests codex_fake_cli)
add_test(NAME codex_preparation_tests COMMAND codex_preparation_tests "$<TARGET_FILE:codex_fake_cli>")
set_tests_properties(codex_preparation_tests PROPERTIES TIMEOUT 45)
foreach(test_name IN ITEMS codex_jsonl_tests codex_lifecycle_tests codex_startup_observation_tests codex_probe_tests codex_usage_tests)
    add_executable(${test_name} "tests/${test_name}.cpp")
    target_link_libraries(${test_name} PRIVATE codex_provider)
    codex_target(${test_name})
    if(test_name STREQUAL "codex_probe_tests")
        add_dependencies(${test_name} codex_fake_cli codex_quota_helper)
        add_test(NAME ${test_name} COMMAND ${test_name} "$<TARGET_FILE:codex_fake_cli>" "$<TARGET_FILE:codex_quota_helper>")
    elseif(test_name STREQUAL "codex_usage_tests" OR test_name STREQUAL "codex_lifecycle_tests")
        add_dependencies(${test_name} codex_package)
        add_test(NAME ${test_name} COMMAND ${test_name} "${CODEX_PACKAGE_DIRECTORY}")
    else()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 120)
endforeach()
add_executable(codex_usage_host_tests tests/codex_usage_host_tests.cpp)
target_link_libraries(codex_usage_host_tests PRIVATE codex_provider)
codex_target(codex_usage_host_tests)
add_dependencies(codex_usage_host_tests codex_package)
add_test(NAME codex_usage_host_tests COMMAND codex_usage_host_tests "${CODEX_PACKAGE_DIRECTORY}")
set_tests_properties(codex_usage_host_tests PROPERTIES TIMEOUT 120)
add_test(NAME codex_package_validation COMMAND logonomic_cli_addon_validate "${CODEX_PACKAGE_DIRECTORY}" --platform "${CODEX_PLATFORM}")
add_test(NAME codex_package_conformance COMMAND logonomic_cli_addon_conformance "${CODEX_PACKAGE_DIRECTORY}")

# Exercise this repository's actual producer, never a substitute packaging
# implementation. The isolated nested configure uses this same SDK source;
# it does not change the owned master dependency or edit an installed package.
option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME codex_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=codex_package" "--package-id=logonomic.cli.codex"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${CODEX_PLATFORM}")
    set_tests_properties(codex_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
