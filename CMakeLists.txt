cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(AircraftRfQt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(/utf-8)
endif()

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network)
get_target_property(QT_CORE_LOCATION Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION_DEBUG)
if(NOT QT_CORE_LOCATION)
    get_target_property(QT_CORE_LOCATION Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION_RELEASE)
endif()
if(NOT QT_CORE_LOCATION)
    get_target_property(QT_CORE_LOCATION Qt${QT_VERSION_MAJOR}::Core IMPORTED_LOCATION)
endif()
get_filename_component(QT_RUNTIME_DIR "${QT_CORE_LOCATION}" DIRECTORY)
get_target_property(QT_QMAKE_EXECUTABLE Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
if(QT_QMAKE_EXECUTABLE)
    execute_process(
        COMMAND "${QT_QMAKE_EXECUTABLE}" -query QT_INSTALL_PLUGINS
        OUTPUT_VARIABLE QT_INSTALL_PLUGINS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(QT_INSTALL_PLUGINS)
    file(TO_CMAKE_PATH "${QT_INSTALL_PLUGINS}" QT_PLUGIN_DIR)
else()
    get_filename_component(QT_INSTALL_ROOT "${QT_RUNTIME_DIR}/.." ABSOLUTE)
    set(QT_PLUGIN_DIR "${QT_INSTALL_ROOT}/plugins")
endif()
set(QT_PLATFORM_PLUGIN_DIR "${QT_PLUGIN_DIR}/platforms")

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_standard_project_setup()
endif()

option(AIRCRAFT_ENABLE_UHD "Build UHD/USRP hardware support when the SDK is available" ON)
option(AIRCRAFT_ENABLE_NI_VISA "Build NI-VISA/3943B hardware support when the SDK is available" ON)

set(UHD_ROOT "$ENV{UHD_ROOT}" CACHE PATH "Root directory of UHD SDK")
if(AIRCRAFT_ENABLE_UHD)
    find_path(UHD_INCLUDE_DIR
        NAMES uhd/usrp/multi_usrp.hpp
        HINTS
            "${UHD_ROOT}/include"
            "$ENV{UHD_ROOT}/include"
        PATH_SUFFIXES include
    )
    find_library(UHD_LIBRARY
        NAMES uhd
        HINTS
            "${UHD_ROOT}/lib"
            "$ENV{UHD_ROOT}/lib"
        PATH_SUFFIXES lib lib64
    )
    if(NOT UHD_INCLUDE_DIR OR NOT UHD_LIBRARY)
        message(WARNING "UHD SDK not found; building without USRP hardware support. Set UHD_ROOT or -DAIRCRAFT_ENABLE_UHD=OFF to silence this warning.")
        set(AIRCRAFT_ENABLE_UHD OFF)
    endif()
endif()

find_path(BOOST_INCLUDE_DIR
    NAMES boost/core/noncopyable.hpp
    PATHS
        "$ENV{BOOST_ROOT}/include"
        "$ENV{CONDA_PREFIX}/Library/include"
        "E:/anaconda/Library/include"
        "C:/local/boost"
)
if(NOT BOOST_INCLUDE_DIR AND AIRCRAFT_ENABLE_UHD)
    message(FATAL_ERROR "Boost headers not found. Set BOOST_INCLUDE_DIR to a directory containing boost/core/noncopyable.hpp.")
endif()

if(AIRCRAFT_ENABLE_NI_VISA)
    set(NI_VISA_INCLUDE_DIR "$ENV{NI_VISA_INCLUDE_DIR}" CACHE PATH "Directory containing visa.h")
    set(NI_VISA_LIBRARY "$ENV{NI_VISA_LIBRARY}" CACHE FILEPATH "Path to the VISA shared library")
    if(WIN32)
        find_path(NI_VISA_INCLUDE_DIR
            NAMES visa.h
            PATHS "C:/Program Files/IVI Foundation/VISA/Win64/Include"
        )
        find_library(NI_VISA_LIBRARY
            NAMES visa64 visa
            PATHS "C:/Program Files/IVI Foundation/VISA/Win64/Lib_x64/msc"
        )
    else()
        find_path(NI_VISA_INCLUDE_DIR
            NAMES visa.h
            PATHS /usr/include /usr/local/include
        )
        find_library(NI_VISA_LIBRARY
            NAMES visa nivisa
            PATHS /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
        )
    endif()
endif()
if(AIRCRAFT_ENABLE_NI_VISA AND (NOT NI_VISA_INCLUDE_DIR OR NOT NI_VISA_LIBRARY))
    message(WARNING "NI-VISA SDK not found; building without 3943B VISA hardware support. Set NI_VISA_INCLUDE_DIR/NI_VISA_LIBRARY or -DAIRCRAFT_ENABLE_NI_VISA=OFF to silence this warning.")
    set(AIRCRAFT_ENABLE_NI_VISA OFF)
endif()
if(AIRCRAFT_ENABLE_NI_VISA)
    get_filename_component(NI_VISA_LIBRARY_DIR "${NI_VISA_LIBRARY}" DIRECTORY)
    if(WIN32)
        get_filename_component(NI_VISA_ROOT_DIR "${NI_VISA_LIBRARY_DIR}/../.." ABSOLUTE)
        set(NI_VISA_RUNTIME_DIR "${NI_VISA_ROOT_DIR}/Bin")
    else()
        set(NI_VISA_RUNTIME_DIR "${NI_VISA_LIBRARY_DIR}")
    endif()
endif()

set(AIRCRAFT_TEST_ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${QT_RUNTIME_DIR}"
)
if(AIRCRAFT_ENABLE_UHD)
    list(APPEND AIRCRAFT_TEST_ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${UHD_ROOT}/bin"
    )
endif()
if(AIRCRAFT_ENABLE_NI_VISA)
    list(APPEND AIRCRAFT_TEST_ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${NI_VISA_RUNTIME_DIR}"
    )
endif()
set(AIRCRAFT_UI_TEST_ENVIRONMENT
    "QT_QPA_PLATFORM=offscreen"
    "QT_QPA_PLATFORM_PLUGIN_PATH=${QT_PLATFORM_PLUGIN_DIR}"
)

set(AIRCRAFT_CORE_SOURCES
    src/app/ProjectPaths.cpp
    src/core/Records.cpp
    src/services/AppStateStore.cpp
    src/services/CandidateReviewService.cpp
    src/services/CapProbeService.cpp
    src/services/CeyearIfIqPacketParser.cpp
    src/services/CeyearIqReceiver.cpp
    src/services/CeyearPscReceiver.cpp
    src/services/CeyearPscSweep.cpp
    src/services/CeyearVisaQuery.cpp
    src/services/PreprocessProcessRunner.cpp
    src/services/RecognitionProcessRunner.cpp
    src/services/TrainingProcessRunner.cpp
    src/services/UsrpDeviceService.cpp
)
if(AIRCRAFT_ENABLE_NI_VISA)
    list(APPEND AIRCRAFT_CORE_SOURCES src/services/CeyearVisaService.cpp)
else()
    list(APPEND AIRCRAFT_CORE_SOURCES src/services/CeyearVisaServiceStub.cpp)
endif()

add_library(aircraft_core STATIC ${AIRCRAFT_CORE_SOURCES})

target_include_directories(aircraft_core PUBLIC src)
if(AIRCRAFT_ENABLE_UHD)
    target_compile_definitions(aircraft_core PUBLIC AIRCRAFT_HAS_UHD=1)
    target_include_directories(aircraft_core PUBLIC "${UHD_INCLUDE_DIR}" "${BOOST_INCLUDE_DIR}")
    target_link_libraries(aircraft_core PUBLIC "${UHD_LIBRARY}")
else()
    target_compile_definitions(aircraft_core PUBLIC AIRCRAFT_HAS_UHD=0)
endif()
if(AIRCRAFT_ENABLE_NI_VISA)
    target_compile_definitions(aircraft_core PUBLIC AIRCRAFT_HAS_NI_VISA=1)
    target_include_directories(aircraft_core PRIVATE "${NI_VISA_INCLUDE_DIR}")
    target_link_libraries(aircraft_core PUBLIC "${NI_VISA_LIBRARY}")
else()
    target_compile_definitions(aircraft_core PUBLIC AIRCRAFT_HAS_NI_VISA=0)
endif()
target_link_libraries(aircraft_core PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_executable(aircraft_qt
        src/main.cpp
        src/ui/AppTheme.cpp
        src/ui/MainWindow.cpp
        src/ui/pages/WorkflowPage.cpp
        src/ui/pages/CapturePage.cpp
        src/ui/pages/SweepPage.cpp
        src/ui/pages/DatasetPage.cpp
        src/ui/pages/PreprocessPage.cpp
        src/ui/pages/RecognitionPage.cpp
        src/ui/pages/TrainPage.cpp
        src/ui/widgets/IllustrationPanel.cpp
        src/ui/widgets/MetricCard.cpp
        src/ui/widgets/StatusBadge.cpp
        src/ui/widgets/StepNavigator.cpp
        src/ui/widgets/SpectrogramReviewWidget.cpp
        src/ui/widgets/WaterfallWidget.cpp
    )
else()
    add_executable(aircraft_qt
        src/main.cpp
        src/ui/AppTheme.cpp
        src/ui/MainWindow.cpp
        src/ui/pages/WorkflowPage.cpp
        src/ui/pages/CapturePage.cpp
        src/ui/pages/SweepPage.cpp
        src/ui/pages/DatasetPage.cpp
        src/ui/pages/PreprocessPage.cpp
        src/ui/pages/RecognitionPage.cpp
        src/ui/pages/TrainPage.cpp
        src/ui/widgets/IllustrationPanel.cpp
        src/ui/widgets/MetricCard.cpp
        src/ui/widgets/StatusBadge.cpp
        src/ui/widgets/StepNavigator.cpp
        src/ui/widgets/SpectrogramReviewWidget.cpp
        src/ui/widgets/WaterfallWidget.cpp
    )
endif()

target_include_directories(aircraft_qt PRIVATE src)
target_link_libraries(aircraft_qt PRIVATE aircraft_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(AIRCRAFT_ENABLE_NI_VISA)
    add_executable(ceyear_status_query
        src/tools/ceyear_status_query.cpp
    )
    target_include_directories(ceyear_status_query PRIVATE src "${NI_VISA_INCLUDE_DIR}")
    target_link_libraries(ceyear_status_query PRIVATE aircraft_core Qt${QT_VERSION_MAJOR}::Core "${NI_VISA_LIBRARY}")
endif()

add_executable(ceyear_capture_smoke
    src/tools/ceyear_capture_smoke.cpp
)
target_include_directories(ceyear_capture_smoke PRIVATE src)
target_link_libraries(ceyear_capture_smoke PRIVATE aircraft_core Qt${QT_VERSION_MAJOR}::Core)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test Network)
    if(QT_VERSION_MAJOR GREATER_EQUAL 6)
        qt_add_executable(aircraft_service_tests
            tests/service_tests.cpp
        )
    else()
        add_executable(aircraft_service_tests
            tests/service_tests.cpp
        )
    endif()
    target_include_directories(aircraft_service_tests PRIVATE src)
    target_link_libraries(aircraft_service_tests PRIVATE aircraft_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Test)
    add_test(NAME aircraft_service_tests COMMAND aircraft_service_tests)
    set_tests_properties(aircraft_service_tests PROPERTIES
        ENVIRONMENT_MODIFICATION "${AIRCRAFT_TEST_ENVIRONMENT_MODIFICATION}"
    )

    if(QT_VERSION_MAJOR GREATER_EQUAL 6)
        qt_add_executable(aircraft_ui_tests
            tests/ui_tests.cpp
            src/ui/AppTheme.cpp
            src/ui/MainWindow.cpp
            src/ui/pages/WorkflowPage.cpp
            src/ui/pages/CapturePage.cpp
            src/ui/pages/SweepPage.cpp
            src/ui/pages/DatasetPage.cpp
            src/ui/pages/PreprocessPage.cpp
            src/ui/pages/RecognitionPage.cpp
            src/ui/pages/TrainPage.cpp
            src/ui/widgets/IllustrationPanel.cpp
            src/ui/widgets/MetricCard.cpp
            src/ui/widgets/StatusBadge.cpp
            src/ui/widgets/StepNavigator.cpp
            src/ui/widgets/SpectrogramReviewWidget.cpp
            src/ui/widgets/WaterfallWidget.cpp
        )
    else()
        add_executable(aircraft_ui_tests
            tests/ui_tests.cpp
            src/ui/AppTheme.cpp
            src/ui/MainWindow.cpp
            src/ui/pages/WorkflowPage.cpp
            src/ui/pages/CapturePage.cpp
            src/ui/pages/SweepPage.cpp
            src/ui/pages/DatasetPage.cpp
            src/ui/pages/PreprocessPage.cpp
            src/ui/pages/RecognitionPage.cpp
            src/ui/pages/TrainPage.cpp
            src/ui/widgets/IllustrationPanel.cpp
            src/ui/widgets/MetricCard.cpp
            src/ui/widgets/StatusBadge.cpp
            src/ui/widgets/StepNavigator.cpp
            src/ui/widgets/SpectrogramReviewWidget.cpp
            src/ui/widgets/WaterfallWidget.cpp
        )
    endif()
    target_include_directories(aircraft_ui_tests PRIVATE src)
    target_link_libraries(aircraft_ui_tests PRIVATE aircraft_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Test)
    add_custom_command(TARGET aircraft_ui_tests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:aircraft_ui_tests>/platforms"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${QT_PLATFORM_PLUGIN_DIR}" "$<TARGET_FILE_DIR:aircraft_ui_tests>/platforms"
    )
    add_test(NAME aircraft_ui_tests COMMAND aircraft_ui_tests)
    set_tests_properties(aircraft_ui_tests PROPERTIES
        ENVIRONMENT "${AIRCRAFT_UI_TEST_ENVIRONMENT}"
        ENVIRONMENT_MODIFICATION "${AIRCRAFT_TEST_ENVIRONMENT_MODIFICATION}"
    )
endif()

include(GNUInstallDirs)
