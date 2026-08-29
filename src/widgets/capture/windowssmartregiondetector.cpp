// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "windowssmartregiondetector.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <QGuiApplication>
#include <QScreen>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
struct ScreenMapping
{
    QRect nativeGeometry;
    QRect logicalGeometry;
};

struct MonitorEnumerationContext
{
    QList<ScreenMapping>* mappings;
    QList<QScreen*> unmatchedScreens;
};

QRect rectFromNative(const RECT& rect)
{
    return { rect.left,
             rect.top,
             rect.right - rect.left,
             rect.bottom - rect.top };
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

BOOL CALLBACK collectMonitor(HMONITOR monitor,
                             HDC,
                             LPRECT,
                             LPARAM contextValue)
{
    auto* context =
      reinterpret_cast<MonitorEnumerationContext*>(contextValue);
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

QRect toLogicalRect(const QRect& nativeRect,
                    const QList<ScreenMapping>& mappings)
{
    QRect logicalResult;
    for (const ScreenMapping& mapping : mappings) {
        const QRect nativePart = nativeRect.intersected(mapping.nativeGeometry);
        if (!nativePart.isValid() || mapping.nativeGeometry.width() == 0 ||
            mapping.nativeGeometry.height() == 0) {
            continue;
        }

        const double xScale = static_cast<double>(mapping.logicalGeometry.width()) /
                              mapping.nativeGeometry.width();
        const double yScale =
          static_cast<double>(mapping.logicalGeometry.height()) /
          mapping.nativeGeometry.height();
        const int left = static_cast<int>(std::floor(
          mapping.logicalGeometry.x() +
          (nativePart.x() - mapping.nativeGeometry.x()) * xScale));
        const int top = static_cast<int>(std::floor(
          mapping.logicalGeometry.y() +
          (nativePart.y() - mapping.nativeGeometry.y()) * yScale));
        const int right = static_cast<int>(std::ceil(
          mapping.logicalGeometry.x() +
          (nativePart.x() + nativePart.width() - mapping.nativeGeometry.x()) *
            xScale));
        const int bottom = static_cast<int>(std::ceil(
          mapping.logicalGeometry.y() +
          (nativePart.y() + nativePart.height() - mapping.nativeGeometry.y()) *
            yScale));
        const QRect logicalPart(left, top, right - left, bottom - top);
        logicalResult = logicalResult.isValid()
                          ? logicalResult.united(logicalPart)
                          : logicalPart;
    }
    return logicalResult.isValid() ? logicalResult : nativeRect;
}

POINT toNativePoint(const QPoint& logicalPoint,
                    const QList<ScreenMapping>& mappings)
{
    for (const ScreenMapping& mapping : mappings) {
        if (!mapping.logicalGeometry.contains(logicalPoint) ||
            mapping.logicalGeometry.width() == 0 ||
            mapping.logicalGeometry.height() == 0) {
            continue;
        }

        const double xScale = static_cast<double>(mapping.nativeGeometry.width()) /
                              mapping.logicalGeometry.width();
        const double yScale =
          static_cast<double>(mapping.nativeGeometry.height()) /
          mapping.logicalGeometry.height();
        return { mapping.nativeGeometry.x() +
                   static_cast<LONG>(std::lround(
                     (logicalPoint.x() - mapping.logicalGeometry.x()) * xScale)),
                 mapping.nativeGeometry.y() +
                   static_cast<LONG>(std::lround(
                     (logicalPoint.y() - mapping.logicalGeometry.y()) *
                     yScale)) };
    }
    return { logicalPoint.x(), logicalPoint.y() };
}

class AutomationWorker
{
public:
    AutomationWorker()
      : m_thread(&AutomationWorker::run, this)
    {}

    ~AutomationWorker()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        m_requestReady.notify_one();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    QList<QRect> elementChainAt(HWND window, const POINT& position)
    {
        std::unique_lock lock(m_mutex);
        const quint64 requestId = ++m_nextRequestId;
        m_pendingRequest = Request{ requestId, window, position };
        m_requestReady.notify_one();
        if (!m_resultReady.wait_for(
              lock,
              std::chrono::milliseconds(50),
              [this, requestId]() {
                  return m_stopping || m_completedRequestId == requestId;
              }) ||
            m_completedRequestId != requestId) {
            return {};
        }
        return m_result;
    }

    bool available() const { return m_available.load(); }

private:
    struct Request
    {
        quint64 id;
        HWND window;
        POINT position;
    };

    static bool elementRectAt(IUIAutomationElement* element,
                              const POINT& position,
                              RECT* rect)
    {
        BOOL offscreen = TRUE;
        if (FAILED(element->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
            FAILED(element->get_CurrentBoundingRectangle(rect)) ||
            !validNativeRect(*rect)) {
            return false;
        }
        return containsPoint(*rect, position);
    }

    static ComPtr<IUIAutomationElement> deepestElementAt(
      IUIAutomationTreeWalker* walker,
      IUIAutomationElement* root,
      const POINT& position)
    {
        ComPtr<IUIAutomationElement> current = root;
        for (int depth = 0; depth < 32; ++depth) {
            ComPtr<IUIAutomationElement> child;
            if (FAILED(walker->GetFirstChildElement(current.Get(), &child)) ||
                !child) {
                break;
            }

            ComPtr<IUIAutomationElement> bestChild;
            qint64 bestArea = std::numeric_limits<qint64>::max();
            for (int siblingCount = 0; child && siblingCount < 512;
                 ++siblingCount) {
                RECT childRect{};
                if (elementRectAt(child.Get(), position, &childRect)) {
                    const qint64 area =
                      static_cast<qint64>(childRect.right - childRect.left) *
                      (childRect.bottom - childRect.top);
                    if (area < bestArea) {
                        bestArea = area;
                        bestChild = child;
                    }
                }

                ComPtr<IUIAutomationElement> next;
                if (FAILED(
                      walker->GetNextSiblingElement(child.Get(), &next))) {
                    break;
                }
                child = std::move(next);
            }

            if (!bestChild) {
                break;
            }
            current = std::move(bestChild);
        }
        return current;
    }

    static QList<QRect> collectElementChain(IUIAutomation* automation,
                                            IUIAutomationTreeWalker* walker,
                                            HWND window,
                                            const POINT& position)
    {
        if (!IsWindow(window)) {
            return {};
        }

        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(window, &root)) || !root) {
            return {};
        }

        ComPtr<IUIAutomationElement> current =
          deepestElementAt(walker, root.Get(), position);
        QList<QRect> geometries;
        for (int depth = 0; current && depth < 32; ++depth) {
            RECT geometry{};
            if (elementRectAt(current.Get(), position, &geometry)) {
                geometries.append(rectFromNative(geometry));
            }

            BOOL isRoot = FALSE;
            if (SUCCEEDED(
                  automation->CompareElements(current.Get(), root.Get(), &isRoot)) &&
                isRoot) {
                break;
            }

            ComPtr<IUIAutomationElement> parent;
            if (FAILED(walker->GetParentElement(current.Get(), &parent)) ||
                !parent) {
                break;
            }
            current = std::move(parent);
        }
        return geometries;
    }

    void run()
    {
        const HRESULT comResult =
          CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comResult)) {
            return;
        }

        ComPtr<IUIAutomation> automation;
        ComPtr<IUIAutomationTreeWalker> walker;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,
                                       nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&automation))) &&
            SUCCEEDED(automation->get_ControlViewWalker(&walker)) && walker) {
            ComPtr<IUIAutomation6> automation6;
            if (SUCCEEDED(automation.As(&automation6))) {
                automation6->put_ConnectionTimeout(80);
                automation6->put_TransactionTimeout(80);
            }
            m_available.store(true);
        }

        for (;;) {
            Request request{};
            {
                std::unique_lock lock(m_mutex);
                m_requestReady.wait(lock, [this]() {
                    return m_stopping || m_pendingRequest.has_value();
                });
                if (m_stopping) {
                    break;
                }
                request = *m_pendingRequest;
                m_pendingRequest.reset();
            }

            QList<QRect> result;
            if (automation && walker) {
                result = collectElementChain(automation.Get(),
                                             walker.Get(),
                                             request.window,
                                             request.position);
            }

            {
                std::lock_guard lock(m_mutex);
                m_result = std::move(result);
                m_completedRequestId = request.id;
            }
            m_resultReady.notify_all();
        }

        m_available.store(false);
        CoUninitialize();
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_requestReady;
    std::condition_variable m_resultReady;
    std::optional<Request> m_pendingRequest;
    QList<QRect> m_result;
    quint64 m_nextRequestId{ 0 };
    quint64 m_completedRequestId{ 0 };
    bool m_stopping{ false };
    std::atomic_bool m_available{ false };
    std::thread m_thread;
};
}

