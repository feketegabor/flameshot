// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QList>
#include <QPoint>
#include <QRect>

#include <memory>

// Finds the windows and UI elements under the pointer using native platform
// APIs, so that a click can select them without dragging a rectangle.
class SmartRegionDetector
{
public:
    virtual ~SmartRegionDetector() = default;

    // Record the visible windows. Call this before the capture overlay is
    // shown, so the overlay itself is not part of the snapshot.
    virtual void snapshotWindows() = 0;

    // Regions under globalPosition, in Qt global logical coordinates, ordered
    // from the deepest UI element to the window that contains it. Returns an
    // empty list when there is no window under the position.
    virtual QList<QRect> regionsAt(const QPoint& globalPosition) = 0;

    // False when only whole windows can be detected, for example because the
    // macOS Accessibility permission has not been granted.
    virtual bool elementDetectionAvailable() const { return true; }

    // Whether this build and session have a detector backend.
    static bool isSupported();

    // Returns nullptr when isSupported() is false.
    static std::unique_ptr<SmartRegionDetector> create();
};

// Turns raw element geometries (deepest first) into the region chain returned
// by SmartRegionDetector::regionsAt(): regions are clipped to the window, and
// tiny, duplicate or unrelated ones are dropped. The window always ends the
// chain.
QList<QRect> buildRegionChain(const QList<QRect>& elementGeometries,
                              const QRect& windowGeometry,
                              const QPoint& globalPosition);

#if defined(Q_OS_WIN)
// For a native Windows wheel message, sets naturalScroll to whether it comes
// from a touchpad that scrolls content opposite to the fingers (Windows'
// default). Other messages are ignored. Qt does not report this as an
// inverted wheel event, unlike on macOS.
void trackTouchpadScroll(const void* message, bool* naturalScroll);
#endif

#if defined(Q_OS_MACOS)
// UI element detection on macOS needs the Accessibility permission.
bool macOsAccessibilityTrusted();
void requestMacOsAccessibilityPermission();
#endif
