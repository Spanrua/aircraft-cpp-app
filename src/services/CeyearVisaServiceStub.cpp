#include "services/CeyearVisaService.h"

namespace
{
CeyearStatusResult unavailableStatus(const QString &resource)
{
    CeyearStatusResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    result.message = QStringLiteral("NI-VISA support is not available in this build.");
    return result;
}

CeyearCaptureResult unavailableCapture(const QString &resource)
{
    CeyearCaptureResult result;
    result.resource = resource.trimmed().isEmpty() ? defaultCeyearVisaResource() : resource.trimmed();
    result.message = QStringLiteral("NI-VISA/3943B support is not available in this build.");
    return result;
}

bool statusItemsContainCommandError(const QVector<CeyearStatusItem> &items)
{
    for (const auto &item : items) {
        if (!item.success) {
            continue;
        }
        const QString normalized = item.response.trimmed().toLower();
        if (normalized.contains(QStringLiteral("402"))
            || normalized.contains(QStringLiteral("cmd err"))) {
            return true;
        }
    }
    return false;
}
} // namespace

bool ceyearStatusContainsCommandError(const CeyearStatusResult &statusResult)
{
    return statusItemsContainCommandError(statusResult.items);
}

bool ceyearStatusContainsCommandError(const CeyearCaptureResult &captureResult)
{
    return statusItemsContainCommandError(captureResult.items);
}

bool ceyearStatusItemsAllSucceeded(const QVector<CeyearStatusItem> &items)
{
    if (items.isEmpty()) {
        return false;
    }
    for (const auto &item : items) {
        if (!item.success) {
            return false;
        }
    }
    return true;
}

CeyearStatusResult CeyearVisaService::queryStatus(const QString &resource) const
{
    return unavailableStatus(resource);
}

CeyearStatusResult CeyearVisaService::applyControl(const QString &resource, const CeyearControlConfig &) const
{
    return unavailableStatus(resource);
}

CeyearCaptureResult CeyearVisaService::configureCapturePath(const QString &resource, const CeyearCaptureConfig &) const
{
    return unavailableCapture(resource);
}

CeyearCaptureResult CeyearVisaService::startManualIqReport(const QString &resource, const CeyearCaptureConfig &) const
{
    return unavailableCapture(resource);
}

CeyearCaptureResult CeyearVisaService::triggerIqReport(const QString &resource, const CeyearCaptureConfig &) const
{
    return unavailableCapture(resource);
}

CeyearCaptureResult CeyearVisaService::startCapture(const QString &resource, const CeyearCaptureConfig &) const
{
    return unavailableCapture(resource);
}

CeyearCaptureResult CeyearVisaService::stopCapture(const QString &resource) const
{
    return unavailableCapture(resource);
}

void CeyearVisaService::requestStop() const
{
}

CeyearCaptureResult CeyearVisaService::startPscSweep(const QString &resource, const CeyearPscSweepConfig &) const
{
    return unavailableCapture(resource);
}

CeyearCaptureResult CeyearVisaService::stopPscSweep(const QString &resource) const
{
    return unavailableCapture(resource);
}
