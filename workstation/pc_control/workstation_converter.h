#pragma once

#include "workstation_job.h"

class QWidget;
QString openConversionDialog(QWidget* parent, const Job& nativeTemplate, const QString& workspace, const QByteArray& initialSource = {});
