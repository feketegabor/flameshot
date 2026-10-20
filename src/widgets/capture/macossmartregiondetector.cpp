// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "macossmartregiondetector.h"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>

#include <QCoreApplication>

#include <algorithm>
#include <cmath>

namespace {
template<typename T>
class CFRef
{
public:
    explicit CFRef(T value = nullptr)
      : m_value(value)
    {}

    ~CFRef() { reset(); }

    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;

    CFRef(CFRef&& other) noexcept
      : m_value(other.release())
    {}

    CFRef& operator=(CFRef&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    T get() const { return m_value; }

    T release()
    {
        T value = m_value;
        m_value = nullptr;
        return value;
    }

    void reset(T value = nullptr)
    {
        if (m_value) {
            CFRelease(m_value);
        }
        m_value = value;
    }

private:
    T m_value;
};

bool numberValue(CFDictionaryRef dictionary, CFStringRef key, int* value)
{
    const auto* number =
      static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
    return number && CFGetTypeID(number) == CFNumberGetTypeID() &&
           CFNumberGetValue(number, kCFNumberIntType, value);
}

bool numberValue(CFDictionaryRef dictionary, CFStringRef key, double* value)
{
    const auto* number =
      static_cast<CFNumberRef>(CFDictionaryGetValue(dictionary, key));
    return number && CFGetTypeID(number) == CFNumberGetTypeID() &&
           CFNumberGetValue(number, kCFNumberDoubleType, value);
}

bool boolValue(CFDictionaryRef dictionary, CFStringRef key, bool fallback)
{
    const auto* value =
      static_cast<CFBooleanRef>(CFDictionaryGetValue(dictionary, key));
    if (!value || CFGetTypeID(value) != CFBooleanGetTypeID()) {
        return fallback;
    }
    return CFBooleanGetValue(value);
}

QString stringValue(CFTypeRef value)
{
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) {
        return {};
    }

    CFStringRef string = static_cast<CFStringRef>(value);
    const CFIndex length = CFStringGetLength(string);
    if (length == 0) {
        return {};
    }

    QString result(length, Qt::Uninitialized);
    CFStringGetCharacters(string,
                          CFRangeMake(0, length),
                          reinterpret_cast<UniChar*>(result.data()));
    return result;
}

QRect qtRect(const CGRect& rect)
{
    const int left = static_cast<int>(std::floor(CGRectGetMinX(rect)));
    const int top = static_cast<int>(std::floor(CGRectGetMinY(rect)));
    const int right = static_cast<int>(std::ceil(CGRectGetMaxX(rect)));
    const int bottom = static_cast<int>(std::ceil(CGRectGetMaxY(rect)));
    return { left, top, right - left, bottom - top };
}

bool copyAXFrame(AXUIElementRef element, QRect* geometry)
{
    CFTypeRef positionValue = nullptr;
    CFTypeRef sizeValue = nullptr;
    const AXError positionError = AXUIElementCopyAttributeValue(
      element, kAXPositionAttribute, &positionValue);
    const AXError sizeError =
      AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &sizeValue);
    CFRef<CFTypeRef> position(positionValue);
    CFRef<CFTypeRef> size(sizeValue);

    if (positionError != kAXErrorSuccess || sizeError != kAXErrorSuccess ||
        !position.get() || !size.get() ||
        CFGetTypeID(position.get()) != AXValueGetTypeID() ||
        CFGetTypeID(size.get()) != AXValueGetTypeID()) {
        return false;
    }

    CGPoint point;
    CGSize dimensions;
    if (!AXValueGetValue(static_cast<AXValueRef>(position.get()),
                         static_cast<AXValueType>(kAXValueCGPointType),
                         &point) ||
        !AXValueGetValue(static_cast<AXValueRef>(size.get()),
                         static_cast<AXValueType>(kAXValueCGSizeType),
                         &dimensions)) {
        return false;
    }

    *geometry =
      qtRect(CGRectMake(point.x, point.y, dimensions.width, dimensions.height));
    return true;
}

QString copyAXString(AXUIElementRef element, CFStringRef attribute)
{
    CFTypeRef value = nullptr;
    if (AXUIElementCopyAttributeValue(element, attribute, &value) !=
        kAXErrorSuccess) {
        return {};
    }
    CFRef<CFTypeRef> holder(value);
    return stringValue(holder.get());
}

bool accessibilityTrusted(bool prompt)
{
    if (!prompt) {
        return AXIsProcessTrusted();
    }

    const void* keys[] = { kAXTrustedCheckOptionPrompt };
    const void* values[] = { kCFBooleanTrue };
    CFRef<CFDictionaryRef> options(
      CFDictionaryCreate(kCFAllocatorDefault,
                         keys,
                         values,
                         1,
                         &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks));
    return AXIsProcessTrustedWithOptions(options.get());
}
}

struct MacOsSmartRegionDetector::Impl
{
    struct Window
    {
        QRect geometry;
        pid_t ownerPid{ 0 };
    };

