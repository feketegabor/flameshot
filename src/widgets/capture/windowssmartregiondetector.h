// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include "smartregiondetector.h"

#include <memory>

class WindowsSmartRegionDetector : public SmartRegionDetector
{
public:
    WindowsSmartRegionDetector();
    ~WindowsSmartRegionDetector() override;

    void refresh() override;
    QList<SmartRegionCandidate> candidatesAt(
      const QPoint& globalPosition) override;
    bool accessibilityAvailable() const override;
    void requestAccessibilityPermission() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
