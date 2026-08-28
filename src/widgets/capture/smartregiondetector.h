// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

#include <memory>

struct SmartRegionCandidate
{
    QRect geometry;
    QString description;
};

class SmartRegionDetector
{
public:
    virtual ~SmartRegionDetector() = default;

    // Capture the window stacking state before the Flameshot overlay appears.
    virtual void refresh() = 0;

    // Return candidates from the deepest UI element to its containing window.
    virtual QList<SmartRegionCandidate> candidatesAt(
      const QPoint& globalPosition) = 0;

    virtual bool accessibilityAvailable() const = 0;
    virtual void requestAccessibilityPermission() = 0;

    static std::unique_ptr<SmartRegionDetector> create();
};
