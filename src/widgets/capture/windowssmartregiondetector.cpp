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

// UI Automation calls run on a worker thread. A request is abandoned after
// its budget, and the GUI thread waits slightly longer before falling back
// to the window, so a hung provider cannot freeze the overlay.
constexpr std::chrono::milliseconds REQUEST_BUDGET(200);
constexpr std::chrono::milliseconds GUI_WAIT(250);
constexpr DWORD PROVIDER_TIMEOUT_MS = 80;
constexpr int MAX_DEPTH = 32;
constexpr int MAX_VISITED_ELEMENTS = 1024;
constexpr int MAX_SIBLINGS = 512;
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

    const QString deviceName = QString::fromWCharArray(info.szDevice);
    auto screenIterator = std::find_if(
      context->unmatchedScreens.begin(),
      context->unmatchedScreens.end(),
      [&deviceName](QScreen* screen) {
          return screen->name().compare(deviceName, Qt::CaseInsensitive) == 0;
      });
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

class AutomationWorker
{
private:
    using Clock = std::chrono::steady_clock;

    struct Request
    {
        quint64 id;
        HWND window;
        POINT position;
        Clock::time_point deadline;
    };

    struct State
    {
        std::mutex mutex;
        std::condition_variable requestReady;
        std::condition_variable resultReady;
        std::optional<Request> pendingRequest;
        QList<QRect> result;
        quint64 completedRequestId{ 0 };
        bool initializationFailed{ false };
        std::atomic<quint64> latestRequestId{ 0 };
        std::atomic_bool stopping{ false };
        std::atomic_bool available{ false };
    };

public:
    AutomationWorker()
      : m_state(std::make_unique<State>())
      , m_thread([state = m_state.get()]() { run(state); })
    {}

    ~AutomationWorker()
    {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->stopping.store(true);
            m_state->latestRequestId.fetch_add(1);
        }
        m_state->requestReady.notify_one();
        // Native per-call limits and request cancellation bound normal UIA
        // shutdown; all interfaces stay on their owning MTA thread.
        m_thread.join();
    }

    QList<QRect> elementChainAt(HWND window, const POINT& position)
    {
        std::unique_lock lock(m_state->mutex);
        if (m_state->initializationFailed || m_state->stopping.load()) {
            return {};
        }
        const quint64 requestId = m_state->latestRequestId.fetch_add(1) + 1;
        m_state->pendingRequest =
          Request{ requestId, window, position, Clock::now() + REQUEST_BUDGET };
        m_state->requestReady.notify_one();
        if (!m_state->resultReady.wait_for(
              lock,
              GUI_WAIT,
              [this, requestId]() {
                  return m_state->stopping.load() ||
                         m_state->initializationFailed ||
                         m_state->completedRequestId == requestId;
              }) ||
            m_state->completedRequestId != requestId) {
            // Invalidate timed-out work as well as superseded requests.
            quint64 expected = requestId;
            m_state->latestRequestId.compare_exchange_strong(expected,
                                                             requestId + 1);
            return {};
        }
        return m_state->result;
    }

    bool available() const { return m_state->available.load(); }

