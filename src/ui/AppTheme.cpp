#include "ui/AppTheme.h"

#include <QApplication>

void AppTheme::apply(QApplication &app)
{
    app.setStyle(QStringLiteral("Fusion"));
    app.setStyleSheet(QStringLiteral(R"(
        QWidget {
            background: #101820;
            color: #e7edf4;
            font-size: 14px;
        }

        QMainWindow, QStackedWidget {
            background: #101820;
        }

        QFrame#SystemBar {
            background: #151f2a;
            border: 1px solid rgba(148, 163, 184, 36);
            border-radius: 12px;
        }

        QLabel#AppTitle {
            font-size: 21px;
            font-weight: 600;
            color: #f8fafc;
        }

        QLabel#CurrentStepLabel, QLabel#ClockLabel, QLabel#DataPathLabel {
            color: #9fb0c1;
        }

        QLabel#DataPathLabel {
            padding: 7px 10px;
            border: 1px solid rgba(148, 163, 184, 30);
            border-radius: 999px;
            background: rgba(255, 255, 255, 14);
        }

        QLabel#PageTitle {
            font-size: 24px;
            font-weight: 600;
            color: #f8fafc;
        }

        QLabel#PageSubtitle {
            color: #9fb0c1;
            font-size: 13px;
        }

        QLabel#EmptyState {
            color: #9fb0c1;
            background: #151f2a;
            border: 1px dashed rgba(148, 163, 184, 48);
            border-radius: 10px;
            padding: 20px;
        }

        QPushButton {
            background: #182432;
            border: 1px solid rgba(148, 163, 184, 42);
            border-radius: 9px;
            padding: 9px 15px;
            color: #dbe7f2;
        }

        QPushButton:hover {
            background: #1f2d3d;
            border-color: rgba(147, 197, 253, 95);
        }

        QPushButton:checked {
            background: #1d4f78;
            border-color: #60a5fa;
            color: #ffffff;
        }

        QPushButton:disabled {
            background: #141b24;
            border-color: rgba(148, 163, 184, 22);
            color: #64748b;
        }

        QPushButton[primary="true"] {
            background: #3b82f6;
            border-color: #60a5fa;
            color: #ffffff;
            font-weight: 600;
        }

        QPushButton[primary="true"]:hover {
            background: #2563eb;
            border-color: #93c5fd;
        }

        QPushButton[secondary="true"] {
            background: rgba(255, 255, 255, 10);
            color: #cbd8e6;
        }

        QPushButton[stepButton="true"] {
            text-align: left;
            padding-left: 16px;
            padding-right: 16px;
            border-radius: 10px;
            background: rgba(255, 255, 255, 12);
            border-color: rgba(148, 163, 184, 34);
        }

        QPushButton[stepButton="true"][completed="true"] {
            background: rgba(20, 184, 166, 32);
            border-color: rgba(45, 212, 191, 70);
            color: #ccfbf1;
        }

        QPushButton[stepButton="true"][pending="true"] {
            color: #91a4b7;
        }

        QPushButton[stepButton="true"]:checked {
            background: #1d4f78;
            border-color: #60a5fa;
            color: #ffffff;
        }

        QLineEdit, QComboBox, QPlainTextEdit {
            background: #0f1720;
            border: 1px solid rgba(148, 163, 184, 42);
            border-radius: 8px;
            padding: 8px 10px;
            selection-background-color: #2563eb;
        }

        QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus {
            border-color: #60a5fa;
        }

        QComboBox {
            min-height: 20px;
            padding-right: 28px;
        }

        QComboBox::drop-down {
            subcontrol-origin: padding;
            subcontrol-position: top right;
            width: 28px;
            border-left: 1px solid rgba(148, 163, 184, 32);
            border-top-right-radius: 8px;
            border-bottom-right-radius: 8px;
            background: #162231;
        }

        QComboBox::down-arrow {
            width: 0;
            height: 0;
            border-left: 5px solid transparent;
            border-right: 5px solid transparent;
            border-top: 6px solid #cbd8e6;
            margin-right: 8px;
        }

        QComboBox QAbstractItemView {
            background: #111b26;
            color: #e7edf4;
            border: 1px solid rgba(96, 165, 250, 95);
            selection-background-color: #1d4f78;
            selection-color: #ffffff;
            outline: 0;
        }

        QPlainTextEdit {
            min-height: 120px;
        }

        QTableWidget {
            background: #111b26;
            alternate-background-color: #162231;
            gridline-color: rgba(148, 163, 184, 25);
            border: 1px solid rgba(148, 163, 184, 34);
            border-radius: 10px;
            selection-background-color: #1d4f78;
            selection-color: #ffffff;
        }

        QTabWidget::pane {
            border: 1px solid rgba(148, 163, 184, 34);
            border-radius: 10px;
            background: #111b26;
            top: -1px;
        }

        QTabBar::tab {
            background: #182432;
            color: #9fb0c1;
            border: 1px solid rgba(148, 163, 184, 34);
            border-bottom: 0;
            padding: 9px 14px;
            min-width: 90px;
            margin-right: 4px;
            border-top-left-radius: 8px;
            border-top-right-radius: 8px;
        }

        QTabBar::tab:selected {
            background: #233448;
            color: #ffffff;
            border-color: rgba(96, 165, 250, 90);
        }

        QLabel[muted="true"] {
            color: #9fb0c1;
        }

        QLabel[summary="true"] {
            background: rgba(255, 255, 255, 10);
            border: 1px solid rgba(148, 163, 184, 30);
            border-radius: 8px;
            padding: 10px;
            color: #cbd8e6;
        }

        QLabel#UsrpCaptureValidationMessage, QLabel#PreprocessValidationMessage {
            background: rgba(255, 255, 255, 10);
            border: 1px solid rgba(148, 163, 184, 30);
            border-radius: 8px;
            padding: 9px 10px;
            color: #cbd8e6;
        }

        QLabel#UsrpCaptureValidationMessage[kind="good"], QLabel#PreprocessValidationMessage[kind="good"] {
            background: rgba(16, 185, 129, 28);
            border-color: rgba(16, 185, 129, 78);
            color: #a7f3d0;
        }

        QLabel#UsrpCaptureValidationMessage[kind="error"], QLabel#PreprocessValidationMessage[kind="error"] {
            background: rgba(239, 68, 68, 28);
            border-color: rgba(239, 68, 68, 78);
            color: #fecaca;
        }

        QLabel#CaptureTaskStateLabel, QLabel#PreprocessRunStateLabel {
            background: rgba(255, 255, 255, 10);
            border: 1px solid rgba(148, 163, 184, 30);
            border-radius: 999px;
            padding: 8px 11px;
            color: #cbd8e6;
        }

        QLabel#CaptureTaskStateLabel[state="running"], QLabel#PreprocessRunStateLabel[state="running"] {
            background: rgba(59, 130, 246, 30);
            border-color: rgba(96, 165, 250, 90);
            color: #dbeafe;
        }

        QLabel#CaptureTaskStateLabel[state="done"], QLabel#PreprocessRunStateLabel[state="done"] {
            background: rgba(16, 185, 129, 28);
            border-color: rgba(16, 185, 129, 78);
            color: #a7f3d0;
        }

        QLabel#CaptureTaskStateLabel[state="error"], QLabel#PreprocessRunStateLabel[state="error"] {
            background: rgba(239, 68, 68, 28);
            border-color: rgba(239, 68, 68, 78);
            color: #fecaca;
        }

        QHeaderView::section {
            background: #1d2a38;
            color: #eef5fb;
            padding: 9px 10px;
            border: 0;
            border-right: 1px solid rgba(148, 163, 184, 28);
            border-bottom: 1px solid rgba(148, 163, 184, 30);
            font-weight: 600;
        }

        QLabel#StatusBadge {
            border-radius: 999px;
            padding: 7px 11px;
            background: rgba(255, 255, 255, 14);
            border: 1px solid rgba(148, 163, 184, 34);
        }

        QLabel#StatusBadge[kind="warning"] {
            background: rgba(245, 158, 11, 28);
            border-color: rgba(245, 158, 11, 86);
            color: #fcd58d;
        }

        QLabel#StatusBadge[kind="good"] {
            background: rgba(16, 185, 129, 30);
            border-color: rgba(16, 185, 129, 86);
            color: #a7f3d0;
        }

        QLabel#StatusBadge[kind="danger"] {
            background: rgba(239, 68, 68, 30);
            border-color: rgba(239, 68, 68, 88);
            color: #fecaca;
        }

        QLabel#StatusBadge[kind="neutral"] {
            color: #cbd8e6;
        }

        QLabel#MetricTitle {
            color: #91a4b7;
        }

        QLabel#MetricValue {
            color: #f8fafc;
            font-size: 22px;
            font-weight: 600;
        }

        QFrame#MetricCard {
            background: #172330;
            border: 1px solid rgba(148, 163, 184, 34);
            border-radius: 12px;
        }

        QFrame#PreprocessSummaryPanel,
        QFrame#PreprocessPathParametersPanel,
        QFrame#PreprocessBasicParametersPanel,
        QFrame#PreprocessAdvancedParametersPanel {
            background: #111a24;
            border: 1px solid rgba(148, 163, 184, 22);
            border-radius: 8px;
        }

        QFrame#PreprocessInputColumn,
        QFrame#PreprocessParametersColumn,
        QFrame#PreprocessRunColumn {
            background: #131d28;
            border: 1px solid rgba(148, 163, 184, 28);
            border-radius: 8px;
        }

        QLabel[sectionTitle="true"] {
            color: #eef5fb;
            font-size: 16px;
            font-weight: 600;
        }

        QGroupBox {
            border: 1px solid rgba(148, 163, 184, 34);
            border-radius: 10px;
            margin-top: 10px;
            padding: 12px;
            color: #dbe7f2;
            background: #151f2a;
        }

        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
            color: #9fb0c1;
        }

        QStatusBar {
            background: #0f1720;
            color: #9fb0c1;
            border-top: 1px solid rgba(148, 163, 184, 22);
        }
    )"));
}
