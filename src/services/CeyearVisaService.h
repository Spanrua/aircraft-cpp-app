#pragma once

#include <QString>
#include <QVector>

#include "services/CeyearVisaQuery.h"

struct CeyearPscSweepConfig;

class ICeyearVisaService
{
public:
    virtual ~ICeyearVisaService() = default;
    virtual CeyearStatusResult queryStatus(const QString &resource) const = 0;
    virtual CeyearStatusResult applyControl(const QString &resource, const CeyearControlConfig &config) const = 0;
    virtual CeyearCaptureResult configureCapturePath(const QString &resource, const CeyearCaptureConfig &config) const = 0;
    virtual CeyearCaptureResult startManualIqReport(const QString &resource, const CeyearCaptureConfig &config) const = 0;
    virtual CeyearCaptureResult triggerIqReport(const QString &resource, const CeyearCaptureConfig &config) const = 0;
    virtual CeyearCaptureResult startCapture(const QString &resource, const CeyearCaptureConfig &config) const = 0;
    virtual CeyearCaptureResult stopCapture(const QString &resource) const = 0;
    virtual void requestStop() const = 0;
    virtual CeyearCaptureResult startPscSweep(const QString &resource, const CeyearPscSweepConfig &config) const = 0;
    virtual CeyearCaptureResult stopPscSweep(const QString &resource) const = 0;
};

class CeyearVisaService : public ICeyearVisaService
{
public:
    CeyearStatusResult queryStatus(const QString &resource) const override;
    CeyearStatusResult applyControl(const QString &resource, const CeyearControlConfig &config) const override;
    CeyearCaptureResult configureCapturePath(const QString &resource, const CeyearCaptureConfig &config) const override;
    CeyearCaptureResult startManualIqReport(const QString &resource, const CeyearCaptureConfig &config) const override;
    CeyearCaptureResult triggerIqReport(const QString &resource, const CeyearCaptureConfig &config) const override;
    CeyearCaptureResult startCapture(const QString &resource, const CeyearCaptureConfig &config) const override;
    CeyearCaptureResult stopCapture(const QString &resource) const override;
    void requestStop() const override;
    CeyearCaptureResult startPscSweep(const QString &resource, const CeyearPscSweepConfig &config) const override;
    CeyearCaptureResult stopPscSweep(const QString &resource) const override;
};

bool ceyearStatusContainsCommandError(const CeyearStatusResult &statusResult);
bool ceyearStatusContainsCommandError(const CeyearCaptureResult &captureResult);
bool ceyearStatusItemsAllSucceeded(const QVector<CeyearStatusItem> &items);
