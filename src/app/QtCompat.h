#pragma once

#include <QtGlobal>

#define CPP_APP_QT_AT_LEAST(major, minor, patch) (QT_VERSION >= QT_VERSION_CHECK(major, minor, patch))
#define CPP_APP_QT_51210_OR_NEWER CPP_APP_QT_AT_LEAST(5, 12, 10)
#define CPP_APP_QT_6_OR_NEWER CPP_APP_QT_AT_LEAST(6, 0, 0)
