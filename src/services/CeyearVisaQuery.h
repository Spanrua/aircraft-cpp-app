#pragma once

#include <QString>
#include <QVector>

struct CeyearVisaQuery
{
    QString label;
    QString command;
    bool readOnly = true;
};

QVector<CeyearVisaQuery> defaultCeyearStatusQueries();
