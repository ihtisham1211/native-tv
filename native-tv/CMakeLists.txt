cmake_minimum_required(VERSION 3.16)
project(nativetv_tv VERSION 0.1.0 LANGUAGES C CXX)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NATIVETV_SERVER_VERSION LIMIT_COUNT 1)
if(NOT NATIVETV_SERVER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "native-tv/VERSION must contain a semantic release version")
endif()
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(NATIVETV_SANITIZE "Enable host address and undefined-behavior sanitizers" OFF)
option(NATIVETV_BUILD_CLIENT "Build the SDL/OpenGL TV client" ON)
option(NATIVETV_STATIC_JSON "Embed json-c in TV executables" ${CMAKE_CROSSCOMPILING})
set(NATIVETV_SDK_SYSROOT "" CACHE PATH "webOS SDK sysroot used to stage runtime libraries")
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)
pkg_check_modules(JSON REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
add_library(nativetv_core src/core/json.cpp src/core/sha256.cpp src/core/md5.cpp)
target_include_directories(nativetv_core PUBLIC src)
if(NATIVETV_STATIC_JSON)
  find_file(JSON_ARCHIVE libjson-c.a HINTS ${JSON_LIBRARY_DIRS})
  if(NOT JSON_ARCHIVE)
    message(FATAL_ERROR "The TV SDK must provide the json-c static archive")
  endif()
  target_include_directories(nativetv_core SYSTEM PUBLIC ${JSON_INCLUDE_DIRS})
  target_link_libraries(nativetv_core PUBLIC "${JSON_ARCHIVE}" ${CMAKE_DL_LIBS})
else()
  target_link_libraries(nativetv_core PUBLIC PkgConfig::JSON)
endif()
target_compile_options(nativetv_core PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_client_config src/platform/client_config.cpp)
target_link_libraries(nativetv_client_config PUBLIC nativetv_core)
target_compile_options(nativetv_client_config PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_client_launch src/platform/client_launch.cpp)
target_link_libraries(nativetv_client_launch PUBLIC nativetv_core)
target_compile_options(nativetv_client_launch PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_say src/domain/say.cpp)
target_link_libraries(nativetv_say PUBLIC nativetv_core)
target_compile_options(nativetv_say PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_names src/domain/names.cpp)
target_link_libraries(nativetv_names PUBLIC nativetv_core nativetv_say)
target_compile_options(nativetv_names PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_state src/domain/live_state.cpp)
target_link_libraries(nativetv_state PUBLIC nativetv_core nativetv_names)
target_compile_options(nativetv_state PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_daemon src/daemon/app_tile_manager.cpp
  src/daemon/app_install_upload_reservation.cpp
  src/daemon/app_install_executor.cpp
  src/daemon/app_elevation_executor.cpp
  src/daemon/app_elevation_store.cpp
  src/daemon/install_job_store.cpp
  src/daemon/install_jobs.cpp
  src/daemon/child_process.cpp src/daemon/command_policy.cpp
  src/daemon/adblock_manager.cpp src/daemon/config.cpp src/daemon/devtools_firewall.cpp
  src/daemon/daily_backup_scheduler.cpp src/daemon/game_frame_rate.cpp
  src/daemon/home_boot_manager.cpp
  src/daemon/home_crash_breaker.cpp
  src/daemon/home_orchestrator.cpp
  src/daemon/home_probe_gate.cpp
  src/daemon/home_restore_manager.cpp
  src/daemon/home_screen_manager.cpp
  src/daemon/home_takeover_manager.cpp
  src/daemon/home_takeover_lifecycle.cpp
  src/daemon/home_takeover_stager.cpp
  src/daemon/ha_discovery.cpp src/daemon/ha_discovery_templates.cpp
  src/daemon/ha_entity_catalogue.cpp
  src/daemon/sam_rescan_manager.cpp
  src/daemon/screensaver_manager.cpp
  src/daemon/screensaver_control.cpp
  src/daemon/backup_restore.cpp
  src/daemon/stats_collector.cpp
  src/daemon/retired_files.cpp
  src/daemon/launcher_sync.cpp
  src/daemon/hostname_cache.cpp
  src/daemon/services_keeper.cpp
  src/daemon/tv_app_manager.cpp
  src/daemon/event_loop.cpp src/daemon/heartbeat.cpp src/daemon/json_file.cpp
  src/daemon/hdmi_seen.cpp src/daemon/last_app.cpp src/daemon/apps_state.cpp
  src/daemon/media_seen.cpp
  src/daemon/logger.cpp src/daemon/luna.cpp src/daemon/mqtt_setup.cpp
  src/daemon/mqtt_availability.cpp
  src/daemon/mqtt_client.cpp src/daemon/mqtt_command_router.cpp
  src/daemon/ntp_client.cpp src/daemon/paths.cpp
  src/daemon/mqtt_state_publisher.cpp src/daemon/mqtt_telemetry_publisher.cpp src/daemon/mqtt_publish_payload.cpp
  src/daemon/mqtt_topics.cpp
  src/daemon/remote_battery.cpp src/daemon/remote_buttons.cpp
  src/daemon/remote_button_log_watcher.cpp
  src/daemon/remote_keys_reconciler.cpp
  src/daemon/state_subscriptions.cpp
  src/daemon/screen_time_enforcer.cpp src/daemon/syslog_forwarder.cpp)
target_link_libraries(nativetv_daemon PUBLIC nativetv_core nativetv_client_config nativetv_state
  Threads::Threads PkgConfig::CURL)
target_link_libraries(nativetv_daemon PUBLIC nativetv_history)
target_compile_options(nativetv_daemon PRIVATE -Wall -Wextra -Werror)
add_library(civetweb STATIC third_party/civetweb/src/civetweb.c)
target_include_directories(civetweb PUBLIC third_party/civetweb)
target_compile_definitions(civetweb PRIVATE USE_IPV6 NO_SSL NO_CGI NO_WEBSOCKET
  NO_ZLIB NO_LUA NO_DUKTAPE USE_STACK_SIZE=262144)
target_link_libraries(civetweb PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
add_library(nativetv_http_server src/daemon/http_auth.cpp src/daemon/http_csrf.cpp
  src/daemon/http_caps.cpp src/daemon/http_frontdoor.cpp
  src/daemon/http_proxy.cpp src/daemon/http_server.cpp src/daemon/http_static_assets.cpp
  src/daemon/state_node_bridge.cpp)
target_link_libraries(nativetv_http_server PUBLIC nativetv_daemon civetweb ZLIB::ZLIB)
target_link_libraries(nativetv_http_server PUBLIC nativetv_history)
target_compile_options(nativetv_http_server PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_metrics src/service/kernel_files.cpp src/service/process_metrics.cpp
  src/service/system_metrics.cpp src/service/hdmi_inputs.cpp src/service/logs.cpp
  src/service/ntp.cpp src/service/cpu_windows.cpp src/service/network_rates.cpp
  src/service/syslog.cpp src/service/prometheus.cpp src/service/oled_care.cpp
  src/service/stuck_child_reaper.cpp
  src/service/sound_outputs.cpp)
target_link_libraries(nativetv_metrics PUBLIC nativetv_core nativetv_names Threads::Threads)
target_compile_options(nativetv_metrics PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_history src/service/history.cpp src/service/history_recorder.cpp)
target_link_libraries(nativetv_history PUBLIC nativetv_core nativetv_names nativetv_state)
target_compile_options(nativetv_history PRIVATE -Wall -Wextra -Werror)
add_library(nativetv_tv_services src/service/control_commands.cpp
  src/service/adblock.cpp src/service/adblock_store.cpp src/service/app_ids.cpp
  src/service/app_list.cpp src/service/app_catalog.cpp
  src/service/app_package_fetcher.cpp src/service/app_package_inspector.cpp
  src/service/app_package_policy.cpp src/service/app_install_preview.cpp
  src/service/app_install_space.cpp
  src/service/admin_pin.cpp src/service/app_icons.cpp src/service/app_kinds.cpp src/service/backup.cpp
  src/service/quick_access.cpp src/service/screen_time.cpp
  src/service/screensaver_policy.cpp
  src/service/stats_fields.cpp
  src/service/preload.cpp src/service/lg_settings.cpp src/service/piccap.cpp
  src/service/privacy_consent.cpp src/service/privacy_service.cpp
  src/service/config_settings.cpp src/service/remote_input.cpp src/service/remote_keys.cpp
  src/service/ssh_keys.cpp
  src/service/background_services.cpp src/service/saved_pages.cpp
  src/service/home_profiles.cpp src/service/home_channels.cpp
  src/service/home_status.cpp
  src/service/home_guide.cpp src/service/home_wallpapers.cpp
  src/service/home_catalogue.cpp src/service/home_recent_apps.cpp
  src/service/home_recent_history.cpp
  src/service/home_picture.cpp src/service/home_now.cpp
  src/service/home_open.cpp
  src/service/home_content.cpp src/service/home_discover.cpp
  src/service/home_image_proxy.cpp)
target_link_libraries(nativetv_tv_services PUBLIC nativetv_core nativetv_names
  PkgConfig::CURL ZLIB::ZLIB)
target_compile_options(nativetv_tv_services PRIVATE -Wall -Wextra -Werror)
target_link_libraries(nativetv_daemon PUBLIC nativetv_metrics nativetv_tv_services)
target_link_libraries(nativetv_http_server PUBLIC nativetv_metrics nativetv_tv_services)
add_executable(nativetv-service src/service/main.cpp)
target_link_libraries(nativetv-service PRIVATE nativetv_metrics)
target_compile_options(nativetv-service PRIVATE -Wall -Wextra -Werror)
add_executable(nativetv-daemon src/daemon/main.cpp)
target_link_libraries(nativetv-daemon PRIVATE nativetv_http_server)
target_compile_definitions(nativetv-daemon PRIVATE
  NATIVETV_SERVER_VERSION="${NATIVETV_SERVER_VERSION}")
target_compile_options(nativetv-daemon PRIVATE -Wall -Wextra -Werror)
if(NOT APPLE)
  target_link_options(nativetv-service PRIVATE -static-libstdc++ -static-libgcc -Wl,--as-needed)
  target_link_options(nativetv-daemon PRIVATE -static-libstdc++ -static-libgcc -Wl,--as-needed)
endif()
if(CMAKE_CROSSCOMPILING)
  set_target_properties(nativetv-service nativetv-daemon PROPERTIES
    BUILD_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN")
endif()
if(NATIVETV_SANITIZE)
  target_compile_options(nativetv_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(nativetv_core PUBLIC -fsanitize=address,undefined)
  target_compile_options(civetweb PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
  # Host daemon runtime tests use short SAM waits. Production SDK builds set
  # BUILD_TESTING=OFF and retain the full TV timings.
  target_compile_definitions(nativetv_daemon PRIVATE NATIVETV_TEST_FAST_SAM_TIMING=1)
endif()
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(OPENSSL_EXECUTABLE openssl)
  if(NOT OPENSSL_EXECUTABLE)
    message(FATAL_ERROR "OpenSSL is required for the custom-CA MQTT TLS test")
  endif()
  find_program(NODE_EXECUTABLE node)
  if(NOT NODE_EXECUTABLE)
    message(FATAL_ERROR "Node is required for the process CLI contract tests and the UI suites")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon_runtime.py")
  add_executable(native-sha256-tests tests/sha256.cpp)
  target_link_libraries(native-sha256-tests PRIVATE nativetv_core)
  target_compile_options(native-sha256-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-sha256 COMMAND native-sha256-tests)
  add_executable(native-client-config-tests tests/client_config.cpp)
  target_link_libraries(native-client-config-tests PRIVATE nativetv_client_config)
  target_compile_options(native-client-config-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-client-config COMMAND native-client-config-tests)
  add_executable(native-client-launch-tests tests/client_launch.cpp)
  target_link_libraries(native-client-launch-tests PRIVATE nativetv_client_launch)
  target_compile_options(native-client-launch-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-client-launch COMMAND native-client-launch-tests)
  add_test(NAME native-runtime COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_package.py")
  add_test(NAME native-install-app COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_app.py")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon_runtime.py" --list-tests
    RESULT_VARIABLE DAEMON_RUNTIME_LIST_RESULT
    OUTPUT_VARIABLE DAEMON_RUNTIME_LIST
    ERROR_VARIABLE DAEMON_RUNTIME_LIST_ERROR)
  if(NOT DAEMON_RUNTIME_LIST_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not enumerate daemon runtime tests: ${DAEMON_RUNTIME_LIST_ERROR}")
  endif()
  string(STRIP "${DAEMON_RUNTIME_LIST}" DAEMON_RUNTIME_LIST)
  string(REPLACE "\n" ";" DAEMON_RUNTIME_CASES "${DAEMON_RUNTIME_LIST}")
  foreach(DAEMON_RUNTIME_CASE IN LISTS DAEMON_RUNTIME_CASES)
    if(NOT DAEMON_RUNTIME_CASE STREQUAL "")
      add_test(NAME "native-daemon-runtime-${DAEMON_RUNTIME_CASE}"
        COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/daemon_runtime.py"
          "$<TARGET_FILE:nativetv-daemon>"
          "DaemonRuntimeTests.${DAEMON_RUNTIME_CASE}")
    endif()
  endforeach()
  add_test(NAME native-adblock-frontdoor COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/adblock_frontdoor.py" "$<TARGET_FILE:nativetv-daemon>")
  add_test(NAME native-csrf-policy COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/check-csrf-policy.py")
  add_test(NAME native-caps-route-contract COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/check-native-caps.py")
  add_executable(native-http-server-tests tests/http_server.cpp)
  target_link_libraries(native-http-server-tests PRIVATE nativetv_http_server)
  target_compile_options(native-http-server-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-http-server COMMAND native-http-server-tests)
  add_executable(native-json-threads-tests tests/json_threads.cpp)
  target_link_libraries(native-json-threads-tests PRIVATE nativetv_daemon Threads::Threads)
  target_compile_options(native-json-threads-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-json-threads COMMAND native-json-threads-tests)
  add_executable(native-store-tests tests/json_file.cpp)
  target_link_libraries(native-store-tests PRIVATE nativetv_daemon)
  target_compile_options(native-store-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-store COMMAND native-store-tests)
  add_executable(native-hdmi-seen-tests tests/hdmi_seen.cpp)
  target_link_libraries(native-hdmi-seen-tests PRIVATE nativetv_daemon)
  target_compile_options(native-hdmi-seen-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-hdmi-seen COMMAND native-hdmi-seen-tests)
  add_executable(native-last-app-tests tests/last_app.cpp)
  target_link_libraries(native-last-app-tests PRIVATE nativetv_daemon)
  target_compile_options(native-last-app-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-last-app COMMAND native-last-app-tests)
  add_executable(native-apps-state-tests tests/apps_state.cpp)
  target_link_libraries(native-apps-state-tests PRIVATE nativetv_daemon)
  target_compile_options(native-apps-state-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-apps-state COMMAND native-apps-state-tests)
  add_executable(native-app-elevation-store-tests tests/app_elevation_store.cpp)
  target_link_libraries(native-app-elevation-store-tests PRIVATE nativetv_daemon)
  target_compile_options(native-app-elevation-store-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-app-elevation-store COMMAND native-app-elevation-store-tests)
  add_executable(native-app-elevation-executor-tests tests/app_elevation_executor.cpp)
  target_link_libraries(native-app-elevation-executor-tests PRIVATE nativetv_daemon)
  target_compile_options(native-app-elevation-executor-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-app-elevation-executor COMMAND native-app-elevation-executor-tests)
  add_executable(native-app-install-executor-tests tests/app_install_executor.cpp)
  target_link_libraries(native-app-install-executor-tests PRIVATE nativetv_daemon)
  target_compile_options(native-app-install-executor-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-app-install-executor COMMAND native-app-install-executor-tests)
  add_executable(native-install-job-store-tests tests/install_job_store.cpp)
  target_link_libraries(native-install-job-store-tests PRIVATE nativetv_daemon)
  target_compile_options(native-install-job-store-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-install-job-store COMMAND native-install-job-store-tests)
  add_executable(native-app-list-tests tests/app_list.cpp)
  target_link_libraries(native-app-list-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-app-list-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-app-list COMMAND native-app-list-tests)
  add_executable(native-remote-keys-catalog-tests tests/remote_keys_catalog.cpp)
  target_link_libraries(native-remote-keys-catalog-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-remote-keys-catalog-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-remote-keys-catalog COMMAND native-remote-keys-catalog-tests)
  add_executable(native-app-catalog-tests tests/app_catalog.cpp)
  target_link_libraries(native-app-catalog-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-app-catalog-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-app-catalog COMMAND native-app-catalog-tests)
  add_executable(native-daemon-tests tests/event_loop.cpp)
  target_link_libraries(native-daemon-tests PRIVATE nativetv_daemon)
  target_compile_options(native-daemon-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-daemon COMMAND native-daemon-tests)
  add_executable(native-daily-backup-scheduler-tests tests/daily_backup_scheduler.cpp)
  target_link_libraries(native-daily-backup-scheduler-tests PRIVATE nativetv_daemon)
  target_compile_options(native-daily-backup-scheduler-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-daily-backup-scheduler COMMAND native-daily-backup-scheduler-tests)
  add_executable(native-config-tests tests/config.cpp)
  target_link_libraries(native-config-tests PRIVATE nativetv_daemon)
  target_compile_options(native-config-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-config COMMAND native-config-tests)
  add_executable(native-config-settings-tests tests/config_settings.cpp)
  target_link_libraries(native-config-settings-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-config-settings-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-config-settings COMMAND native-config-settings-tests)
  add_executable(native-ha-entity-catalogue-tests tests/ha_entity_catalogue.cpp)
  target_link_libraries(native-ha-entity-catalogue-tests PRIVATE nativetv_daemon)
  target_compile_options(native-ha-entity-catalogue-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-ha-entity-catalogue COMMAND native-ha-entity-catalogue-tests)
  add_executable(native-ha-discovery-tests tests/ha_discovery.cpp)
  target_link_libraries(native-ha-discovery-tests PRIVATE nativetv_daemon)
  target_compile_options(native-ha-discovery-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-ha-discovery COMMAND native-ha-discovery-tests)
  add_executable(native-sound-outputs-tests tests/sound_outputs.cpp)
  target_link_libraries(native-sound-outputs-tests PRIVATE nativetv_metrics)
  target_compile_options(native-sound-outputs-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-sound-outputs COMMAND native-sound-outputs-tests)
  add_test(NAME native-ha-golden
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ha_golden.py
      $<TARGET_FILE:native-ha-entity-catalogue-tests> $<TARGET_FILE:native-ha-discovery-tests>)
  add_executable(native-saved-pages-tests tests/saved_pages.cpp)
  target_link_libraries(native-saved-pages-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-saved-pages-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-saved-pages COMMAND native-saved-pages-tests)
  add_executable(native-home-profiles-tests tests/home_profiles.cpp)
  target_link_libraries(native-home-profiles-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-profiles-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-profiles COMMAND native-home-profiles-tests)
  add_executable(native-home-channels-tests tests/home_channels.cpp)
  target_link_libraries(native-home-channels-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-channels-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-channels COMMAND native-home-channels-tests)
  add_executable(native-home-guide-tests tests/home_guide.cpp)
  target_link_libraries(native-home-guide-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-guide-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-guide COMMAND native-home-guide-tests)
  add_executable(native-home-picture-tests tests/home_picture.cpp)
  target_link_libraries(native-home-picture-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-picture-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-picture COMMAND native-home-picture-tests)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_picture_runtime.py")
  add_test(NAME native-home-picture-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_picture_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  add_executable(native-home-now-tests tests/home_now.cpp)
  target_link_libraries(native-home-now-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-now-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-now COMMAND native-home-now-tests)
  add_executable(native-home-status-tests tests/home_status.cpp)
  target_link_libraries(native-home-status-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-status-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-status COMMAND native-home-status-tests)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_status_runtime.py")
  add_test(NAME native-home-status-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_status_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_now_runtime.py")
  add_test(NAME native-home-now-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_now_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  add_executable(native-home-wallpapers-tests tests/home_wallpapers.cpp)
  target_link_libraries(native-home-wallpapers-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-wallpapers-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-wallpapers COMMAND native-home-wallpapers-tests)
  add_executable(native-home-catalogue-tests tests/home_catalogue.cpp)
  target_link_libraries(native-home-catalogue-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-catalogue-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-catalogue COMMAND native-home-catalogue-tests)
  add_executable(native-home-recent-apps-tests tests/home_recent_apps.cpp)
  target_link_libraries(native-home-recent-apps-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-recent-apps-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-recent-apps COMMAND native-home-recent-apps-tests)
  add_executable(native-home-open-tests tests/home_open.cpp)
  target_link_libraries(native-home-open-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-open-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-open COMMAND native-home-open-tests)
  add_executable(native-home-content-tests tests/home_content.cpp)
  target_link_libraries(native-home-content-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-content-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-content COMMAND native-home-content-tests)
  add_executable(native-home-discover-tests tests/home_discover.cpp)
  target_link_libraries(native-home-discover-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-discover-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-discover COMMAND native-home-discover-tests)
  add_executable(native-home-image-proxy-tests tests/home_image_proxy.cpp)
  target_link_libraries(native-home-image-proxy-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-home-image-proxy-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-home-image-proxy COMMAND native-home-image-proxy-tests)
  add_executable(native-admin-pin-tests tests/admin_pin.cpp)
  target_link_libraries(native-admin-pin-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-admin-pin-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-admin-pin COMMAND native-admin-pin-tests)
  add_executable(native-app-icons-tests tests/app_icons.cpp)
  target_link_libraries(native-app-icons-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-app-icons-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-app-icons COMMAND native-app-icons-tests)
  add_executable(native-backup-tests tests/backup.cpp)
  target_link_libraries(native-backup-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-backup-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-backup COMMAND native-backup-tests)
  add_executable(native-logger-tests tests/logger.cpp)
  target_link_libraries(native-logger-tests PRIVATE nativetv_daemon)
  target_compile_options(native-logger-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-logger COMMAND native-logger-tests)
  add_executable(native-child-tests tests/child_process.cpp)
  target_link_libraries(native-child-tests PRIVATE nativetv_daemon)
  target_compile_options(native-child-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-child COMMAND native-child-tests)
  add_executable(native-fake-luna-send tests/fake_luna_send.cpp)
  target_link_libraries(native-fake-luna-send PRIVATE nativetv_core Threads::Threads)
  target_compile_options(native-fake-luna-send PRIVATE -Wall -Wextra -Werror)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_channels_runtime.py")
  add_test(NAME native-home-channels-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_channels_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_guide_runtime.py")
  add_test(NAME native-home-guide-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_guide_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_wallpapers_runtime.py")
  add_test(NAME native-home-wallpapers-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_wallpapers_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_content_runtime.py")
  add_test(NAME native-home-content-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_content_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_icon_runtime.py")
  add_test(NAME native-home-icon-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_icon_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_catalogue_runtime.py")
  add_test(NAME native-home-catalogue-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_catalogue_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_recent_apps_runtime.py")
  add_test(NAME native-home-recent-apps-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_recent_apps_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_open_runtime.py")
  add_test(NAME native-home-open-runtime COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/home_open_runtime.py"
    "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-fake-luna-send>")
  add_executable(native-app-catalog-frontdoor-tests tests/app_catalog_frontdoor.cpp)
  target_link_libraries(native-app-catalog-frontdoor-tests PRIVATE nativetv_http_server)
  target_compile_options(native-app-catalog-frontdoor-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_dependencies(native-app-catalog-frontdoor-tests native-fake-luna-send)
  add_test(NAME native-app-catalog-frontdoor COMMAND native-app-catalog-frontdoor-tests
    "$<TARGET_FILE:native-fake-luna-send>")
  add_executable(native-app-package-fetcher-tests tests/app_package_fetcher.cpp)
  target_link_libraries(native-app-package-fetcher-tests PRIVATE nativetv_http_server)
  target_compile_options(native-app-package-fetcher-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-app-package-fetcher COMMAND native-app-package-fetcher-tests)
  add_executable(native-app-package-inspector-tests tests/app_package_inspector.cpp)
  target_link_libraries(native-app-package-inspector-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-app-package-inspector-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-app-package-inspector COMMAND native-app-package-inspector-tests)
  add_executable(native-app-package-policy-tests tests/app_package_policy.cpp)
  target_link_libraries(native-app-package-policy-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-app-package-policy-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-app-package-policy COMMAND native-app-package-policy-tests)
  add_executable(native-app-install-preview-tests tests/app_install_preview.cpp)
  target_link_libraries(native-app-install-preview-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-app-install-preview-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-app-install-preview COMMAND native-app-install-preview-tests)
  add_executable(native-app-install-space-tests tests/app_install_space.cpp)
  target_link_libraries(native-app-install-space-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-app-install-space-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-app-install-space COMMAND native-app-install-space-tests)
  add_executable(native-app-install-upload-reservation-tests
    tests/app_install_upload_reservation.cpp)
  target_link_libraries(native-app-install-upload-reservation-tests PRIVATE nativetv_daemon)
  target_compile_options(native-app-install-upload-reservation-tests PRIVATE
    -Wall -Wextra -Werror)
  add_test(NAME native-app-install-upload-reservation
    COMMAND native-app-install-upload-reservation-tests)
  add_executable(native-luna-tests tests/luna.cpp)
  target_link_libraries(native-luna-tests PRIVATE nativetv_daemon)
  target_compile_options(native-luna-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-luna-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_test(NAME native-luna COMMAND native-luna-tests)
  add_executable(native-command-policy-tests tests/command_policy.cpp)
  target_link_libraries(native-command-policy-tests PRIVATE nativetv_daemon)
  target_compile_options(native-command-policy-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-command-policy-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_dependencies(native-command-policy-tests native-fake-luna-send)
  add_test(NAME native-command-policy COMMAND native-command-policy-tests)
  add_executable(native-state-subscriptions-tests tests/state_subscriptions.cpp)
  target_link_libraries(native-state-subscriptions-tests PRIVATE nativetv_daemon)
  target_compile_options(native-state-subscriptions-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-state-subscriptions-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_dependencies(native-state-subscriptions-tests native-fake-luna-send)
  add_test(NAME native-state-subscriptions COMMAND native-state-subscriptions-tests)
  add_executable(native-remote-keys-reconciler-tests
    tests/remote_keys_reconciler.cpp)
  target_link_libraries(native-remote-keys-reconciler-tests PRIVATE nativetv_daemon)
  target_compile_options(native-remote-keys-reconciler-tests PRIVATE
    -Wall -Wextra -Werror)
  target_compile_definitions(native-remote-keys-reconciler-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_dependencies(native-remote-keys-reconciler-tests native-fake-luna-send)
  add_test(NAME native-remote-keys-reconciler
    COMMAND native-remote-keys-reconciler-tests)
  add_executable(native-remote-keys-reconcile-tests
    tests/remote_keys_reconcile.cpp)
  target_link_libraries(native-remote-keys-reconcile-tests PRIVATE
    nativetv_http_server)
  target_compile_options(native-remote-keys-reconcile-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  target_compile_definitions(native-remote-keys-reconcile-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_dependencies(native-remote-keys-reconcile-tests native-fake-luna-send)
  add_test(NAME native-remote-keys-reconcile
    COMMAND native-remote-keys-reconcile-tests)
  add_executable(native-remote-battery-tests tests/remote_battery.cpp)
  target_link_libraries(native-remote-battery-tests PRIVATE nativetv_daemon)
  target_compile_options(native-remote-battery-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-remote-battery-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_dependencies(native-remote-battery-tests native-fake-luna-send)
  add_test(NAME native-remote-battery COMMAND native-remote-battery-tests)
  add_executable(native-remote-input-tests tests/remote_input.cpp)
  target_link_libraries(native-remote-input-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-remote-input-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-remote-input COMMAND native-remote-input-tests)
  add_test(NAME native-system COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/system_metrics.py"
    "$<TARGET_FILE:nativetv-service>")
  add_executable(native-metrics-tests tests/process_metrics.cpp)
  target_link_libraries(native-metrics-tests PRIVATE nativetv_metrics)
  target_compile_options(native-metrics-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-metrics COMMAND native-metrics-tests)
  add_executable(native-prometheus-renderer src/service/prometheus_main.cpp)
  target_link_libraries(native-prometheus-renderer PRIVATE nativetv_metrics)
  target_compile_options(native-prometheus-renderer PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-prometheus-golden COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/prometheus_golden.py"
    "$<TARGET_FILE:native-prometheus-renderer>")
  add_executable(native-hdmi-inputs-tests tests/hdmi_inputs.cpp)
  target_link_libraries(native-hdmi-inputs-tests PRIVATE nativetv_metrics)
  target_compile_definitions(native-hdmi-inputs-tests PRIVATE NATIVETV_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  target_compile_options(native-hdmi-inputs-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-hdmi-inputs COMMAND native-hdmi-inputs-tests)
  add_executable(native-logs-tests tests/logs.cpp)
  target_link_libraries(native-logs-tests PRIVATE nativetv_metrics)
  target_compile_options(native-logs-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-logs COMMAND native-logs-tests)
  add_executable(native-syslog-tests tests/syslog.cpp)
  target_link_libraries(native-syslog-tests PRIVATE nativetv_metrics)
  target_compile_options(native-syslog-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-syslog-format COMMAND native-syslog-tests)
  add_executable(native-syslog-forwarder-tests tests/syslog_forwarder.cpp)
  target_link_libraries(native-syslog-forwarder-tests PRIVATE nativetv_daemon)
  target_compile_options(native-syslog-forwarder-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-syslog-forwarder COMMAND native-syslog-forwarder-tests)
  add_executable(native-ntp-tests tests/ntp.cpp)
  target_link_libraries(native-ntp-tests PRIVATE nativetv_metrics)
  target_compile_options(native-ntp-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-ntp COMMAND native-ntp-tests)
  add_executable(native-ntp-client-tests tests/ntp_client.cpp)
  target_link_libraries(native-ntp-client-tests PRIVATE nativetv_daemon)
  target_compile_options(native-ntp-client-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-ntp-client-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_test(NAME native-ntp-client COMMAND native-ntp-client-tests)
  add_executable(native-cpu-windows-tests tests/cpu_windows.cpp)
  target_link_libraries(native-cpu-windows-tests PRIVATE nativetv_metrics)
  target_compile_options(native-cpu-windows-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-cpu-windows COMMAND native-cpu-windows-tests)
  add_executable(native-network-rates-tests tests/network_rates.cpp)
  target_link_libraries(native-network-rates-tests PRIVATE nativetv_metrics)
  target_compile_options(native-network-rates-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-network-rates COMMAND native-network-rates-tests)
  add_executable(native-history-tests tests/history.cpp)
  target_link_libraries(native-history-tests PRIVATE nativetv_history)
  target_compile_options(native-history-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-history COMMAND native-history-tests)
  add_executable(native-quick-access-tests tests/quick_access.cpp)
  target_link_libraries(native-quick-access-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-quick-access-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-quick-access COMMAND native-quick-access-tests)
  add_executable(native-control-command-tests tests/control_commands.cpp)
  target_link_libraries(native-control-command-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-control-command-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-control-command COMMAND native-control-command-tests)
  add_executable(native-piccap-tests tests/piccap.cpp)
  target_link_libraries(native-piccap-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-piccap-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-piccap COMMAND native-piccap-tests)
  add_executable(native-oled-care-tests tests/oled_care.cpp)
  target_link_libraries(native-oled-care-tests PRIVATE nativetv_metrics)
  target_compile_options(native-oled-care-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-oled-care COMMAND native-oled-care-tests)
  add_executable(native-privacy-consent-tests tests/privacy_consent.cpp)
  target_link_libraries(native-privacy-consent-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-privacy-consent-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-privacy-consent COMMAND native-privacy-consent-tests)
  add_executable(native-privacy-service-tests tests/privacy_service.cpp)
  target_link_libraries(native-privacy-service-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-privacy-service-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-privacy-service COMMAND native-privacy-service-tests)
  add_executable(native-ssh-keys-tests tests/ssh_keys.cpp)
  target_link_libraries(native-ssh-keys-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-ssh-keys-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-ssh-keys COMMAND native-ssh-keys-tests)
  add_executable(native-background-services-tests tests/background_services.cpp)
  target_link_libraries(native-background-services-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-background-services-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-background-services COMMAND native-background-services-tests)
  add_executable(native-adblock-tests tests/adblock.cpp)
  target_link_libraries(native-adblock-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-adblock-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-adblock COMMAND native-adblock-tests)
  add_executable(native-screen-time-tests tests/screen_time.cpp)
  target_link_libraries(native-screen-time-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-screen-time-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-screen-time COMMAND native-screen-time-tests)
  add_executable(native-screensaver-policy-tests tests/screensaver_policy.cpp)
  target_link_libraries(native-screensaver-policy-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-screensaver-policy-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-screensaver-policy COMMAND native-screensaver-policy-tests)
  add_executable(native-screensaver-control-tests tests/screensaver_control.cpp)
  target_link_libraries(native-screensaver-control-tests PRIVATE nativetv_daemon)
  target_compile_options(native-screensaver-control-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-screensaver-control COMMAND native-screensaver-control-tests)
  add_executable(native-install-jobs-tests tests/install_jobs.cpp)
  target_link_libraries(native-install-jobs-tests PRIVATE nativetv_daemon Threads::Threads)
  target_compile_options(native-install-jobs-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-install-jobs
    COMMAND native-install-jobs-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/install-jobs.json)
  add_executable(native-home-orchestrator-tests tests/home_orchestrator.cpp)
  target_link_libraries(native-home-orchestrator-tests PRIVATE nativetv_daemon nativetv_tv_services Threads::Threads)
  target_compile_options(native-home-orchestrator-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-home-orchestrator
    COMMAND native-home-orchestrator-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/home-orchestrator.json)
  add_executable(native-home-crash-breaker-tests tests/home_crash_breaker.cpp)
  target_link_libraries(native-home-crash-breaker-tests PRIVATE nativetv_daemon)
  target_compile_options(native-home-crash-breaker-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-home-crash-breaker COMMAND native-home-crash-breaker-tests)
  add_executable(native-home-probe-gate-tests tests/home_probe_gate.cpp)
  target_link_libraries(native-home-probe-gate-tests PRIVATE nativetv_daemon Threads::Threads)
  target_compile_options(native-home-probe-gate-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-home-probe-gate COMMAND native-home-probe-gate-tests)
  add_executable(native-backup-restore-tests tests/backup_restore.cpp)
  target_link_libraries(native-backup-restore-tests PRIVATE nativetv_daemon)
  target_compile_options(native-backup-restore-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-backup-restore COMMAND native-backup-restore-tests)
  # Every manifest route answers with no Node behind the daemon and every switch on.
  add_test(NAME native-probe-node-free
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/node-free-probe.py"
            $<TARGET_FILE:nativetv-daemon> --flags=all --timeout=3 --fail-on-node)
  set_tests_properties(native-probe-node-free PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
  add_executable(native-adblock-manager-tests tests/adblock_manager.cpp)
  target_link_libraries(native-adblock-manager-tests PRIVATE nativetv_daemon)
  target_compile_options(native-adblock-manager-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-adblock-manager COMMAND native-adblock-manager-tests)
  add_executable(native-sam-rescan-manager-tests tests/sam_rescan_manager.cpp)
  target_link_libraries(native-sam-rescan-manager-tests PRIVATE nativetv_daemon)
  target_compile_options(native-sam-rescan-manager-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-sam-rescan-manager-tests PRIVATE
    NATIVETV_PYTHON="${Python3_EXECUTABLE}"
    NATIVETV_SAM_FAKE_TV="${CMAKE_CURRENT_SOURCE_DIR}/tests/sam_fake_tv.py")
  add_test(NAME native-sam-rescan-manager COMMAND native-sam-rescan-manager-tests)
  add_executable(native-home-screen-manager-tests tests/home_screen_manager.cpp)
  target_link_libraries(native-home-screen-manager-tests PRIVATE nativetv_daemon)
  target_compile_options(native-home-screen-manager-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-home-screen-manager COMMAND native-home-screen-manager-tests)
  add_executable(native-lg-settings-tests tests/lg_settings.cpp)
  target_link_libraries(native-lg-settings-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-lg-settings-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-lg-settings COMMAND native-lg-settings-tests)
  add_executable(native-launcher-sync-tests tests/launcher_sync.cpp)
  target_link_libraries(native-launcher-sync-tests PRIVATE nativetv_daemon)
  target_compile_options(native-launcher-sync-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  target_compile_definitions(native-launcher-sync-tests PRIVATE NATIVETV_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_test(NAME native-launcher-sync COMMAND native-launcher-sync-tests)
  add_executable(native-devtools-firewall-tests tests/devtools_firewall.cpp)
  target_link_libraries(native-devtools-firewall-tests PRIVATE nativetv_daemon)
  target_compile_options(native-devtools-firewall-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-devtools-firewall COMMAND native-devtools-firewall-tests)
  add_executable(native-game-frame-rate-tests tests/game_frame_rate.cpp)
  target_link_libraries(native-game-frame-rate-tests PRIVATE nativetv_daemon)
  target_compile_options(native-game-frame-rate-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-game-frame-rate COMMAND native-game-frame-rate-tests)
  add_executable(native-hostname-cache-tests tests/hostname_cache.cpp)
  target_link_libraries(native-hostname-cache-tests PRIVATE nativetv_daemon)
  target_compile_options(native-hostname-cache-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-hostname-cache COMMAND native-hostname-cache-tests)
  add_executable(native-static-assets-tests tests/http_static_assets.cpp)
  target_link_libraries(native-static-assets-tests PRIVATE nativetv_http_server)
  target_compile_options(native-static-assets-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-static-assets COMMAND native-static-assets-tests)
  add_executable(native-retired-files-tests tests/retired_files.cpp)
  target_link_libraries(native-retired-files-tests PRIVATE nativetv_daemon)
  target_compile_options(native-retired-files-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-retired-files COMMAND native-retired-files-tests)
  add_executable(native-tv-app-manager-tests tests/tv_app_manager.cpp)
  target_link_libraries(native-tv-app-manager-tests PRIVATE nativetv_daemon)
  target_compile_options(native-tv-app-manager-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-tv-app-manager COMMAND native-tv-app-manager-tests)
  # tvwebctl against a stand-in daemon; needs Linux's /proc and start-stop-daemon (skips, 77, elsewhere).
  add_test(NAME native-tvwebctl COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/tvwebctl_test.sh
           ${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/tvwebctl)
  set_tests_properties(native-tvwebctl PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240 RUN_SERIAL TRUE)
  add_executable(native-services-keeper-tests tests/services_keeper.cpp)
  target_link_libraries(native-services-keeper-tests PRIVATE nativetv_daemon)
  target_compile_options(native-services-keeper-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-services-keeper-tests PRIVATE NATIVETV_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME native-services-keeper COMMAND native-services-keeper-tests)
  add_executable(native-stats-fields-tests tests/stats_fields.cpp)
  target_link_libraries(native-stats-fields-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-stats-fields-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-stats-fields
    COMMAND native-stats-fields-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/stats-fields.json)
  add_executable(native-stats-collector-tests tests/stats_collector.cpp)
  target_link_libraries(native-stats-collector-tests PRIVATE nativetv_daemon)
  target_compile_options(native-stats-collector-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-stats-collector COMMAND native-stats-collector-tests)
  add_executable(native-mqtt-publish-tests tests/mqtt_publish_payload.cpp)
  target_link_libraries(native-mqtt-publish-tests PRIVATE nativetv_daemon)
  target_compile_options(native-mqtt-publish-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-mqtt-publish
    COMMAND native-mqtt-publish-tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mqtt-publish.json)
  add_executable(native-preload-tests tests/preload.cpp)
  target_link_libraries(native-preload-tests PRIVATE nativetv_tv_services)
  target_compile_options(native-preload-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-preload COMMAND native-preload-tests)
  add_executable(native-screen-time-runtime-tests tests/screen_time_enforcer.cpp)
  target_link_libraries(native-screen-time-runtime-tests PRIVATE nativetv_daemon)
  target_compile_options(native-screen-time-runtime-tests PRIVATE -Wall -Wextra -Werror)
  target_compile_definitions(native-screen-time-runtime-tests PRIVATE
    NATIVETV_FAKE_LUNA_SEND="$<TARGET_FILE:native-fake-luna-send>")
  add_dependencies(native-screen-time-runtime-tests native-fake-luna-send)
  add_test(NAME native-screen-time-runtime COMMAND native-screen-time-runtime-tests)
  add_executable(native-mqtt-tls-probe tests/mqtt_tls_probe.cpp)
  target_link_libraries(native-mqtt-tls-probe PRIVATE nativetv_daemon)
  target_compile_options(native-mqtt-tls-probe PRIVATE -Wall -Wextra -Werror)
  add_executable(native-mqtt-client-tests tests/mqtt_client.cpp)
  target_link_libraries(native-mqtt-client-tests PRIVATE nativetv_daemon)
  target_compile_options(native-mqtt-client-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-mqtt-client COMMAND native-mqtt-client-tests)
  add_executable(native-mqtt-state-tests tests/mqtt_state.cpp)
  target_link_libraries(native-mqtt-state-tests PRIVATE nativetv_daemon)
  target_compile_options(native-mqtt-state-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-mqtt-state COMMAND native-mqtt-state-tests)
  add_executable(native-mqtt-telemetry-tests tests/mqtt_telemetry.cpp)
  target_link_libraries(native-mqtt-telemetry-tests PRIVATE nativetv_daemon)
  target_compile_options(native-mqtt-telemetry-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-mqtt-telemetry COMMAND native-mqtt-telemetry-tests)
  add_executable(native-mqtt-command-router-tests tests/mqtt_command_router.cpp)
  target_link_libraries(native-mqtt-command-router-tests PRIVATE nativetv_daemon)
  target_compile_options(native-mqtt-command-router-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-mqtt-command-router COMMAND native-mqtt-command-router-tests)
  add_executable(native-mqtt-availability-tests tests/mqtt_availability.cpp)
  target_link_libraries(native-mqtt-availability-tests PRIVATE nativetv_daemon)
  target_compile_options(native-mqtt-availability-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-mqtt-availability COMMAND native-mqtt-availability-tests)
  add_executable(native-remote-buttons-tests tests/remote_buttons.cpp)
  target_link_libraries(native-remote-buttons-tests PRIVATE nativetv_daemon)
  target_compile_options(native-remote-buttons-tests PRIVATE
    -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME native-remote-buttons COMMAND native-remote-buttons-tests)
  add_test(NAME native-mqtt-tls COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/mqtt_tls_fixture.py"
    "$<TARGET_FILE:native-mqtt-tls-probe>" "${OPENSSL_EXECUTABLE}")
  add_test(NAME native-process-contract COMMAND "${NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/process-contract.js" "$<TARGET_FILE:nativetv-service>")
  # The dashboard's scripts and the boot hook, run on the host.
  add_test(NAME native-ui-suites COMMAND "${NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui/run.js")
  add_test(NAME native-service-port COMMAND "${NODE_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/service-port.js" "$<TARGET_FILE:nativetv-service>")
  add_test(NAME native-service-stuck-child-reaper COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/stuck_child_reaper_runtime.py"
    "$<TARGET_FILE:nativetv-service>")
  add_executable(native-names-tests tests/names.cpp)
  target_link_libraries(native-names-tests PRIVATE nativetv_names)
  target_compile_options(native-names-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-names COMMAND native-names-tests)
  add_executable(native-tv-name-tests tests/tv_name.cpp)
  target_link_libraries(native-tv-name-tests PRIVATE nativetv_names)
  target_compile_definitions(native-tv-name-tests PRIVATE NATIVETV_TV_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tv")
  target_compile_options(native-tv-name-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-tv-name COMMAND native-tv-name-tests)
  add_executable(native-say-tests tests/say.cpp)
  target_link_libraries(native-say-tests PRIVATE nativetv_say)
  target_compile_options(native-say-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-say COMMAND native-say-tests)
  add_executable(native-live-state-tests tests/live_state.cpp)
  target_link_libraries(native-live-state-tests PRIVATE nativetv_state)
  target_compile_options(native-live-state-tests PRIVATE -Wall -Wextra -Werror)
  add_test(NAME native-live-state COMMAND native-live-state-tests)
  # These check with assert(), which a Release build (-DNDEBUG) compiles away.
  foreach(assert_test native-app-install-preview-tests native-app-catalog-frontdoor-tests
          native-app-package-inspector-tests native-app-package-policy-tests
          native-background-services-tests native-config-settings-tests
          native-mqtt-availability-tests native-remote-buttons-tests native-saved-pages-tests)
    if(TARGET ${assert_test})
      target_compile_options(${assert_test} PRIVATE -UNDEBUG)
    endif()
  endforeach()
endif()
install(TARGETS nativetv-service nativetv-daemon RUNTIME DESTINATION .)
if(NATIVETV_BUILD_CLIENT)
  pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2 SDL2_image SDL2_ttf)
  pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
  add_library(nativetv_models src/domain/home.cpp src/domain/dashboard.cpp src/domain/setup.cpp src/domain/setup_pages.cpp src/ui/keyboard.cpp
    src/domain/live_cards.cpp src/domain/settings_items.cpp)
  target_include_directories(nativetv_models PUBLIC src)
  target_link_libraries(nativetv_models PUBLIC nativetv_core nativetv_say nativetv_names)
  target_compile_options(nativetv_models PRIVATE -Wall -Wextra -Werror)
  add_library(qrcodegen STATIC third_party/qrcodegen/qrcodegen.cpp)
  target_include_directories(qrcodegen PUBLIC third_party/qrcodegen)
  add_library(nanosvg STATIC third_party/nanosvg/implementation.cpp)
  target_include_directories(nanosvg PUBLIC third_party/nanosvg)
  add_library(nativetv_presentation
    src/ui/scene.cpp src/ui/kit/components.cpp src/ui/kit/widgets.cpp src/ui/kit/gallery.cpp
    src/ui/application.cpp src/ui/navigation.cpp src/ui/input.cpp
    src/ui/menu_controller.cpp src/ui/data_controller.cpp
    src/ui/home_view.cpp src/ui/menu_view.cpp
    src/ui/picture_controller.cpp src/ui/recents_controller.cpp src/ui/recents_view.cpp
    src/ui/home_motion.cpp src/ui/screen_registry.cpp src/ui/optimistic.cpp
    src/ui/kit/entry.cpp src/ui/keyboard_pad.cpp src/ui/qr_code.cpp src/ui/setup_screen.cpp
    src/ui/splash.cpp
    src/ui/dashboard_screens.cpp src/ui/live_screen.cpp src/ui/settings_list.cpp
    src/ui/settings_screen.cpp src/ui/catalog_screen.cpp
    src/ui/kit/overlay.cpp src/ui/quick_screen.cpp src/ui/profiles_screen.cpp
    src/ui/details_screen.cpp src/ui/editor_screen.cpp src/ui/pin_screen.cpp)
  target_link_libraries(nativetv_presentation PUBLIC nativetv_models nativetv_motion nativetv_say qrcodegen)
  target_compile_options(nativetv_presentation PRIVATE -Wall -Wextra -Werror)
  add_library(nativetv_http src/adapters/http_services.cpp src/adapters/legacy_routes.cpp)
  target_link_libraries(nativetv_http PUBLIC nativetv_models PkgConfig::CURL Threads::Threads)
  target_compile_options(nativetv_http PRIVATE -Wall -Wextra -Werror)
  add_library(nativetv_raster src/graphics/svg.cpp src/graphics/text.cpp
    src/graphics/a8_atlas.cpp src/graphics/glyph_atlas.cpp src/graphics/icon_atlas.cpp
    src/graphics/image_loader.cpp)
  target_link_libraries(nativetv_raster PUBLIC nativetv_models nanosvg PkgConfig::SDL PkgConfig::FREETYPE Threads::Threads)
  target_compile_options(nativetv_raster PRIVATE -Wall -Wextra -Werror)
  add_library(nativetv_kit_glyphs src/ui/kit/run_glyphs.cpp)
  target_link_libraries(nativetv_kit_glyphs PUBLIC nativetv_presentation nativetv_raster)
  target_compile_options(nativetv_kit_glyphs PRIVATE -Wall -Wextra -Werror)
  # The client's loop pieces: frame statistics and the app clock (no SDL), and
  # the window, GL context and --probe (GL).
  add_library(nativetv_frame_loop src/platform/frame_stats.cpp)
  target_include_directories(nativetv_frame_loop PUBLIC src)
  target_compile_options(nativetv_frame_loop PRIVATE -Wall -Wextra -Werror)
  add_library(nativetv_gl_probe src/platform/gl_window.cpp src/platform/probe.cpp)
  target_link_libraries(nativetv_gl_probe PUBLIC nativetv_core PkgConfig::SDL)
  target_compile_options(nativetv_gl_probe PRIVATE -Wall -Wextra -Werror)
  if(APPLE)
    find_package(OpenGL REQUIRED)
    target_compile_definitions(nativetv_gl_probe PRIVATE GL_GLEXT_PROTOTYPES)
    target_link_libraries(nativetv_gl_probe PUBLIC OpenGL::GL)
    target_compile_options(nativetv_gl_probe PRIVATE -Wno-deprecated-declarations)
  else()
    target_link_libraries(nativetv_gl_probe PUBLIC GLESv2)
  endif()
  add_library(nativetv_motion src/ui/motion/motion.cpp)
  target_include_directories(nativetv_motion PUBLIC src)
  target_compile_options(nativetv_motion PRIVATE -Wall -Wextra -Werror)
  # Scene to GPU batches and the GLSL sources: no GL calls, so tests run them headless.
  add_library(nativetv_batch src/graphics/batch.cpp src/graphics/shaders.cpp)
  target_include_directories(nativetv_batch PUBLIC src)
  target_compile_options(nativetv_batch PRIVATE -Wall -Wextra -Werror)
  add_executable(nativetv-tv src/main.cpp src/platform/paths.cpp src/graphics/renderer.cpp)
  target_link_libraries(nativetv-tv PRIVATE nativetv_batch)
  target_link_libraries(nativetv-tv PRIVATE nativetv_client_config nativetv_client_launch nativetv_presentation nativetv_kit_glyphs nativetv_http qrcodegen nativetv_raster nativetv_frame_loop nativetv_gl_probe PkgConfig::SDL)
  target_compile_options(nativetv-tv PRIVATE -Wall -Wextra -Werror)
  if(APPLE)
    target_compile_definitions(nativetv-tv PRIVATE GL_GLEXT_PROTOTYPES)
    find_package(OpenGL REQUIRED)
    target_link_libraries(nativetv-tv PRIVATE OpenGL::GL)
    target_compile_options(nativetv-tv PRIVATE -Wno-deprecated-declarations)
  else()
    target_link_libraries(nativetv-tv PRIVATE GLESv2)
    target_link_options(nativetv-tv PRIVATE -static-libstdc++ -static-libgcc -Wl,--as-needed)
  endif()
  if(CMAKE_CROSSCOMPILING)
    set_target_properties(nativetv-tv PROPERTIES
      BUILD_RPATH "$ORIGIN"
      BUILD_WITH_INSTALL_RPATH TRUE
      INSTALL_RPATH "$ORIGIN")
  endif()
  if(BUILD_TESTING)
    add_executable(native-model-tests tests/models.cpp)
    target_link_libraries(native-model-tests PRIVATE nativetv_models)
    target_compile_options(native-model-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-models COMMAND native-model-tests)
    add_executable(native-controller-tests tests/controllers.cpp)
    target_link_libraries(native-controller-tests PRIVATE nativetv_presentation)
    target_compile_options(native-controller-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-controllers COMMAND native-controller-tests)
    add_executable(native-overlay-screen-tests tests/overlay_screens.cpp)
    target_link_libraries(native-overlay-screen-tests PRIVATE nativetv_presentation)
    target_compile_options(native-overlay-screen-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-overlay-screens COMMAND native-overlay-screen-tests)
    add_executable(native-navigation-walk-tests tests/navigation_walk.cpp)
    target_link_libraries(native-navigation-walk-tests PRIVATE nativetv_presentation)
    target_compile_definitions(native-navigation-walk-tests PRIVATE NATIVETV_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_compile_options(native-navigation-walk-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-navigation-walk COMMAND native-navigation-walk-tests)
    add_executable(native-dashboard-tests tests/dashboard_screens.cpp)
    target_link_libraries(native-dashboard-tests PRIVATE nativetv_presentation)
    target_compile_definitions(native-dashboard-tests PRIVATE NATIVETV_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_compile_options(native-dashboard-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-dashboard COMMAND native-dashboard-tests)
    add_executable(native-setup-tests tests/setup_screen.cpp)
    target_link_libraries(native-setup-tests PRIVATE nativetv_presentation)
    target_compile_options(native-setup-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-setup COMMAND native-setup-tests)
    add_executable(native-setup-flow-tests tests/setup_flow.cpp)
    target_link_libraries(native-setup-flow-tests PRIVATE nativetv_http nativetv_presentation)
    target_compile_options(native-setup-flow-tests PRIVATE -Wall -Wextra -Werror)
    add_executable(native-qr-matrix tests/qr_matrix.cpp)
    target_link_libraries(native-qr-matrix PRIVATE nativetv_presentation)
    target_compile_options(native-qr-matrix PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-setup-flow COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/setup_flow.py" "$<TARGET_FILE:native-setup-flow-tests>")
    # The same first run against the real daemon (without Node, with its stand-in luna-send).
    add_dependencies(native-setup-flow-tests nativetv-daemon native-fake-luna-send)
    add_test(NAME native-setup-daemon COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/setup_daemon.py" "$<TARGET_FILE:nativetv-daemon>" "$<TARGET_FILE:native-setup-flow-tests>")
    add_executable(native-quick-drawer-tests tests/quick_drawer.cpp)
    target_link_libraries(native-quick-drawer-tests PRIVATE nativetv_presentation)
    target_compile_options(native-quick-drawer-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-quick-drawer COMMAND native-quick-drawer-tests)
    add_executable(native-optimistic-tests tests/optimistic.cpp)
    target_link_libraries(native-optimistic-tests PRIVATE nativetv_presentation)
    target_compile_options(native-optimistic-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-optimistic COMMAND native-optimistic-tests)
    add_test(NAME native-optimistic-guard COMMAND "${Python3_EXECUTABLE}"
             "${CMAKE_CURRENT_SOURCE_DIR}/tests/optimistic_guard.py" --src "${CMAKE_CURRENT_SOURCE_DIR}/src/ui")
    add_executable(native-kit-tests tests/kit.cpp)
    target_link_libraries(native-kit-tests PRIVATE nativetv_kit_glyphs nativetv_motion)
    target_compile_definitions(native-kit-tests PRIVATE NATIVETV_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    target_compile_options(native-kit-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-kit COMMAND native-kit-tests)
    add_test(NAME native-kit-tokens COMMAND "${Python3_EXECUTABLE}"
             ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_tokens.py --check)
    if(APPLE)
      list(JOIN SDL_LIBRARY_DIRS ":" KIT_RUNTIME_LIBRARY_PATH)
      set_tests_properties(native-kit PROPERTIES ENVIRONMENT "DYLD_LIBRARY_PATH=${KIT_RUNTIME_LIBRARY_PATH}")
    endif()
    add_executable(native-raster-tests tests/raster.cpp)
    target_link_libraries(native-raster-tests PRIVATE nativetv_raster)
    target_compile_options(native-raster-tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(native-raster-tests PRIVATE NATIVETV_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_test(NAME native-raster COMMAND native-raster-tests)
    # The renderer checks draw scenes through the batcher into a CPU raster and compare goldens.
    target_link_libraries(native-raster-tests PRIVATE nativetv_batch nativetv_presentation)
    target_compile_definitions(native-raster-tests PRIVATE NATIVETV_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_executable(native-atlas-tests tests/atlas.cpp)
    target_link_libraries(native-atlas-tests PRIVATE nativetv_raster)
    target_compile_options(native-atlas-tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(native-atlas-tests PRIVATE NATIVETV_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    add_test(NAME native-atlas COMMAND native-atlas-tests)
    add_test(NAME native-woff-to-ttf COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/woff_roundtrip.py")
    add_executable(native-image-loader-tests tests/image_loader.cpp)
    target_link_libraries(native-image-loader-tests PRIVATE nativetv_raster)
    target_compile_options(native-image-loader-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-image-loader COMMAND native-image-loader-tests)
    if(APPLE)
      # SDL2 compatibility builds load SDL3 dynamically; ASan needs the pkg-config search path.
      list(JOIN SDL_LIBRARY_DIRS ":" SDL_RUNTIME_LIBRARY_PATH)
      set_tests_properties(native-raster native-atlas native-image-loader PROPERTIES ENVIRONMENT "DYLD_LIBRARY_PATH=${SDL_RUNTIME_LIBRARY_PATH}")
    endif()
    # The same scenes through the real renderer; skipped (77) without a GL context.
    add_executable(native-renderer-gl-tests tests/renderer_gl.cpp src/graphics/renderer.cpp)
    target_link_libraries(native-renderer-gl-tests PRIVATE nativetv_batch nativetv_presentation nativetv_raster qrcodegen PkgConfig::SDL)
    target_compile_options(native-renderer-gl-tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(native-renderer-gl-tests PRIVATE NATIVETV_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets")
    if(APPLE)
      target_compile_definitions(native-renderer-gl-tests PRIVATE GL_GLEXT_PROTOTYPES)
      target_link_libraries(native-renderer-gl-tests PRIVATE OpenGL::GL)
      target_compile_options(native-renderer-gl-tests PRIVATE -Wno-deprecated-declarations)
    else()
      target_link_libraries(native-renderer-gl-tests PRIVATE GLESv2)
    endif()
    add_test(NAME native-renderer-gl COMMAND native-renderer-gl-tests)
    set_tests_properties(native-renderer-gl PROPERTIES SKIP_RETURN_CODE 77)
    if(APPLE)
      set_tests_properties(native-renderer-gl PROPERTIES ENVIRONMENT "DYLD_LIBRARY_PATH=${SDL_RUNTIME_LIBRARY_PATH}")
    endif()
    if(NATIVETV_SANITIZE)
      set_property(TEST native-image-loader native-renderer-gl APPEND PROPERTY
                   ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
    endif()
    add_executable(native-http-tests tests/http.cpp)
    target_link_libraries(native-http-tests PRIVATE nativetv_http)
    target_compile_options(native-http-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-http COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/http_fixture.py" "$<TARGET_FILE:native-http-tests>")
    add_executable(native-frame-loop-tests tests/frame_loop.cpp)
    target_link_libraries(native-frame-loop-tests PRIVATE nativetv_frame_loop)
    target_compile_options(native-frame-loop-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME native-frame-loop COMMAND native-frame-loop-tests)
    # These run the client itself; they skip when the machine has no GL to give it.
    add_test(NAME native-client-platform COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/client_platform.py" "$<TARGET_FILE:nativetv-tv>")
    add_test(NAME native-golden-compare COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/test_golden.py")
    add_test(NAME native-golden COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/golden.py" check --client "$<TARGET_FILE:nativetv-tv>")
    set_tests_properties(native-golden PROPERTIES SKIP_RETURN_CODE 77)
    # The end-to-end cases need a real TV (NATIVETV_TV_HOST); without one this entry skips, visibly.
    add_test(NAME native-e2e-runner COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/test_runner.py")
    add_test(NAME native-e2e-tv COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/run.py" --all)
    set_tests_properties(native-e2e-tv PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "NATIVETV_TV_HOST=")
    add_test(NAME native-qr-capture COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/qr_capture.py" "$<TARGET_FILE:nativetv-tv>" "$<TARGET_FILE:native-qr-matrix>")
    set_tests_properties(native-qr-capture PROPERTIES SKIP_RETURN_CODE 77)
    if(APPLE)
      set_tests_properties(native-client-platform native-golden native-qr-capture PROPERTIES ENVIRONMENT "DYLD_LIBRARY_PATH=${SDL_RUNTIME_LIBRARY_PATH}")
    endif()
    add_executable(native-motion-tests tests/motion.cpp)
    target_link_libraries(native-motion-tests PRIVATE nativetv_motion)
    target_compile_options(native-motion-tests PRIVATE -Wall -Wextra -Werror)
    add_test(NAME native-motion COMMAND native-motion-tests)
  endif()
  install(TARGETS nativetv-tv RUNTIME DESTINATION .)
  install(FILES appinfo.json DESTINATION .)
  file(COPY assets DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
  install(DIRECTORY assets DESTINATION .)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json-c/SOURCE.md" JSON_SOURCE_RECORD)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json-c/SOURCE.md")
  string(REGEX REPLACE ".*<!-- BEGIN DISTRIBUTION NOTICE -->\n(.*)<!-- END DISTRIBUTION NOTICE -->.*" "\\1" JSON_NOTICE "${JSON_SOURCE_RECORD}")
  if(JSON_NOTICE STREQUAL JSON_SOURCE_RECORD)
    message(FATAL_ERROR "The json-c source record has no distribution notice")
  endif()
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/assets/LICENSE-json-c.txt" "${JSON_NOTICE}")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/assets/LICENSE-json-c.txt" DESTINATION assets)
endif()
