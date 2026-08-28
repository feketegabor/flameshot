// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "smartregiondetector.h"

#if defined(Q_OS_MACOS)
#include "macossmartregiondetector.h"
#endif

namespace {
#if !defined(Q_OS_MACOS)
class NullSmartRegionDetector : public SmartRegionDetector
{
public:
    void refresh() override {}

    QList<SmartRegionCandidate> candidatesAt(const QPoint&) override
    {
        return {};
    }

    bool accessibilityAvailable() const override { return false; }
    void requestAccessibilityPermission() override {}
};
#endif
}

std::unique_ptr<SmartRegionDetector> SmartRegionDetector::create()
{
#if defined(Q_OS_MACOS)
    return std::make_unique<MacOsSmartRegionDetector>();
#else
    return std::make_unique<NullSmartRegionDetector>();
#endif
}
