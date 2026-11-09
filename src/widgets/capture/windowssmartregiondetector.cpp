// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

// Windows backend: top-level windows come from EnumWindows and DWM, and the
// UI elements inside them from UI Automation.

#include "smartregiondetector.h"
#include "smartregiongeometry.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
// UIAutomation requires Windows and OLE declarations in this order.
// clang-format off
#include <Windows.h>
#include <ole2.h>
#include <UIAutomation.h>
// clang-format on
#include <dwmapi.h>
#include <wrl/client.h>

#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
using SmartRegionGeometry::ScreenMapping;

constexpr int MIN_WINDOW_SIDE = 20;

// How long the GUI thread waits for a UI Automation lookup before showing
// the window, and how long the worker may keep looking in the background.
constexpr std::chrono::milliseconds GUI_WAIT(60);
constexpr std::chrono::milliseconds REQUEST_BUDGET(1500);
// Per call into another application.
constexpr DWORD PROVIDER_TIMEOUT_MS = 300;
constexpr int MAX_DEPTH = 32;
constexpr int MAX_VISITED_ELEMENTS = 2048;
using SmartRegionGeometry::toLogicalRect;

struct MonitorEnumerationContext
{
    QList<ScreenMapping>* mappings;
    QList<QScreen*> unmatchedScreens;
};

QRect rectFromNative(const RECT& rect)
{
    return {
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top
    };
}

bool validNativeRect(const RECT& rect)
{
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool containsPoint(const RECT& rect, const POINT& point)
{
    return point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM contextValue)
{
    auto* context = reinterpret_cast<MonitorEnumerationContext*>(contextValue);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    // Qt keeps screen origins in physical pixels on Windows, so the origin
    // identifies the screen; Qt 6 may report a friendly name instead of the
    // device name.
    const QString deviceName = QString::fromWCharArray(info.szDevice);
    const QPoint origin(info.rcMonitor.left, info.rcMonitor.top);
    auto screenIterator =
      std::find_if(context->unmatchedScreens.begin(),
                   context->unmatchedScreens.end(),
                   [&origin](QScreen* screen) {
                       return screen->geometry().topLeft() == origin;
                   });
    if (screenIterator == context->unmatchedScreens.end()) {
        screenIterator =
          std::find_if(context->unmatchedScreens.begin(),
                       context->unmatchedScreens.end(),
                       [&deviceName](QScreen* screen) {
                           return screen->name().compare(
                                    deviceName, Qt::CaseInsensitive) == 0;
                       });
    }
    if (screenIterator == context->unmatchedScreens.end()) {
        QScreen* primary = QGuiApplication::primaryScreen();
        screenIterator = std::find(context->unmatchedScreens.begin(),
                                   context->unmatchedScreens.end(),
                                   primary);
    }
    if (screenIterator == context->unmatchedScreens.end() &&
        !context->unmatchedScreens.isEmpty()) {
        screenIterator = context->unmatchedScreens.begin();
    }
    if (screenIterator == context->unmatchedScreens.end()) {
        return TRUE;
    }

    context->mappings->append(
      { rectFromNative(info.rcMonitor), (*screenIterator)->geometry() });
    context->unmatchedScreens.erase(screenIterator);
    return TRUE;
}

// Runs UI Automation on a thread of its own: calls into other processes can
// be slow, and a hung application must not freeze the overlay. The GUI thread
// only waits briefly. A slower lookup keeps running, and its result is kept,
// so asking again for the same window and position (the capture widget does
// while the pointer rests) returns it without starting over.
class AutomationWorker
{
public:
    AutomationWorker()
      : m_thread([this]() { run(); })
    {}

    ~AutomationWorker()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            ++m_generation;
        }
        m_wake.notify_one();
        m_thread.join();
    }

    // Element rectangles in physical pixels, from the deepest element under
    // the position to the window. Empty while the lookup is still running.
    QList<QRect> elementChainAt(HWND window, const POINT& position)
    {
        const Request request{ window, position };
        std::unique_lock lock(m_mutex);
        if (m_finished == request) {
            return m_result;
        }
        if (m_running != request && m_pending != request) {
            // A new position supersedes the lookup in progress.
            m_pending = request;
            ++m_generation;
            m_wake.notify_one();
        }
        m_done.wait_for(lock, GUI_WAIT, [&]() {
            return m_stopping || m_finished == request;
        });
        return m_finished == request ? m_result : QList<QRect>();
    }

    bool available() const { return m_available.load(); }

