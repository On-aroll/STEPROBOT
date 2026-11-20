#pragma once

#include <QSettings>

inline QSettings workstationSettings() {
    // Production uses the native store; smoke tests select a temporary INI store.
    return QSettings(QSettings::defaultFormat(), QSettings::UserScope, "STEPROBOTICS", "StepWorkstation");
}
