// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "smartregiondetector.h"

#if defined(Q_OS_MACOS)
#include "macossmartregiondetector.h"
#elif defined(Q_OS_WIN)
#include "windowssmartregiondetector.h"
#endif

namespace {
#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)
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

QList<SmartRegionCandidate> makeSmartRegionCandidateChain(
  const QList<QRect>& elementGeometries,
  const QRect& windowGeometry,
  const QPoint& globalPosition)
{
    QList<SmartRegionCandidate> candidates;
    QList<QRect> seen;
    const QRect window = windowGeometry.normalized();

    if (!window.isValid() || !window.contains(globalPosition)) {
        return candidates;
    }

    for (const QRect& elementGeometry : elementGeometries) {
        const QRect geometry =
          elementGeometry.normalized().intersected(window);
        if (geometry.width() < 4 || geometry.height() < 4 ||
            !geometry.contains(globalPosition) || seen.contains(geometry)) {
            continue;
        }
        candidates.append({ geometry });
        seen.append(geometry);
    }

    if (!seen.contains(window)) {
        candidates.append({ window });
    }
    return candidates;
}

std::unique_ptr<SmartRegionDetector> SmartRegionDetector::create()
{
#if defined(Q_OS_MACOS)
    return std::make_unique<MacOsSmartRegionDetector>();
#elif defined(Q_OS_WIN)
    return std::make_unique<WindowsSmartRegionDetector>();
#else
    return std::make_unique<NullSmartRegionDetector>();
#endif
}
