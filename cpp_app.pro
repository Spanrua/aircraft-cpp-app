QT += core gui widgets network
CONFIG += c++17
TEMPLATE = app
TARGET = aircraft_qt

win32 {
    QMAKE_CXXFLAGS += /utf-8
}

SOURCES += \
    src/main.cpp \
    src/app/ProjectPaths.cpp \
    src/core/Records.cpp \
    src/services/AppStateStore.cpp \
    src/services/CapProbeService.cpp \
    src/services/CeyearIfIqPacketParser.cpp \
    src/services/CeyearIqReceiver.cpp \
    src/services/CeyearPscReceiver.cpp \
    src/services/CeyearPscSweep.cpp \
    src/services/PscWidebandAlertDetector.cpp \
    src/services/CeyearVisaQuery.cpp \
    src/services/PreprocessProcessRunner.cpp \
    src/services/RecognitionProcessRunner.cpp \
    src/services/SignalCleaningProcessRunner.cpp \
    src/services/TrainingProcessRunner.cpp \
    src/services/UsrpDeviceService.cpp \
    src/ui/AppTheme.cpp \
    src/ui/MainWindow.cpp \
    src/ui/pages/CapturePage.cpp \
    src/ui/pages/DatasetPage.cpp \
    src/ui/pages/PreprocessPage.cpp \
    src/ui/pages/RecognitionPage.cpp \
    src/ui/pages/SignalCleaningPage.cpp \
    src/ui/pages/SweepPage.cpp \
    src/ui/pages/TrainPage.cpp \
    src/ui/pages/WorkflowPage.cpp \
    src/ui/widgets/IllustrationPanel.cpp \
    src/ui/widgets/MetricCard.cpp \
    src/ui/widgets/StatusBadge.cpp \
    src/ui/widgets/StepNavigator.cpp \
    src/ui/widgets/SpectrogramReviewWidget.cpp \
    src/ui/widgets/WaterfallWidget.cpp

HEADERS += \
    src/app/QtCompat.h \
    src/app/ProjectPaths.h \
    src/core/Records.h \
    src/services/AppStateStore.h \
    src/services/CapProbeService.h \
    src/services/CeyearIfIqPacketParser.h \
    src/services/CeyearIqReceiver.h \
    src/services/CeyearPscReceiver.h \
    src/services/CeyearPscSweep.h \
    src/services/PscWidebandAlertDetector.h \
    src/services/CeyearVisaQuery.h \
    src/services/CeyearVisaService.h \
    src/services/PreprocessProcessRunner.h \
    src/services/RecognitionProcessRunner.h \
    src/services/SignalCleaningProcessRunner.h \
    src/services/TrainingProcessRunner.h \
    src/services/UsrpDeviceService.h \
    src/ui/AppTheme.h \
    src/ui/MainWindow.h \
    src/ui/pages/CapturePage.h \
    src/ui/pages/DatasetPage.h \
    src/ui/pages/PreprocessPage.h \
    src/ui/pages/RecognitionPage.h \
    src/ui/pages/SignalCleaningPage.h \
    src/ui/pages/SweepPage.h \
    src/ui/pages/TrainPage.h \
    src/ui/pages/WorkflowPage.h \
    src/ui/widgets/IllustrationPanel.h \
    src/ui/widgets/MetricCard.h \
    src/ui/widgets/StatusBadge.h \
    src/ui/widgets/StepNavigator.h \
    src/ui/widgets/SpectrogramReviewWidget.h \
    src/ui/widgets/WaterfallWidget.h

INCLUDEPATH += $$PWD/src

isEmpty(AIRCRAFT_ENABLE_UHD) {
    unix: AIRCRAFT_ENABLE_UHD = 0
    win32: AIRCRAFT_ENABLE_UHD = 1
}
isEmpty(AIRCRAFT_ENABLE_NI_VISA) {
    unix: AIRCRAFT_ENABLE_NI_VISA = 0
    win32: AIRCRAFT_ENABLE_NI_VISA = 1
}

isEmpty(UHD_ROOT) {
    UHD_ROOT = $$(UHD_ROOT)
}
isEmpty(BOOST_INCLUDE_DIR) {
    BOOST_INCLUDE_DIR = $$(BOOST_INCLUDE_DIR)
}

contains(AIRCRAFT_ENABLE_UHD, 1) {
    DEFINES += AIRCRAFT_HAS_UHD=1
    INCLUDEPATH += $$UHD_ROOT/include $$BOOST_INCLUDE_DIR
    LIBS += -L$$UHD_ROOT/lib -luhd
} else {
    DEFINES += AIRCRAFT_HAS_UHD=0
}

contains(AIRCRAFT_ENABLE_NI_VISA, 1) {
    DEFINES += AIRCRAFT_HAS_NI_VISA=1
    SOURCES += src/services/CeyearVisaService.cpp
    unix {
        isEmpty(NI_VISA_INCLUDE_DIR) {
            NI_VISA_INCLUDE_DIR = $$(NI_VISA_INCLUDE_DIR)
        }
        isEmpty(NI_VISA_LIB_DIR) {
            NI_VISA_LIB_DIR = $$(NI_VISA_LIB_DIR)
        }
        INCLUDEPATH += $$NI_VISA_INCLUDE_DIR
        LIBS += -L$$NI_VISA_LIB_DIR -lvisa
    }
    win32 {
    contains(QT_ARCH, x86_64) {
        NI_VISA_INCLUDE_DIR = C:/Program Files/IVI Foundation/VISA/Win64/Include
        NI_VISA_LIB_DIR = C:/Program Files/IVI Foundation/VISA/Win64/Lib_x64/msc
        NI_VISA_BIN_DIR = C:/Program Files/IVI Foundation/VISA/Win64/Bin
    } else {
        NI_VISA_INCLUDE_DIR = C:/Program Files/IVI Foundation/VISA/Win32/Include
        NI_VISA_LIB_DIR = C:/Program Files/IVI Foundation/VISA/Win32/Lib_x86/msc
        NI_VISA_BIN_DIR = C:/Program Files/IVI Foundation/VISA/Win32/Bin
    }
        INCLUDEPATH += $$NI_VISA_INCLUDE_DIR
        LIBS += -L$$NI_VISA_LIB_DIR -lvisa64
    }
} else {
    DEFINES += AIRCRAFT_HAS_NI_VISA=0
    SOURCES += src/services/CeyearVisaServiceStub.cpp
}

DISTFILES += \
    README.md \
    README.zh-CN.md \
    requirements.md \
    requirements-python.txt