    QList<Window> windows;
    bool trusted{ false };

    const Window* windowAt(const QPoint& position) const
    {
        const auto found = std::find_if(
          windows.cbegin(), windows.cend(), [&position](const Window& window) {
              return window.geometry.contains(position);
          });
        return found == windows.cend() ? nullptr : &(*found);
    }
};

MacOsSmartRegionDetector::MacOsSmartRegionDetector()
  : m_impl(std::make_unique<Impl>())
{}

MacOsSmartRegionDetector::~MacOsSmartRegionDetector() = default;

void MacOsSmartRegionDetector::refresh()
{
    m_impl->windows.clear();

    m_impl->trusted = accessibilityTrusted(false);

    const auto options = static_cast<CGWindowListOption>(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements);
    CFRef<CFArrayRef> windowList(
      CGWindowListCopyWindowInfo(options, kCGNullWindowID));
    if (!windowList.get()) {
        return;
    }

    const pid_t ownPid = static_cast<pid_t>(QCoreApplication::applicationPid());
    const CFIndex count = CFArrayGetCount(windowList.get());
    for (CFIndex index = 0; index < count; ++index) {
        const auto* dictionary = static_cast<CFDictionaryRef>(
          CFArrayGetValueAtIndex(windowList.get(), index));
        if (!dictionary || CFGetTypeID(dictionary) != CFDictionaryGetTypeID()) {
            continue;
        }

        int ownerPid = 0;
        int layer = 0;
        double alpha = 1.0;
        if (!numberValue(dictionary, kCGWindowOwnerPID, &ownerPid) ||
            ownerPid == ownPid ||
            !numberValue(dictionary, kCGWindowLayer, &layer) || layer != 0 ||
            !boolValue(dictionary, kCGWindowIsOnscreen, true)) {
            continue;
        }
        numberValue(dictionary, kCGWindowAlpha, &alpha);
        if (alpha <= 0.01) {
            continue;
        }

        const auto* boundsDictionary = static_cast<CFDictionaryRef>(
          CFDictionaryGetValue(dictionary, kCGWindowBounds));
        CGRect bounds;
        if (!boundsDictionary ||
            CFGetTypeID(boundsDictionary) != CFDictionaryGetTypeID() ||
            !CGRectMakeWithDictionaryRepresentation(boundsDictionary,
                                                    &bounds)) {
            continue;
        }

        const QRect geometry = qtRect(bounds);
        if (geometry.width() < 20 || geometry.height() < 20) {
            continue;
        }

        Impl::Window window;
        window.geometry = geometry;
        window.ownerPid = static_cast<pid_t>(ownerPid);
        m_impl->windows.append(std::move(window));
    }
}

QList<SmartRegionCandidate> MacOsSmartRegionDetector::candidatesAt(
  const QPoint& globalPosition)
{
    const Impl::Window* window = m_impl->windowAt(globalPosition);
    if (!window) {
        return {};
    }

    QList<QRect> elementGeometries;
    if (m_impl->trusted) {
        CFRef<AXUIElementRef> application(
          AXUIElementCreateApplication(window->ownerPid));
        if (application.get()) {
            AXUIElementSetMessagingTimeout(application.get(), 0.08F);
            AXUIElementRef hitElement = nullptr;
            if (AXUIElementCopyElementAtPosition(
                  application.get(),
                  static_cast<float>(globalPosition.x()),
                  static_cast<float>(globalPosition.y()),
                  &hitElement) == kAXErrorSuccess &&
                hitElement) {
                CFRef<AXUIElementRef> current(hitElement);
                for (int depth = 0; current.get() && depth < 32; ++depth) {
                    const QString role =
                      copyAXString(current.get(), kAXRoleAttribute);
                    if (role == QStringLiteral("AXApplication")) {
                        break;
                    }

                    QRect geometry;
                    if (copyAXFrame(current.get(), &geometry)) {
                        elementGeometries.append(geometry);
                    }

                    if (role == QStringLiteral("AXWindow")) {
                        break;
                    }

                    CFTypeRef parentValue = nullptr;
                    if (AXUIElementCopyAttributeValue(
                          current.get(), kAXParentAttribute, &parentValue) !=
                          kAXErrorSuccess ||
                        !parentValue ||
                        CFGetTypeID(parentValue) != AXUIElementGetTypeID()) {
                        if (parentValue) {
                            CFRelease(parentValue);
                        }
                        break;
                    }
                    current.reset(static_cast<AXUIElementRef>(parentValue));
                }
            }
        }
    }

    return makeSmartRegionCandidateChain(
      elementGeometries, window->geometry, globalPosition);
}

bool MacOsSmartRegionDetector::accessibilityAvailable() const
{
    return m_impl->trusted;
}

void MacOsSmartRegionDetector::requestAccessibilityPermission()
{
    m_impl->trusted = accessibilityTrusted(true);
}