private:
    using Clock = std::chrono::steady_clock;

    struct Request
    {
        HWND window;
        POINT position;
        bool operator==(const Request& other) const
        {
            return window == other.window && position.x == other.position.x &&
                   position.y == other.position.y;
        }
        bool operator!=(const Request& other) const
        {
            return !(*this == other);
        }
    };

    struct Lookup
    {
        Request request;
        quint64 generation;
        Clock::time_point deadline;
    };

    bool active(const Lookup& lookup) const
    {
        return m_generation.load() == lookup.generation &&
               Clock::now() < lookup.deadline;
    }

    // Reads the bounding rectangle fetched with the element. Off-screen
    // elements and those not under the position are skipped.
    bool cachedRectAt(IUIAutomationElement* element,
                      const POINT& position,
                      RECT* rect) const
    {
        BOOL offscreen = TRUE;
        return SUCCEEDED(element->get_CachedBoundingRectangle(rect)) &&
               validNativeRect(*rect) && containsPoint(*rect, position) &&
               SUCCEEDED(element->get_CachedIsOffscreen(&offscreen)) &&
               !offscreen;
    }

    struct Search
    {
        const Lookup& lookup;
        QList<QRect> path;
        QList<QRect> deepest;
        int visited = 0;
    };

    // Siblings can overlap, so every branch under the position is searched;
    // the deepest element wins, and the smaller one on a tie. Each level is
    // fetched with its rectangles in a single call into the application.
    void descend(IUIAutomationElement* element, Search& search) const
    {
        const auto area = [](const QRect& rect) {
            return qint64(rect.width()) * rect.height();
        };
        if (search.path.size() > search.deepest.size() ||
            (search.path.size() == search.deepest.size() &&
             area(search.path.last()) < area(search.deepest.last()))) {
            search.deepest = search.path;
        }
        if (search.path.size() >= MAX_DEPTH ||
            search.visited >= MAX_VISITED_ELEMENTS || !active(search.lookup)) {
            return;
        }

        ComPtr<IUIAutomationElementArray> children;
        int count = 0;
        if (FAILED(element->FindAllBuildCache(TreeScope_Children,
                                              m_controlView.Get(),
                                              m_cacheRequest.Get(),
                                              &children)) ||
            !children || FAILED(children->get_Length(&count))) {
            return;
        }
        for (int i = 0; i < count && search.visited < MAX_VISITED_ELEMENTS &&
                        active(search.lookup);
             ++i, ++search.visited) {
            ComPtr<IUIAutomationElement> child;
            RECT rect{};
            if (SUCCEEDED(children->GetElement(i, &child)) && child &&
                cachedRectAt(
                  child.Get(), search.lookup.request.position, &rect)) {
                search.path.append(rectFromNative(rect));
                descend(child.Get(), search);
                search.path.removeLast();
            }
        }
    }

    // Returns false when the lookup ran out of time or was superseded.
    bool lookUp(const Lookup& lookup, QList<QRect>* chain) const
    {
        const Request& request = lookup.request;
        ComPtr<IUIAutomationElement> root;
        RECT rootRect{};
        if (!IsWindow(request.window) ||
            FAILED(m_automation->ElementFromHandleBuildCache(
              request.window, m_cacheRequest.Get(), &root)) ||
            !root || !cachedRectAt(root.Get(), request.position, &rootRect)) {
            chain->clear();
            return active(lookup);
        }
        Search search{ lookup, { rectFromNative(rootRect) }, {} };
        descend(root.Get(), search);
        if (!active(lookup)) {
            return false;
        }
        chain->clear();
        for (auto it = search.deepest.crbegin(); it != search.deepest.crend();
             ++it) {
            chain->append(*it);
        }
        return true;
    }

    bool initialize()
    {
        // CUIAutomation8 provides the provider timeouts.
        return SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8,
                                          nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&m_automation))) &&
               SUCCEEDED(
                 m_automation->put_ConnectionTimeout(PROVIDER_TIMEOUT_MS)) &&
               SUCCEEDED(
                 m_automation->put_TransactionTimeout(PROVIDER_TIMEOUT_MS)) &&
               SUCCEEDED(
                 m_automation->get_ControlViewCondition(&m_controlView)) &&
               SUCCEEDED(m_automation->CreateCacheRequest(&m_cacheRequest)) &&
               SUCCEEDED(m_cacheRequest->AddProperty(
                 UIA_BoundingRectanglePropertyId)) &&
               SUCCEEDED(
                 m_cacheRequest->AddProperty(UIA_IsOffscreenPropertyId));
    }

    void run()
    {
        const bool comReady =
          SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        m_available.store(comReady && initialize());

        std::unique_lock lock(m_mutex);
        for (;;) {
            m_wake.wait(lock, [this]() { return m_stopping || m_pending; });
            if (m_stopping) {
                break;
            }
            const Lookup lookup{ *m_pending,
                                 m_generation.load(),
                                 Clock::now() + REQUEST_BUDGET };
            m_running = m_pending;
            m_pending.reset();
            lock.unlock();

            QList<QRect> chain;
            const bool complete = available() && lookUp(lookup, &chain);

            lock.lock();
            m_running.reset();
            // Keep complete results only, so an expired lookup is retried.
            if (complete) {
                m_finished = lookup.request;
                m_result = std::move(chain);
            }
            m_done.notify_all();
        }
        m_done.notify_all();
        lock.unlock();

        // Release apartment-bound interfaces before shutting down COM.
        m_cacheRequest.Reset();
        m_controlView.Reset();
        m_automation.Reset();
        if (comReady) {
            CoUninitialize();
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_done;
    std::optional<Request> m_pending;
    std::optional<Request> m_running;
    std::optional<Request> m_finished;
    QList<QRect> m_result;
    bool m_stopping = false;
    // Changes whenever a lookup is superseded, which cancels it.
    std::atomic<quint64> m_generation{ 0 };
    std::atomic_bool m_available{ false };

    // Only used on the worker thread.
    ComPtr<IUIAutomation2> m_automation;
    ComPtr<IUIAutomationCondition> m_controlView;
    ComPtr<IUIAutomationCacheRequest> m_cacheRequest;

    std::thread m_thread;
};

class WindowsSmartRegionDetector : public SmartRegionDetector
{
public:
    void snapshotWindows() override;
    QList<QRect> regionsAt(const QPoint& globalPosition) override;
    bool elementDetectionAvailable() const override
    {
        return m_automation.available();
    }

private:
    struct Window
    {
        HWND handle;
        QRect nativeGeometry;
    };

    static BOOL CALLBACK collectWindow(HWND window, LPARAM detector)
    {
        reinterpret_cast<WindowsSmartRegionDetector*>(detector)->addWindow(
          window);
        return TRUE;
    }

    void addWindow(HWND window);

    // Front to back, in the order EnumWindows reports them.
    QList<Window> m_windows;
    QList<ScreenMapping> m_screens;
    AutomationWorker m_automation;
};

void WindowsSmartRegionDetector::snapshotWindows()
{
    m_windows.clear();
    m_screens.clear();

    MonitorEnumerationContext context{ &m_screens, QGuiApplication::screens() };
    EnumDisplayMonitors(
      nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&context));
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(this));
}

