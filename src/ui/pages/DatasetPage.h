#pragma once

#include "ui/pages/WorkflowPage.h"

class QTableWidget;
class QLabel;
class QShowEvent;

class DatasetPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit DatasetPage(QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;

private:
    void loadSamples();

    QLabel *m_emptyState = nullptr;
    QTableWidget *m_table = nullptr;
};