struct WindowsSmartRegionDetector::Impl
{
    struct Window
    {
        HWND handle;
        QRect logicalGeometry;
    };

    QList<Window> windows;
    QList<ScreenMapping> screenMappings;
    AutomationWorker automation;

    static BOOL CALLBACK collectWindow(HWND window, LPARAM contextValue)
    {
        auto* self = reinterpret_cast<Impl*>(contextValue);
        self->appendWindow(window);
        return TRUE;
    }

    void appendWindow(HWND window)
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

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(window,
                                            DWMWA_CLOAKED,
                                            &cloaked,
                                            sizeof(cloaked))) &&
            cloaked != 0) {
            return;
        }

        const LONG_PTR extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if ((extendedStyle & WS_EX_LAYERED) != 0) {
            BYTE alpha = 255;
            DWORD flags = 0;
            if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) &&
                (flags & LWA_ALPHA) != 0 && alpha == 0) {
                return;
            }
        }

        RECT nativeRect{};
        if (FAILED(DwmGetWindowAttribute(window,
                                         DWMWA_EXTENDED_FRAME_BOUNDS,
                                         &nativeRect,
                                         sizeof(nativeRect))) &&
            !GetWindowRect(window, &nativeRect)) {
            return;
        }
        if (!validNativeRect(nativeRect) || nativeRect.right - nativeRect.left < 20 ||
            nativeRect.bottom - nativeRect.top < 20) {
            return;
        }

        const QRect nativeGeometry = rectFromNative(nativeRect);
        const QRect logicalGeometry =
          toLogicalRect(nativeGeometry, screenMappings);
        if (logicalGeometry.width() < 20 || logicalGeometry.height() < 20) {
            return;
        }
        windows.append({ window, logicalGeometry });
    }

    const Window* windowAt(const QPoint& position) const
    {
        const auto found =
          std::find_if(windows.cbegin(), windows.cend(), [&position](const Window& window) {
              return window.logicalGeometry.contains(position);
          });
        return found == windows.cend() ? nullptr : &(*found);
    }
};