void WindowsSmartRegionDetector::addWindow(HWND window)
{
    if (!IsWindowVisible(window) || IsIconic(window) ||
        GetAncestor(window, GA_ROOT) != window ||
        window == GetDesktopWindow() || window == GetShellWindow()) {
        return;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return;
    }

    // Cloaked windows are hidden by DWM, e.g. on another virtual desktop.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(
          window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return;
    }

    const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((extendedStyle & WS_EX_LAYERED) != 0) {
        // Layered windows that are click-through or fully transparent are
        // overlays the user cannot see or interact with.
        if ((extendedStyle & WS_EX_TRANSPARENT) != 0) {
            return;
        }
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) &&
            (flags & LWA_ALPHA) != 0 && alpha == 0) {
            return;
        }
    }

    // The extended frame bounds exclude the invisible resize borders that
    // GetWindowRect includes on Windows 10 and later.
    RECT nativeRect{};
    if (FAILED(DwmGetWindowAttribute(window,
                                     DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &nativeRect,
                                     sizeof(nativeRect))) &&
        !GetWindowRect(window, &nativeRect)) {
        return;
    }
    if (!validNativeRect(nativeRect) ||
        nativeRect.right - nativeRect.left < MIN_WINDOW_SIDE ||
        nativeRect.bottom - nativeRect.top < MIN_WINDOW_SIDE) {
        return;
    }
    m_windows.append({ window, rectFromNative(nativeRect) });
}

QList<QRect> WindowsSmartRegionDetector::regionsAt(const QPoint& globalPosition)
{
    // Map through the pointer's screen only: with mixed DPI, parts of a
    // window on other screens would map to a different logical scale.
    const ScreenMapping* screen =
      SmartRegionGeometry::screenAt(globalPosition, m_screens);
    if (!screen) {
        return {};
    }
    const QPoint nativePoint =
      SmartRegionGeometry::toNativePoint(globalPosition, *screen);
    const auto window = std::find_if(
      m_windows.cbegin(), m_windows.cend(), [&](const Window& candidate) {
          return candidate.nativeGeometry.contains(nativePoint);
      });
    if (window == m_windows.cend()) {
        return {};
    }

    QList<QRect> elements;
    const POINT nativePosition{ nativePoint.x(), nativePoint.y() };
    for (const QRect& element :
         m_automation.elementChainAt(window->handle, nativePosition)) {
        elements.append(toLogicalRect(element, *screen));
    }
    return buildRegionChain(
      elements, toLogicalRect(window->nativeGeometry, *screen), globalPosition);
}
} // namespace

bool nativeSmartRegionDetectorSupported()
{
    return true;
}

std::unique_ptr<SmartRegionDetector> createNativeSmartRegionDetector()
{
    return std::make_unique<WindowsSmartRegionDetector>();
}
