#pragma once

#include "core/Records.h"
#include "services/CandidateReviewService.h"
#include "ui/pages/WorkflowPage.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTableWidgetItem;
class MetricCard;
class SpectrogramReviewWidget;

class DatasetPage : public WorkflowPage
{
    Q_OBJECT

public:
    explicit DatasetPage(QWidget *parent = nullptr);
    bool hasSamples() const;
    bool hasDatasetVersion() const;

signals:
    void requestOpenTraining();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void loadSamples();
    void rebuildCandidateGroups();
    void rebuildCategoryTable();
    void loadVersionTable();
    void updateSelectionSummary();
    void generateDatasetVersion();
    void setAllCategoryChecks(Qt::CheckState state);
    void handleCategoryItemChanged(QTableWidgetItem *item);
    void updateReviewPanel();
    void applyReviewToSelection(const QString &reviewState);
    void applyRuleTriage();
    void applyLabelToSelection();
    QStringList selectedLabels() const;
    QVector<int> selectedSampleIndices() const;
    QVector<int> selectedCandidateIndices() const;
    QVector<int> sampleIndicesForCandidates(const QVector<int> &candidateIndices) const;
    bool isSampleEligible(const SampleRecord &sample) const;
    bool isPartialSample(const SampleRecord &sample) const;
    bool isReviewedSample(const SampleRecord &sample) const;
    bool saveSamplesToState(const QString &successMessage);
    void setPageMessage(const QString &message, const QString &kind = QString());

    QLabel *m_emptyState = nullptr;
    QLabel *m_pageMessage = nullptr;
    QLabel *m_reviewSelectionLabel = nullptr;
    MetricCard *m_totalSamplesMetric = nullptr;
    MetricCard *m_selectedSamplesMetric = nullptr;
    MetricCard *m_selectedCategoriesMetric = nullptr;
    MetricCard *m_versionCountMetric = nullptr;
    QLabel *m_selectionSummaryLabel = nullptr;
    QComboBox *m_labelCombo = nullptr;
    QTableWidget *m_categoryTable = nullptr;
    QTableWidget *m_table = nullptr;
    QTableWidget *m_versionTable = nullptr;
    QComboBox *m_stateFilter = nullptr;
    QCheckBox *m_includePartialCheck = nullptr;
    QCheckBox *m_includePendingReviewCheck = nullptr;
    QPushButton *m_generateVersionButton = nullptr;
    QPushButton *m_nextTrainingButton = nullptr;
    QPushButton *m_approveButton = nullptr;
    QPushButton *m_rejectButton = nullptr;
    QPushButton *m_pendingButton = nullptr;
    QPushButton *m_ruleTriageButton = nullptr;
    QPushButton *m_applyLabelButton = nullptr;
    QVector<SampleRecord> m_samples;
    QVector<CandidateReviewGroup> m_candidateGroups;
    QVector<DatasetVersionRecord> m_versions;
    bool m_loadingCategories = false;
    QLabel *m_readinessLabel = nullptr;
    QLabel *m_sampleDetailLabel = nullptr;
    QLabel *m_spectrogramPathLabel = nullptr;
    SpectrogramReviewWidget *m_spectrogramReviewWidget = nullptr;
};
