cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
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

if(NOT TARGET tomlplusplus::tomlplusplus)
    find_path(CODEX_TOML_INCLUDE_DIR NAMES toml++/toml.hpp)
    if(NOT CODEX_TOML_INCLUDE_DIR)
        include(FetchContent)
        # Consume only headers so sibling packages can select their own parser target.
        FetchContent_Declare(codex_tomlplusplus
            GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR include)
        FetchContent_MakeAvailable(codex_tomlplusplus)
        set(CODEX_TOML_INCLUDE_DIR "${codex_tomlplusplus_SOURCE_DIR}/include")
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
    src/codex_pet.cpp
    src/codex_context.cpp
    src/codex_conversation.cpp
    src/codex_conversation_source.cpp
    src/codex_current_account.cpp
    src/codex_jsonl.cpp
    src/codex_jsonl_projector.cpp
    src/codex_launcher.cpp
    src/codex_launcher_approval.cpp
    src/codex_lifecycle.cpp
    src/codex_preparation.cpp
    src/codex_probes.cpp
    src/codex_quota_exchange.cpp
    src/codex_session_wake.cpp
    src/codex_startup_observation.cpp
    src/codex_startup_probe.cpp
    src/codex_terminal_identity.cpp
    src/codex_usage.cpp
    src/codex_usage_temporal.cpp
    src/codex_usage_records.cpp)
codex_target(codex_provider)
target_include_directories(codex_provider PUBLIC src)
target_link_libraries(codex_provider PUBLIC logonomic_cli_addon_sdk::sdk)
if(WIN32)
    target_link_libraries(codex_provider PRIVATE ws2_32 shell32 ole32 uuid)
    if(MINGW)
        string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" codex_sdk_arch)
        if(codex_sdk_arch MATCHES "^(x64|x86_64|amd64)$")
            set(codex_sdk_arch x64)
        elseif(codex_sdk_arch MATCHES "^(x86|i[3-6]86)$")
            set(codex_sdk_arch x86)
        elseif(codex_sdk_arch MATCHES "^(arm64|aarch64)$")
            set(codex_sdk_arch arm64)
        elseif(codex_sdk_arch MATCHES "^(arm|armv7.*)$")
            set(codex_sdk_arch arm)
        else()
            message(FATAL_ERROR
                "Cannot select the Windows SQLite import library for target "
                "architecture '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}'.")
        endif()

        set(codex_sdk_roots "$ENV{WindowsSdkDir}")
        foreach(codex_registry_view IN ITEMS 64 32)
            cmake_host_system_information(RESULT codex_sdk_root
                QUERY WINDOWS_REGISTRY
                    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
                VALUE KitsRoot10 VIEW ${codex_registry_view})
            list(APPEND codex_sdk_roots "${codex_sdk_root}")
        endforeach()
        list(REMOVE_DUPLICATES codex_sdk_roots)
        set(codex_sqlite_header "")
        set(codex_sqlite_library "")
        foreach(codex_sdk_root IN LISTS codex_sdk_roots)
            if(NOT codex_sdk_root)
                continue()
            endif()
            cmake_path(CONVERT "${codex_sdk_root}" TO_CMAKE_PATH_LIST codex_sdk_root NORMALIZE)
            file(GLOB codex_sdk_versions RELATIVE "${codex_sdk_root}/Include"
                "${codex_sdk_root}/Include/*")
            list(SORT codex_sdk_versions COMPARE NATURAL ORDER DESCENDING)
            foreach(codex_sdk_version IN LISTS codex_sdk_versions)
                set(codex_sdk_include "${codex_sdk_root}/Include/${codex_sdk_version}/um")
                set(codex_sdk_library
                    "${codex_sdk_root}/Lib/${codex_sdk_version}/um/${codex_sdk_arch}/winsqlite3.lib")
                if(EXISTS "${codex_sdk_include}/winsqlite/winsqlite3.h" AND
                   EXISTS "${codex_sdk_library}")
                    set(codex_sqlite_header "${codex_sdk_include}/winsqlite/winsqlite3.h")
                    set(codex_sqlite_library "${codex_sdk_library}")
                    break()
                endif()
            endforeach()
            if(codex_sqlite_header)
                break()
            endif()
        endforeach()
        if(NOT codex_sqlite_header)
            message(FATAL_ERROR
                "Codex conversation extraction requires a Windows SDK with "
                "winsqlite/winsqlite3.h and the ${codex_sdk_arch} winsqlite3.lib.")
        endif()

        # Keep the SDK's C API header without replacing MinGW's system headers.
        set(codex_sqlite_include "${CMAKE_CURRENT_BINARY_DIR}/windows_sqlite/include")
        file(MAKE_DIRECTORY "${codex_sqlite_include}/winsqlite")
        configure_file("${codex_sqlite_header}"
            "${codex_sqlite_include}/winsqlite/winsqlite3.h" COPYONLY)
        target_include_directories(codex_provider PRIVATE "${codex_sqlite_include}")
        target_link_libraries(codex_provider PRIVATE "${codex_sqlite_library}")
    else()
        target_link_libraries(codex_provider PRIVATE winsqlite3)
    endif()
