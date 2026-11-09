// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

// Runs the Windows backend against a real Notepad window, so CI checks that UI
// Automation finds elements inside a window and not only the window itself.

#include "widgets/capture/smartregiondetector.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <QDebug>
#include <QGuiApplication>
#include <QProcess>
#include <QScopeGuard>
#include <QScreen>
#include <QTest>

class WindowsSmartRegionTest : public QObject
{
    Q_OBJECT

private slots:
    void findsElementsInsideNotepad();
};

namespace {
HWND findNotepad(DWORD processId)
{
    struct Search
    {
        DWORD processId;
        HWND window;
    } search{ processId, nullptr };
    EnumWindows(
      [](HWND window, LPARAM value) -> BOOL {
          auto* search = reinterpret_cast<Search*>(value);
          DWORD owner = 0;
          GetWindowThreadProcessId(window, &owner);
          if (owner == search->processId && IsWindowVisible(window) &&
              GetAncestor(window, GA_ROOT) == window) {
              search->window = window;
              return FALSE;
          }
          return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
    return search.window;
}
}

void WindowsSmartRegionTest::findsElementsInsideNotepad()
{
    QProcess notepad;
    notepad.start(QStringLiteral("notepad.exe"));
    if (!notepad.waitForStarted(10000)) {
        QSKIP("Notepad is not available");
    }
    const auto cleanup = qScopeGuard([&notepad]() { notepad.kill(); });

    // Newer Notepad versions hand over to another process, so fall back to
    // any Notepad window.
    HWND window = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
      (window = findNotepad(static_cast<DWORD>(notepad.processId()))) ||
        (window = FindWindowW(L"Notepad", nullptr)),
      15000);
    SetWindowPos(window, HWND_TOP, 100, 100, 800, 600, SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    QTest::qWait(1000);

    RECT bounds{};
    GetWindowRect(window, &bounds);
    // The text area fills the middle of the window.
    const QPoint native((bounds.left + bounds.right) / 2,
                        (bounds.top + bounds.bottom) / 2);
    const qreal scale = QGuiApplication::primaryScreen()->devicePixelRatio();
    const QPoint logical(qRound(native.x() / scale),
                         qRound(native.y() / scale));

    auto detector = SmartRegionDetector::create();
    QVERIFY(detector);
    detector->snapshotWindows();
    // UI Automation starts on the worker thread.
    QTRY_VERIFY(detector->elementDetectionAvailable());

    // The first lookups may only find the window while UI Automation warms up
    // in the background, like the capture widget's follow-up lookups.
    QList<QRect> regions;
    QTRY_VERIFY_WITH_TIMEOUT(
      (regions = detector->regionsAt(logical)).size() > 1, 10000);
    qInfo() << "Regions under" << logical << regions;
    for (const QRect& region : regions) {
        QVERIFY(region.contains(logical));
    }
    // Ordered from the deepest element to the window.
    QVERIFY(regions.last().contains(regions.first()));
}

QTEST_MAIN(WindowsSmartRegionTest)

#include "windowssmartregiontest.moc"
