cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(AircraftRfQt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_options(/utf-8)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

qt_standard_project_setup()

set(UHD_ROOT "D:/uhd" CACHE PATH "Root directory of UHD SDK")
find_path(UHD_INCLUDE_DIR
    NAMES uhd/usrp/multi_usrp.hpp
    PATHS "${UHD_ROOT}/include"
    NO_DEFAULT_PATH
)
find_library(UHD_LIBRARY
    NAMES uhd
    PATHS "${UHD_ROOT}/lib"
    NO_DEFAULT_PATH
)
if(NOT UHD_INCLUDE_DIR OR NOT UHD_LIBRARY)
    message(FATAL_ERROR "UHD SDK not found. Set UHD_ROOT to the installed UHD directory.")
endif()

find_path(BOOST_INCLUDE_DIR
    NAMES boost/core/noncopyable.hpp
    PATHS
        "$ENV{BOOST_ROOT}/include"
        "$ENV{CONDA_PREFIX}/Library/include"
        "E:/anaconda/Library/include"
        "C:/local/boost"
    NO_DEFAULT_PATH
)
if(NOT BOOST_INCLUDE_DIR)
    find_path(BOOST_INCLUDE_DIR NAMES boost/core/noncopyable.hpp)
endif()
if(NOT BOOST_INCLUDE_DIR)
    message(FATAL_ERROR "Boost headers not found. Set BOOST_INCLUDE_DIR to a directory containing boost/core/noncopyable.hpp.")
endif()

add_library(aircraft_core STATIC
    src/app/ProjectPaths.cpp
    src/core/Records.cpp
    src/services/AppStateStore.cpp
    src/services/CapProbeService.cpp
    src/services/PreprocessProcessRunner.cpp
    src/services/UsrpDeviceService.cpp
)

target_include_directories(aircraft_core PUBLIC src)
target_include_directories(aircraft_core PUBLIC "${UHD_INCLUDE_DIR}")
target_include_directories(aircraft_core PUBLIC "${BOOST_INCLUDE_DIR}")
target_link_libraries(aircraft_core PUBLIC Qt6::Core "${UHD_LIBRARY}")

qt_add_executable(aircraft_qt
    src/main.cpp
    src/ui/AppTheme.cpp
    src/ui/MainWindow.cpp
    src/ui/pages/WorkflowPage.cpp
    src/ui/pages/CapturePage.cpp
    src/ui/pages/DatasetPage.cpp
    src/ui/pages/PreprocessPage.cpp
    src/ui/pages/RecognitionPage.cpp
    src/ui/pages/TrainPage.cpp
    src/ui/widgets/IllustrationPanel.cpp
    src/ui/widgets/MetricCard.cpp
    src/ui/widgets/StatusBadge.cpp
    src/ui/widgets/StepNavigator.cpp
)

target_include_directories(aircraft_qt PRIVATE src)
target_link_libraries(aircraft_qt PRIVATE aircraft_core Qt6::Core Qt6::Gui Qt6::Widgets)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(aircraft_service_tests
        tests/service_tests.cpp
    )
    target_include_directories(aircraft_service_tests PRIVATE src)
    target_link_libraries(aircraft_service_tests PRIVATE aircraft_core Qt6::Core Qt6::Test)
    add_test(NAME aircraft_service_tests COMMAND aircraft_service_tests)

    qt_add_executable(aircraft_ui_tests
        tests/ui_tests.cpp
        src/ui/AppTheme.cpp
        src/ui/MainWindow.cpp
        src/ui/pages/WorkflowPage.cpp
        src/ui/pages/CapturePage.cpp
        src/ui/pages/DatasetPage.cpp
        src/ui/pages/PreprocessPage.cpp
        src/ui/pages/RecognitionPage.cpp
        src/ui/pages/TrainPage.cpp
        src/ui/widgets/IllustrationPanel.cpp
        src/ui/widgets/MetricCard.cpp
        src/ui/widgets/StatusBadge.cpp
        src/ui/widgets/StepNavigator.cpp
    )
    target_include_directories(aircraft_ui_tests PRIVATE src)
    target_link_libraries(aircraft_ui_tests PRIVATE aircraft_core Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    add_test(NAME aircraft_ui_tests COMMAND aircraft_ui_tests)
endif()

include(GNUInstallDirs)
