cmake_minimum_required(VERSION 3.24)
project(StepWorkstation LANGUAGES CXX)

find_package(Qt6 6.8.2 EXACT REQUIRED COMPONENTS Widgets Network Test OpenGLWidgets)
set(CMAKE_AUTOMOC ON)
qt_add_executable(StepWorkstation WIN32 workstation.cpp workstation_job.cpp workstation_converter.cpp workstation_modbus.cpp workstation_plcbridge.cpp workstation_plcbridgepanel.cpp workstation_plcjob.cpp workstation_plcpanel.cpp workstation_session.cpp workstation_experiment.cpp workstation_home.cpp workstation_replay.cpp workstation_filebridge.cpp workstation_fileuploaddialog.cpp workstation_delivery.cpp workstation_deliverydialog.cpp)
target_sources(StepWorkstation PRIVATE workstation_manualbridge.cpp workstation_twin.cpp workstation_mode.cpp workstation_welder.cpp)
qt_add_executable(StepGcodeConverter gcode_converter.cpp workstation_job.cpp workstation_delivery.cpp)
target_compile_features(StepWorkstation PRIVATE cxx_std_20)
target_compile_options(StepWorkstation PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(StepWorkstation PRIVATE Qt6::Widgets Qt6::Network Qt6::OpenGLWidgets Qt6::CorePrivate ws2_32)
target_compile_features(StepGcodeConverter PRIVATE cxx_std_20)
target_compile_options(StepGcodeConverter PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(StepGcodeConverter PRIVATE Qt6::Core Qt6::CorePrivate)

set(STEP_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/sdk_cpp/SR.SDK_C++_X64_Win_V2.4.1.20250801" CACHE PATH "STEP C++ SDK x64 directory containing RobotCom.hpp and Robot-SDK.lib")
if(MSVC AND EXISTS "${STEP_SDK_DIR}/RobotCom.hpp" AND EXISTS "${STEP_SDK_DIR}/Robot-SDK.lib")
    add_executable(helix_pc helix_pc.cpp)
    target_compile_features(helix_pc PRIVATE cxx_std_20)
    target_compile_options(helix_pc PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/wd4828>)
    target_include_directories(helix_pc SYSTEM PRIVATE "${STEP_SDK_DIR}")
    target_link_libraries(helix_pc PRIVATE "${STEP_SDK_DIR}/Robot-SDK.lib" ws2_32 iphlpapi)
else()
    message(STATUS "STEP SDK bridge needs MSVC and SDK: helix_pc skipped; preserve/deploy the existing prebuilt bridge.")
endif()

enable_testing()
foreach(trajectory_size normal small wide)
    set(trajectory_flag "")
    if(NOT trajectory_size STREQUAL "normal")
        set(trajectory_flag "--ui-${trajectory_size}")
    endif()
    add_test(NAME trajectory_lattice_${trajectory_size} COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" ${trajectory_flag}
        --trajectory-ui-smoke "${CMAKE_CURRENT_SOURCE_DIR}/../../dianzhen_R64_dryrun.nc" "${CMAKE_CURRENT_BINARY_DIR}/trajectory_lattice_${trajectory_size}.png")
    set_tests_properties(trajectory_lattice_${trajectory_size} PROPERTIES TIMEOUT 20
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endforeach()
foreach(workflow_size normal small wide)
    set(workflow_flag "")
    if(NOT workflow_size STREQUAL "normal")
        set(workflow_flag "--ui-${workflow_size}")
    endif()
    add_test(NAME workflow_ui_${workflow_size} COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" ${workflow_flag}
        --workflow-ui-smoke "${CMAKE_CURRENT_BINARY_DIR}/workflow_${workflow_size}.png")
    set_tests_properties(workflow_ui_${workflow_size} PROPERTIES TIMEOUT 20
        ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
endforeach()
add_executable(StepWelderTest tests/welder_test.cpp workstation_welder.cpp workstation_mode.cpp workstation_plcbridge.cpp workstation_modbus.cpp workstation_plcbridgepanel.cpp)
target_compile_features(StepWelderTest PRIVATE cxx_std_20)
target_link_libraries(StepWelderTest PRIVATE Qt6::Widgets Qt6::Network Qt6::Test ws2_32)
add_test(NAME welder_and_mode_bridge COMMAND StepWelderTest)
set_tests_properties(welder_and_mode_bridge PROPERTIES TIMEOUT 35
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_custom_command(TARGET StepWorkstation POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:StepWorkstation>/assets")
add_executable(StepTwinTest tests/twin_test.cpp workstation_twin.cpp workstation_plcbridge.cpp workstation_modbus.cpp)
target_compile_features(StepTwinTest PRIVATE cxx_std_20)
target_compile_options(StepTwinTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(StepTwinTest PRIVATE Qt6::Widgets Qt6::Network Qt6::OpenGLWidgets Qt6::Test ws2_32)
add_test(NAME twin_snapshot_and_model COMMAND StepTwinTest "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_CURRENT_BINARY_DIR}/twin_panel.png")
set_tests_properties(twin_snapshot_and_model PROPERTIES TIMEOUT 35
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_test(NAME twin_workstation_ui COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --twin-smoke "${CMAKE_CURRENT_BINARY_DIR}/twin_workstation.png")
add_test(NAME twin_workstation_ui_small COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --ui-small --twin-smoke "${CMAKE_CURRENT_BINARY_DIR}/twin_workstation_small.png")
add_test(NAME twin_workstation_ui_wide COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --ui-wide --twin-smoke "${CMAKE_CURRENT_BINARY_DIR}/twin_workstation_wide.png")
set_tests_properties(twin_workstation_ui twin_workstation_ui_small twin_workstation_ui_wide PROPERTIES TIMEOUT 20
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_executable(StepManualBridgeTest tests/manualbridge_test.cpp workstation_manualbridge.cpp workstation_plcbridge.cpp workstation_modbus.cpp)
target_compile_features(StepManualBridgeTest PRIVATE cxx_std_20)
target_link_libraries(StepManualBridgeTest PRIVATE Qt6::Widgets Qt6::Network Qt6::Test ws2_32)
add_test(NAME plc_manual_bridge COMMAND StepManualBridgeTest "${CMAKE_CURRENT_BINARY_DIR}/plc_manual_panel.png")
set_tests_properties(plc_manual_bridge PROPERTIES TIMEOUT 35
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_test(NAME plc_manual_workstation_ui COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}"
    --plc-manual-ui-smoke "${CMAKE_CURRENT_BINARY_DIR}/plc_manual_workstation.png")
set_tests_properties(plc_manual_workstation_ui PROPERTIES TIMEOUT 15
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_test(NAME plc_manual_ui_small COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --ui-small
    --plc-manual-ui-smoke "${CMAKE_CURRENT_BINARY_DIR}/plc_manual_small.png")
set_tests_properties(plc_manual_ui_small PROPERTIES TIMEOUT 15
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_executable(StepIdentityTest tests/identity_test.cpp)
target_compile_features(StepIdentityTest PRIVATE cxx_std_20)
add_test(NAME robot_identity_aliases COMMAND StepIdentityTest)

add_executable(StepGcodeJobTest tests/gcode_job_test.cpp workstation_job.cpp)
target_compile_features(StepGcodeJobTest PRIVATE cxx_std_20)
target_compile_options(StepGcodeJobTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(StepGcodeJobTest PRIVATE Qt6::Core Qt6::CorePrivate)
add_test(NAME gcode_job COMMAND StepGcodeJobTest)
add_custom_command(TARGET StepGcodeJobTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Core> $<TARGET_FILE_DIR:StepGcodeJobTest>)

add_executable(StepModbusTest tests/modbus_test.cpp workstation_modbus.cpp)
target_compile_features(StepModbusTest PRIVATE cxx_std_20)
target_compile_options(StepModbusTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(StepModbusTest PRIVATE Qt6::Network Qt6::Core Qt6::CorePrivate ws2_32)
add_test(NAME modbus_client COMMAND StepModbusTest)
set_tests_properties(modbus_client PROPERTIES TIMEOUT 30)
add_custom_command(TARGET StepModbusTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Core> $<TARGET_FILE:Qt6::Network> $<TARGET_FILE_DIR:StepModbusTest>)

add_executable(StepPlcJobTest tests/plcjob_test.cpp workstation_plcjob.cpp workstation_modbus.cpp)
target_compile_features(StepPlcJobTest PRIVATE cxx_std_20)
target_compile_options(StepPlcJobTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8> $<$<CXX_COMPILER_ID:MSVC>:/W4>)
target_link_libraries(StepPlcJobTest PRIVATE Qt6::Network Qt6::Core Qt6::CorePrivate ws2_32)
add_test(NAME plcjob_client COMMAND StepPlcJobTest)
set_tests_properties(plcjob_client PROPERTIES TIMEOUT 30)
add_custom_command(TARGET StepPlcJobTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Qt6::Core> $<TARGET_FILE:Qt6::Network> $<TARGET_FILE_DIR:StepPlcJobTest>)

add_executable(StepReturnSpeedTest tests/return_tcp_speed_test.cpp)
target_compile_features(StepReturnSpeedTest PRIVATE cxx_std_20)
add_test(NAME return_tcp_speed_limit COMMAND StepReturnSpeedTest)

add_executable(StepPlcBridgeTest tests/plcbridge_test.cpp workstation_plcbridge.cpp workstation_modbus.cpp)
target_compile_features(StepPlcBridgeTest PRIVATE cxx_std_20)
target_compile_options(StepPlcBridgeTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(StepPlcBridgeTest PRIVATE Qt6::Network Qt6::Core ws2_32)
add_test(NAME plc_bridge COMMAND StepPlcBridgeTest)
set_tests_properties(plc_bridge PROPERTIES TIMEOUT 60)

add_test(NAME plc_bridge_ui COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}"
    --plc-bridge-smoke "${CMAKE_CURRENT_BINARY_DIR}/plc_bridge_ui.png")
set_tests_properties(plc_bridge_ui PROPERTIES TIMEOUT 15
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_test(NAME plc_bridge_ui_small COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --ui-small
    --plc-bridge-smoke "${CMAKE_CURRENT_BINARY_DIR}/plc_bridge_small.png")
set_tests_properties(plc_bridge_ui_small PROPERTIES TIMEOUT 15
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

add_executable(StepPlcBridgePanelTest tests/plcbridge_test.cpp workstation_plcbridgepanel.cpp workstation_plcbridge.cpp workstation_modbus.cpp)
target_compile_features(StepPlcBridgePanelTest PRIVATE cxx_std_20)
target_compile_definitions(StepPlcBridgePanelTest PRIVATE STEP_PLC_PANEL_TEST)
target_compile_options(StepPlcBridgePanelTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(StepPlcBridgePanelTest PRIVATE Qt6::Widgets Qt6::Network ws2_32)
add_test(NAME plc_bridge_panel COMMAND StepPlcBridgePanelTest)
set_tests_properties(plc_bridge_panel PROPERTIES TIMEOUT 45
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

add_executable(StepFileBridgeTest tests/filebridge_test.cpp workstation_filebridge.cpp workstation_job.cpp workstation_delivery.cpp)
target_compile_features(StepFileBridgeTest PRIVATE cxx_std_20)
target_link_libraries(StepFileBridgeTest PRIVATE Qt6::Network Qt6::Core Qt6::CorePrivate)
target_compile_options(StepFileBridgeTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_test(NAME file_bridge COMMAND StepFileBridgeTest "${CMAKE_CURRENT_SOURCE_DIR}/../SR175_3200_Helix_Ready_25mms_v4.zip")
set_tests_properties(file_bridge PROPERTIES TIMEOUT 60)

add_test(NAME file_upload_ui COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}"
    --file-upload-smoke "${CMAKE_CURRENT_BINARY_DIR}/file_upload_ui.png")
set_tests_properties(file_upload_ui PROPERTIES TIMEOUT 15
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

add_test(NAME gcode_delivery_ui COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}"
    --gcode-delivery-smoke "${CMAKE_CURRENT_BINARY_DIR}/gcode_delivery_ui.png")
set_tests_properties(gcode_delivery_ui PROPERTIES TIMEOUT 15
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

add_executable(StepDeliveryFlowTest tests/filebridge_test.cpp workstation_filebridge.cpp workstation_job.cpp
    workstation_delivery.cpp workstation_deliverydialog.cpp workstation_fileuploaddialog.cpp)
target_compile_features(StepDeliveryFlowTest PRIVATE cxx_std_20)
target_compile_definitions(StepDeliveryFlowTest PRIVATE STEP_DELIVERY_GUI_TEST)
target_compile_options(StepDeliveryFlowTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(StepDeliveryFlowTest PRIVATE Qt6::Widgets Qt6::Network Qt6::CorePrivate)
add_test(NAME gcode_delivery_flow COMMAND StepDeliveryFlowTest "${CMAKE_CURRENT_SOURCE_DIR}/../SR175_3200_Helix_Ready_25mms_v4.zip"
    "${CMAKE_CURRENT_BINARY_DIR}/gcode_delivery_complete.png")
set_tests_properties(gcode_delivery_flow PROPERTIES TIMEOUT 30
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

add_executable(StepAutoDeliveryTest tests/filebridge_test.cpp workstation_filebridge.cpp workstation_job.cpp
    workstation_delivery.cpp workstation_deliverydialog.cpp workstation_fileuploaddialog.cpp)
target_compile_features(StepAutoDeliveryTest PRIVATE cxx_std_20)
target_compile_definitions(StepAutoDeliveryTest PRIVATE STEP_AUTO_GUI_TEST)
target_link_libraries(StepAutoDeliveryTest PRIVATE Qt6::Widgets Qt6::Network Qt6::CorePrivate)
add_test(NAME auto_delivery_flow COMMAND StepAutoDeliveryTest "${CMAKE_CURRENT_SOURCE_DIR}/../SR175_3200_Helix_Ready_25mms_v4.zip" "${CMAKE_CURRENT_BINARY_DIR}/auto_delivery_complete.png")
set_tests_properties(auto_delivery_flow PROPERTIES TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
add_test(NAME auto_delivery_ui COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --auto-job-ui-smoke "${CMAKE_CURRENT_BINARY_DIR}/auto_delivery_ui.png")
set_tests_properties(auto_delivery_ui PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")

foreach(file_target StepWorkstation StepFileBridgeTest StepDeliveryFlowTest StepAutoDeliveryTest)
    target_sources(${file_target} PRIVATE workstation_directftp.cpp)
endforeach()

foreach(ui_target StepWorkstation StepManualBridgeTest StepPlcBridgePanelTest)
    qt_add_resources(${ui_target} "ui_icons_${ui_target}" PREFIX "/ui" BASE "ui"
        FILES ui/chevron-up.svg ui/chevron-down.svg)
endforeach()

add_test(NAME plc_manual_ui_wide COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --ui-wide
    --plc-manual-ui-smoke "${CMAKE_CURRENT_BINARY_DIR}/plc_manual_wide.png")
add_test(NAME plc_bridge_ui_wide COMMAND StepWorkstation --control-dir "${CMAKE_CURRENT_SOURCE_DIR}" --ui-wide
    --plc-bridge-smoke "${CMAKE_CURRENT_BINARY_DIR}/plc_bridge_wide.png")
set_tests_properties(plc_manual_ui_wide plc_bridge_ui_wide PROPERTIES TIMEOUT 20
    ENVIRONMENT "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins")