else()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(codex_provider PRIVATE SQLite::SQLite3)
endif()

add_executable(codex_adapter src/adapter.cpp)
target_link_libraries(codex_adapter PRIVATE codex_provider)
codex_target(codex_adapter)
add_executable(codex_helper src/codex_helper.cpp)
codex_target(codex_helper)
add_executable(codex_pet_helper src/codex_pet_helper.cpp src/codex_pet_inspection.cpp)
codex_target(codex_pet_helper)
if(TARGET tomlplusplus::tomlplusplus)
    target_link_libraries(codex_pet_helper PRIVATE tomlplusplus::tomlplusplus)
else()
    target_include_directories(codex_pet_helper SYSTEM PRIVATE "${CODEX_TOML_INCLUDE_DIR}")
endif()
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
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(codex_package logonomic.cli.codex CODEX_PACKAGE_DIRECTORY
    ADAPTER codex_adapter
    HELPERS codex_helper codex_pet_helper codex_launch_bridge codex_quota_helper
    INDEX_ARGUMENTS
        --third-party "assets/openai.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json"
        --third-party "assets/openai-white.svg" "notices/lobe_icons_mit.txt"
            "provenance/lobe_icons.json")

# Everything below is the package's own test suite, which only its standalone
# build configures; an embedding build stages the package and stops here.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_executable(codex_pet_tests tests/codex_pet_tests.cpp)
target_link_libraries(codex_pet_tests PRIVATE codex_provider)
codex_target(codex_pet_tests)
add_dependencies(codex_pet_tests codex_package)
add_test(NAME codex_pet_tests COMMAND codex_pet_tests "${CODEX_PACKAGE_DIRECTORY}")
set_tests_properties(codex_pet_tests PROPERTIES TIMEOUT 90)
add_executable(codex_macho_tests tests/codex_macho_tests.cpp)
target_include_directories(codex_macho_tests PRIVATE src)
if(MSVC)
    target_compile_options(codex_macho_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(codex_macho_tests PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME codex_macho_tests COMMAND codex_macho_tests)
set_tests_properties(codex_macho_tests PROPERTIES TIMEOUT 10)
add_executable(codex_conversation_tests tests/codex_conversation_tests.cpp)
target_link_libraries(codex_conversation_tests PRIVATE codex_provider)
if(MINGW)
    target_include_directories(codex_conversation_tests PRIVATE "${codex_sqlite_include}")
elseif(NOT WIN32)
    target_link_libraries(codex_conversation_tests PRIVATE SQLite::SQLite3)
endif()
codex_target(codex_conversation_tests)
add_test(NAME codex_conversation_tests COMMAND codex_conversation_tests)
set_tests_properties(codex_conversation_tests PROPERTIES TIMEOUT 30)
add_executable(codex_resume_tests tests/codex_resume_tests.cpp)
target_link_libraries(codex_resume_tests PRIVATE codex_provider)
if(MINGW)
    target_include_directories(codex_resume_tests PRIVATE "${codex_sqlite_include}")
elseif(NOT WIN32)
    target_link_libraries(codex_resume_tests PRIVATE SQLite::SQLite3)
endif()
codex_target(codex_resume_tests)
add_dependencies(codex_resume_tests codex_package)
add_test(NAME codex_resume_tests COMMAND codex_resume_tests "${CODEX_PACKAGE_DIRECTORY}" "$<TARGET_FILE:codex_fake_cli>")
set_tests_properties(codex_resume_tests PROPERTIES TIMEOUT 30)
add_executable(codex_terminal_identity_tests tests/codex_terminal_identity_tests.cpp)
target_link_libraries(codex_terminal_identity_tests PRIVATE codex_provider)
if(MINGW)
    target_include_directories(codex_terminal_identity_tests PRIVATE "${codex_sqlite_include}")
elseif(NOT WIN32)
    target_link_libraries(codex_terminal_identity_tests PRIVATE SQLite::SQLite3)
endif()
codex_target(codex_terminal_identity_tests)
add_test(NAME codex_terminal_identity_tests COMMAND codex_terminal_identity_tests)
set_tests_properties(codex_terminal_identity_tests PROPERTIES TIMEOUT 30)
add_executable(codex_fake_cli tests/codex_fake_cli.cpp)
add_dependencies(codex_resume_tests codex_fake_cli)
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
# Two malformed-observer cases each consume a 12-second deadline alongside the serial launch matrix.
set_tests_properties(codex_preparation_tests PROPERTIES TIMEOUT 90)
add_executable(codex_session_wake_tests tests/codex_session_wake_tests.cpp)
target_link_libraries(codex_session_wake_tests PRIVATE codex_provider)
codex_target(codex_session_wake_tests)
add_dependencies(codex_session_wake_tests codex_fake_cli)
add_test(NAME codex_session_wake_tests COMMAND codex_session_wake_tests "$<TARGET_FILE:codex_fake_cli>")
set_tests_properties(codex_session_wake_tests PROPERTIES TIMEOUT 90)
foreach(test_name IN ITEMS codex_jsonl_tests codex_lifecycle_tests codex_startup_observation_tests codex_probe_tests codex_usage_tests codex_context_tests)
    add_executable(${test_name} "tests/${test_name}.cpp")
    target_link_libraries(${test_name} PRIVATE codex_provider)
    codex_target(${test_name})
    if(test_name STREQUAL "codex_probe_tests")
        add_dependencies(${test_name} codex_fake_cli codex_quota_helper)
        add_test(NAME ${test_name} COMMAND ${test_name} "$<TARGET_FILE:codex_fake_cli>" "$<TARGET_FILE:codex_quota_helper>")
    elseif(test_name STREQUAL "codex_usage_tests" OR test_name STREQUAL "codex_lifecycle_tests" OR test_name STREQUAL "codex_context_tests")
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
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(codex_fake_vnmctl tests/codex_fake_vnmctl.cpp)
codex_target(codex_fake_vnmctl)
add_test(NAME codex_reply_helper_tests COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/codex_reply_tests.py"
    "${CODEX_PACKAGE_DIRECTORY}/helpers/codex_helper${CODEX_EXECUTABLE_SUFFIX}"
    "$<TARGET_FILE:codex_fake_vnmctl>"
    "${CODEX_PACKAGE_DIRECTORY}/adapter/codex_adapter${CODEX_EXECUTABLE_SUFFIX}")
set_tests_properties(codex_reply_helper_tests PROPERTIES TIMEOUT 120)

# Exercise this repository's actual producer, never a substitute packaging
# implementation. The isolated nested configure uses this same SDK source;
# it does not change the owned master dependency or edit an installed package.
option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME codex_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${_membership_sdk_source}/cmake/package_membership_tests.py"
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
