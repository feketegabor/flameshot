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
    void findsElementsInsideWindow_data();
    void findsElementsInsideWindow();
};

namespace {
HWND findWindow(DWORD processId, const wchar_t* windowClass)
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
    // Some programs hand over to another process, e.g. Explorer to the shell.
    return search.window ? search.window : FindWindowW(windowClass, nullptr);
}
}

void WindowsSmartRegionTest::findsElementsInsideWindow_data()
{
    QTest::addColumn<QString>("program");
    QTest::addColumn<QStringList>("arguments");
    QTest::addColumn<QString>("windowClass");
    QTest::addColumn<int>("minimumRegions");

    // A text area directly inside a window.
    QTest::newRow("notepad") << QStringLiteral("notepad.exe") << QStringList()
                             << QStringLiteral("Notepad") << 2;
    // A file list with thousands of items several levels deep: too slow for a
    // lookup that walks every element one call at a time.
    QTest::newRow("explorer")
      << QStringLiteral("explorer.exe")
      << QStringList{ QStringLiteral("C:\\Windows\\System32") }
      << QStringLiteral("CabinetWClass") << 3;
}

void WindowsSmartRegionTest::findsElementsInsideWindow()
{
    QFETCH(QString, program);
    QFETCH(QStringList, arguments);
    QFETCH(QString, windowClass);
    QFETCH(int, minimumRegions);

    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) {
        QSKIP("The program is not available");
    }
    HWND window = nullptr;
    const auto cleanup = qScopeGuard([&]() {
        if (window) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        process.kill();
    });
    QTRY_VERIFY_WITH_TIMEOUT(
      (window =
         findWindow(static_cast<DWORD>(process.processId()),
                    reinterpret_cast<const wchar_t*>(windowClass.utf16()))),
      15000);
    SetWindowPos(window, HWND_TOP, 100, 100, 900, 650, SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    QTest::qWait(1500);

    RECT bounds{};
    GetWindowRect(window, &bounds);
    // The main content fills the middle of the window.
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

    // Ask repeatedly, like the capture widget does while the pointer rests:
    // the first lookups may only find the window.
    QList<QRect> regions;
    QTRY_VERIFY_WITH_TIMEOUT(
      (regions = detector->regionsAt(logical)).size() >= minimumRegions, 5000);
    qInfo() << "Regions under" << logical << regions;
    for (const QRect& region : regions) {
        QVERIFY(region.contains(logical));
    }
    // Ordered from the deepest element to the window.
    QVERIFY(regions.last().contains(regions.first()));
}

QTEST_MAIN(WindowsSmartRegionTest)

#include "windowssmartregiontest.moc"
