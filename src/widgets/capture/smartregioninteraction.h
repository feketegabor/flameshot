// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QPoint>

namespace SmartRegionInteraction {
constexpr int CLICK_DRAG_THRESHOLD = 3;

struct WheelDecision
{
    bool consumed;
    int direction;
};

constexpr WheelDecision wheelDecision(bool selectionVisible,
                                      bool smartRegionEnabled,
                                      int delta,
                                      bool inverted)
{
    if (selectionVisible || !smartRegionEnabled) {
        return { false, 0 };
    }

    // Candidate indices run from the deepest element toward its parents.
    const int physicalDelta = inverted ? -delta : delta;
    return { true,
             physicalDelta > 0 ? 1 : physicalDelta < 0 ? -1 : 0 };
}

inline bool startsManualDrag(const QPoint& pressPosition,
                             const QPoint& currentPosition)
{
    return (currentPosition - pressPosition).manhattanLength() >
           CLICK_DRAG_THRESHOLD;
}
}