WindowsSmartRegionDetector::WindowsSmartRegionDetector()
  : m_impl(std::make_unique<Impl>())
{}

WindowsSmartRegionDetector::~WindowsSmartRegionDetector() = default;

void WindowsSmartRegionDetector::refresh()
{
    m_impl->windows.clear();
    m_impl->screenMappings.clear();

    MonitorEnumerationContext context{ &m_impl->screenMappings,
                                       QGuiApplication::screens() };
    EnumDisplayMonitors(
      nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&context));
    EnumWindows(Impl::collectWindow, reinterpret_cast<LPARAM>(m_impl.get()));
}

QList<SmartRegionCandidate> WindowsSmartRegionDetector::candidatesAt(
  const QPoint& globalPosition)
{
    const Impl::Window* window = m_impl->windowAt(globalPosition);
    if (!window) {
        return {};
    }

    const POINT nativePosition =
      toNativePoint(globalPosition, m_impl->screenMappings);
    const QList<QRect> nativeElements =
      m_impl->automation.elementChainAt(window->handle, nativePosition);
    QList<QRect> logicalElements;
    logicalElements.reserve(nativeElements.size());
    for (const QRect& nativeElement : nativeElements) {
        logicalElements.append(
          toLogicalRect(nativeElement, m_impl->screenMappings));
    }

    return makeSmartRegionCandidateChain(
      logicalElements, window->logicalGeometry, globalPosition);
}

bool WindowsSmartRegionDetector::accessibilityAvailable() const
{
    return m_impl->automation.available();
}

void WindowsSmartRegionDetector::requestAccessibilityPermission() {}
