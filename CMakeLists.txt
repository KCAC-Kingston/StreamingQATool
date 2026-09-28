cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
if(NOT ENABLE_FRONTEND_API OR NOT ENABLE_QT)
  message(FATAL_ERROR "StreamingQATool requires ENABLE_FRONTEND_API and ENABLE_QT")
endif()

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/live-session.cpp src/live-session.h src/rtmp-monitor.cpp src/rtmp-monitor.h src/marker-regions.cpp src/marker-regions.h src/camera-assist.cpp src/camera-assist.h src/marker-decoder.cpp src/marker-decoder.h src/marker-tracker.cpp src/marker-tracker.h
          src/marker-monitor.cpp src/marker-monitor.h
          src/event-format.cpp src/event-log.cpp src/event-log.h src/log-window.cpp src/log-window.h
          src/local-websocket.cpp src/local-websocket.h
          src/openlp-monitor.cpp src/openlp-monitor.h src/companion-monitor.cpp src/companion-monitor.h
          src/plugin-main.c src/plugin-ui.cpp src/plugin-ui.h
          src/live-control.cpp src/live-control.h
          src/live-control-requests.cpp src/request-policy.h
          src/service-manager-client.cpp src/service-manager-client.h
          src/settings.cpp src/settings.h
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/windows-http.cpp src/windows-http.h)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winhttp)
endif()