private:
    static bool active(const State& state, const Request& request)
    {
        return !state.stopping.load() &&
               state.latestRequestId.load() == request.id &&
               Clock::now() < request.deadline;
    }

    static bool elementRectAt(const State& state,
                              const Request& request,
                              IUIAutomationElement* element,
                              RECT* rect)
    {
        if (!active(state, request) ||
            FAILED(element->get_CurrentBoundingRectangle(rect)) ||
            !validNativeRect(*rect) ||
            !containsPoint(*rect, request.position) ||
            !active(state, request)) {
            return false;
        }

        BOOL offscreen = TRUE;
        return SUCCEEDED(element->get_CurrentIsOffscreen(&offscreen)) &&
               !offscreen && active(state, request);
    }

    struct ElementHit
    {
        ComPtr<IUIAutomationElement> element;
        int depth;
        qint64 area;
    };

    static ElementHit deepestHitAt(const State& state,
                                   const Request& request,
                                   IUIAutomationTreeWalker* walker,
                                   IUIAutomationElement* element,
                                   const RECT& geometry,
                                   int depth,
                                   int* visited)
    {
        ElementHit best{
            element,
            depth,
            static_cast<qint64>(geometry.right - geometry.left) *
              (geometry.bottom - geometry.top),
        };
        if (depth >= MAX_DEPTH || *visited >= MAX_VISITED_ELEMENTS ||
            !active(state, request)) {
            return best;
        }

        ComPtr<IUIAutomationElement> child;
        if (FAILED(walker->GetFirstChildElement(element, &child))) {
            return best;
        }

        for (int siblingCount = 0;
             child && siblingCount < MAX_SIBLINGS &&
             *visited < MAX_VISITED_ELEMENTS && active(state, request);
             ++siblingCount) {
            ++*visited;
            RECT childRect{};
            if (elementRectAt(state, request, child.Get(), &childRect)) {
                ElementHit candidate = deepestHitAt(state,
                                                    request,
                                                    walker,
                                                    child.Get(),
                                                    childRect,
                                                    depth + 1,
                                                    visited);
                if (candidate.depth > best.depth ||
                    (candidate.depth == best.depth &&
                     candidate.area < best.area)) {
                    best = std::move(candidate);
                }
            }

            if (!active(state, request)) {
                break;
            }
            ComPtr<IUIAutomationElement> next;
            if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) {
                break;
            }
            child = std::move(next);
        }
        return best;
    }

    static ComPtr<IUIAutomationElement> deepestElementAt(
      const State& state,
      const Request& request,
      IUIAutomationTreeWalker* walker,
      IUIAutomationElement* root)
    {
        RECT rootRect{};
        if (!elementRectAt(state, request, root, &rootRect)) {
            return root;
        }

        int visited = 0;
        return deepestHitAt(state, request, walker, root, rootRect, 0, &visited)
          .element;
    }

    static QList<QRect> collectElementChain(const State& state,
                                            const Request& request,
                                            IUIAutomation* automation,
                                            IUIAutomationTreeWalker* walker)
    {
        if (!IsWindow(request.window) || !active(state, request)) {
            return {};
        }

        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(request.window, &root)) ||
            !root || !active(state, request)) {
            return {};
        }

        ComPtr<IUIAutomationElement> current =
          deepestElementAt(state, request, walker, root.Get());
        QList<QRect> geometries;
        for (int depth = 0;
             current && depth < MAX_DEPTH && active(state, request);
             ++depth) {
            RECT geometry{};
            if (elementRectAt(state, request, current.Get(), &geometry)) {
                geometries.append(rectFromNative(geometry));
            }

            if (!active(state, request)) {
                return {};
            }
            BOOL isRoot = FALSE;
            if (SUCCEEDED(automation->CompareElements(
                  current.Get(), root.Get(), &isRoot)) &&
                isRoot) {
                break;
            }

            if (!active(state, request)) {
                return {};
            }
            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.Get(), &parent)) ||
                !parent) {
                break;
            }
            current = std::move(parent);
        }
        return active(state, request) ? geometries : QList<QRect>{};
    }

    static void run(State* state)
    {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            std::lock_guard lock(state->mutex);
            state->initializationFailed = true;
            state->resultReady.notify_all();
            return;
        }

        ComPtr<IUIAutomation2> automation;
        ComPtr<IUIAutomationTreeWalker> walker;
        // CUIAutomation8 exposes the documented provider timeout interface.
        // If those limits cannot be installed, retain window-only snapping.
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8,
                                       nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&automation))) &&
            SUCCEEDED(automation->put_ConnectionTimeout(PROVIDER_TIMEOUT_MS)) &&
            SUCCEEDED(
              automation->put_TransactionTimeout(PROVIDER_TIMEOUT_MS)) &&
            SUCCEEDED(automation->get_ControlViewWalker(&walker)) && walker) {
            state->available.store(true);
        }

        for (;;) {
            Request request{};
            {
                std::unique_lock lock(state->mutex);
                state->requestReady.wait(lock, [&state]() {
                    return state->stopping.load() ||
                           state->pendingRequest.has_value();
                });
                if (state->stopping.load()) {
                    break;
                }
                request = *state->pendingRequest;
                state->pendingRequest.reset();
            }

            QList<QRect> result;
            if (state->available.load()) {
                result = collectElementChain(
                  *state, request, automation.Get(), walker.Get());
            }

            {
                std::lock_guard lock(state->mutex);
                if (state->latestRequestId.load() == request.id) {
                    state->result = std::move(result);
                    state->completedRequestId = request.id;
                }
            }
            state->resultReady.notify_all();
        }

        // Release apartment-bound interfaces before shutting down COM.
        walker.Reset();
        automation.Reset();
        CoUninitialize();
        state->available.store(false);
    }

    std::unique_ptr<State> m_state;
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
