#pragma once

#include "core/Records.h"

#include <QString>
#include <QVector>

struct CandidateReviewGroup
{
    CandidateReviewRecord review;
    QVector<int> sampleIndices;
    QVector<int> representativeSampleIndices;
};

class CandidateReviewService
{
public:
    static QString reviewKey(const SampleRecord &sample);
    static QVector<CandidateReviewGroup> buildGroups(
        const QVector<SampleRecord> &samples,
        const QVector<CandidateReviewRecord> &persistedReviews = {});
    static bool matchesFilter(const CandidateReviewGroup &group, const QString &filter);
};

