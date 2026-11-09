// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#include "flameshot.h"
#include "core/flameshotdaemon.h"
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
#include "qhotkey.h"
#endif

#if defined(Q_OS_MACOS)
#include <QApplication>
#include <QWindow>
#include <memory>
#include <objc/message.h>
#include <utility>

namespace {

constexpr long NSApplicationActivationPolicyRegular = 0;
constexpr long NSApplicationActivationPolicyAccessory = 1;

void setActivationPolicy(long policy)
{
    auto sharedApp = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    auto setPolicy = reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend);
    id nsApp = sharedApp(reinterpret_cast<id>(objc_getClass("NSApplication")),
                         sel_registerName("sharedApplication"));
    setPolicy(nsApp, sel_registerName("setActivationPolicy:"), policy);
}

void setActivationPolicyRegular()
{
    setActivationPolicy(NSApplicationActivationPolicyRegular);
}

void setActivationPolicyAccessory()
{
    setActivationPolicy(NSApplicationActivationPolicyAccessory);
}

constexpr const char* visibleInDockProperty = "_visibleInDock";

// Calls an Objective-C method with the given return and argument types.
template<typename Result = id, typename... Args>
Result send(id object, const char* selector, Args... args)
{
    return reinterpret_cast<Result (*)(id, SEL, Args...)>(objc_msgSend)(
      object, sel_registerName(selector), args...);
}

id objcClass(const char* name)
{
    return reinterpret_cast<id>(objc_getClass(name));
}

id currentApplication()
{
    return send(objcClass("NSRunningApplication"), "currentApplication");
}

id frontmostApplication()
{
    return send(send(objcClass("NSWorkspace"), "sharedWorkspace"),
                "frontmostApplication");
}

bool isSameApplication(id first, id second)
{
    return first && second && send<BOOL>(first, "isEqual:", second);
}

using RunningApplication = std::shared_ptr<objc_object>;

// The application that is active before the capture overlay appears, or null
// when it is Flameshot itself.
RunningApplication applicationBeforeCapture()
{
    id application = frontmostApplication();
    if (!application || isSameApplication(application, currentApplication())) {
        return {};
    }
    return RunningApplication(send(application, "retain"),
                              [](id app) { send<void>(app, "release"); });
}

// Hands focus back to the application that was active before the capture.
void reactivate(const RunningApplication& application)
{
    id current = currentApplication();
    // Leave focus alone if the user already switched to another application,
    // or a Flameshot window (save dialog, pin, launcher) is still open.
    if (!isSameApplication(frontmostApplication(), current)) {
        return;
    }
    if (QWidget* active = QApplication::activeWindow();
        active && active->isVisible()) {
        return;
    }
    if (send<BOOL>(application.get(), "isTerminated")) {
        return;
    }

    id nsApp = send(objcClass("NSApplication"), "sharedApplication");
    if (send<BOOL>(nsApp,
                   "respondsToSelector:",
                   sel_registerName("yieldActivationToApplication:"))) {
        // macOS 14 and later only allow cooperative activation.
        send<void>(nsApp, "yieldActivationToApplication:", application.get());
        send<BOOL>(
          application.get(), "activateFromApplication:options:", current, 0UL);
    } else {
        send<BOOL>(application.get(), "activateWithOptions:", 0UL);
    }
}

// NSMainMenuWindowLevel (24) + 2: above the menu bar, its status items and
// the Dock, but below pop-up menus so Qt menus remain visible.
constexpr long captureWindowLevel = 26;

// Qt::WindowStaysOnTopHint maps to NSFloatingWindowLevel, which is below the
// menu bar and the Dock, so they would cover parts of the capture overlay.
void raiseAboveMenuBar(QWidget* widget)
{
    auto getWindow = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    auto setLevel = reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend);
    auto nsView = reinterpret_cast<id>(widget->winId());
    id nsWindow = getWindow(nsView, sel_registerName("window"));
    if (nsWindow != nullptr) {
        setLevel(nsWindow, sel_registerName("setLevel:"), captureWindowLevel);
    }
}

} // namespace

#include <CoreGraphics/CoreGraphics.h>
#endif

#include "config/cacheutils.h"
#include "config/configresolver.h"
#include "config/configwindow.h"
#include "core/qguiappcurrentscreen.h"
#include "utils/abstractlogger.h"
#include "utils/confighandler.h"
#include "utils/globalvalues.h"
#include "utils/inputtrace.h"
#include "utils/screengrabber.h"
#include "utils/screenshotsaver.h"
#include "widgets/capture/capturewidget.h"
#include "widgets/capturelauncher.h"
#include "widgets/infowindow.h"

#ifdef ENABLE_IMGUR
#include "tools/imgupload/imguploadermanager.h"
#include "tools/imgupload/storages/imguploaderbase.h"
#include "widgets/imguploaddialog.h"
#include "widgets/uploadhistory.h"
#endif

#include <QApplication>
#include <QBuffer>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QMessageBox>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#if defined(Q_OS_MACOS)
#include <QScreen>
#endif

#if defined(Q_OS_WIN)
#include "utils/windowsforeground.h"

namespace {
void bringToFront(QWidget* window)
{
    const bool front = WindowsForeground::take(window->winId());
    InputTrace::write(QStringLiteral("overlay front=%1 %2")
                        .arg(front)
                        .arg(WindowsForeground::describe()));
}
}
#endif

Flameshot::Flameshot()
  : m_haveExternalWidget(false)
  , m_captureWindow(nullptr)
#if (defined(Q_OS_MACOS) || defined(Q_OS_WIN))
  , m_HotkeyScreenshotCapture(nullptr)
#endif
#if (defined(Q_OS_MACOS) && ENABLE_IMGUR)
  , m_HotkeyScreenshotHistory(nullptr)
#endif
{
    QString StyleSheet = CaptureButton::globalStyleSheet();
    qApp->setStyleSheet(StyleSheet);

#if defined(Q_OS_MACOS)
    // CLI captures can quit before the deferred focus restore runs.
    connect(qApp,
            &QCoreApplication::aboutToQuit,
            this,
            &Flameshot::restoreFocusAfterCapture);
    // Request Screen Recording permission via the proper CoreGraphics API
    if (!CGPreflightScreenCaptureAccess()) {
        CGRequestScreenCaptureAccess();
    }
#endif
#if (defined(Q_OS_MACOS) || defined(Q_OS_WIN))
    // Set global shortcuts for MacOS or Windows
    m_HotkeyScreenshotCapture = new QHotkey(
      QKeySequence(ConfigHandler().shortcut("TAKE_SCREENSHOT")), true, this);
    QObject::connect(m_HotkeyScreenshotCapture,
                     &QHotkey::activated,
                     qApp,
                     [this]() { gui(); });
#endif
#if (defined(Q_OS_MACOS) && ENABLE_IMGUR)
    m_HotkeyScreenshotHistory = new QHotkey(
      QKeySequence(ConfigHandler().shortcut("SCREENSHOT_HISTORY")), true, this);
    QObject::connect(m_HotkeyScreenshotHistory,
                     &QHotkey::activated,
                     qApp,
                     [this]() { history(); });
#endif
}

#if defined(Q_OS_MACOS)
void Flameshot::restoreFocusAfterCapture()
{
    if (!m_captureWindow && m_restoreCaptureFocus) {
        std::exchange(m_restoreCaptureFocus, {})();
    }
}
#endif

Flameshot* Flameshot::instance()
{
    static Flameshot c;
    return &c;
}

CaptureWidget* Flameshot::gui(const CaptureRequest& req)
{
    if (InputTrace::enabled()) {
        InputTrace::write(QStringLiteral("capture %1 exe=%2 pid=%3")
                            .arg(GlobalValues::versionInfo().simplified())
                            .arg(QCoreApplication::applicationFilePath())
                            .arg(QCoreApplication::applicationPid()));
    }
    if (!resolveAnyConfigErrors()) {
        return nullptr;
    }

#if defined(Q_OS_MACOS)
    // TCC can revoke access after a rebuild or signing change while the
    // daemon is running. Do not open an overlay with a redacted screenshot.
    if (!CGPreflightScreenCaptureAccess()) {
        const QString message =
          tr("Flameshot cannot record the screen. Enable this application in "
             "System Settings > Privacy & Security > Screen & System Audio "
             "Recording, then quit and reopen Flameshot. If it is already "
             "enabled, remove and add this application again.");
        AbstractLogger::error(AbstractLogger::Stderr) << message;
        if (origin() == DAEMON) {
            QMessageBox::warning(
              nullptr, tr("Screen Recording permission"), message);
        }
        emit captureFailed();
        return nullptr;
    }
#endif

    CaptureRequest request = req;
    if (request.captureMode() == CaptureRequest::GRAPHICAL_MODE &&
        request.initialSelection().isNull() &&
        ConfigHandler().saveLastRegion()) {
        request.setInitialSelection(getLastRegion());
    }

#if defined(Q_OS_MACOS)
    if (RunningApplication previous = applicationBeforeCapture()) {
        m_restoreCaptureFocus = [previous]() { reactivate(previous); };
    }
    // This is required on MacOS because of Mission Control. If you'll switch to
    // another Desktop you cannot take a new screenshot from the tray, you have
    // to switch back to the Flameshot Desktop manually. It is not obvious and a
    // large number of users are confused and report a bug.
    if (m_captureWindow != nullptr) {
        m_captureWindow->close();
        delete m_captureWindow;
        m_captureWindow = nullptr;
    }
#endif

    if (nullptr == m_captureWindow) {
        // TODO is this unnecessary now?
        int timeout = 5000; // 5 seconds
        const int delay = 100;
        QWidget* modalWidget = nullptr;
        for (; timeout >= 0; timeout -= delay) {
            modalWidget = qApp->activeModalWidget();
            if (nullptr == modalWidget) {
                break;
            }
            modalWidget->close();
            modalWidget->deleteLater();
            QThread::msleep(delay);
        }
        if (0 == timeout) {
            QMessageBox::warning(
              nullptr, tr("Error"), tr("Unable to close active modal widgets"));
            return nullptr;
        }

        m_captureWindow = new CaptureWidget(request);

#if defined(Q_OS_MACOS)
        connect(m_captureWindow, &QObject::destroyed, this, [this]() {
            // Let the overlay disappear and export callbacks finish first.
            QTimer::singleShot(0, this, &Flameshot::restoreFocusAfterCapture);
        });
#endif

#ifdef Q_OS_WIN
        m_captureWindow->show();
        // Windows may refuse to bring the overlay to the front, in which case
        // keys such as Escape would go to another application.
        bringToFront(m_captureWindow);
        QTimer::singleShot(150, m_captureWindow, [this]() {
            bringToFront(m_captureWindow);
            m_captureWindow->update();
        });
        QTimer::singleShot(1000, m_captureWindow, [this]() {
            InputTrace::write(QStringLiteral("overlay after 1s foreground=%1")
                                .arg(WindowsForeground::isForeground(
                                  m_captureWindow->winId())));
        });
#elif defined(Q_OS_MACOS)
        if (ConfigHandler().useNativeFullscreen()) {
            m_captureWindow->showFullScreen();
        } else {
            m_captureWindow->show();
            raiseAboveMenuBar(m_captureWindow);
        }
        m_captureWindow->activateWindow();
        m_captureWindow->raise();
#else
        m_captureWindow->showFullScreen();
//        m_captureWindow->show(); // For CaptureWidget Debugging under Linux
#endif
        return m_captureWindow;
    } else {
        emit captureFailed();
        return nullptr;
    }
}

void Flameshot::screen(CaptureRequest req, const int screenNumber)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    bool ok = false;
    QPixmap p;
    QRect geometry;

    if (screenNumber < 0) {
        ScreenGrabber grabber;
        p = grabber.grabEntireDesktop(ok);
        if (ok) {
            QScreen* selectedScreen = grabber.getSelectedScreen();
            if (selectedScreen) {
                geometry = ScreenGrabber().screenGeometry(selectedScreen);
            } else {
                ok = false;
            }
        }
    } else if (screenNumber >= qApp->screens().count()) {
        AbstractLogger() << QObject::tr(
          "Requested screen exceeds screen count");
        ok = false;
    } else {
        // Specific screen number provided - use grabScreen to bypass selector
        QScreen* screen = qApp->screens()[screenNumber];
        p = ScreenGrabber().grabScreen(screen, ok);
        if (ok) {
            geometry = ScreenGrabber().screenGeometry(screen);
        }
    }

    if (ok) {
        QRect region = req.initialSelection();
        if (region.isNull()) {
            region = geometry;
        } else {
            QRect screenGeom = geometry;
            screenGeom.moveTopLeft({ 0, 0 });
            region = region.intersected(screenGeom);
            p = p.copy(region);
        }
        if (req.tasks() & CaptureRequest::PIN) {
            // change geometry for pin task
            req.addPinTask(region);
        }
        exportCapture(p, geometry, req);
    } else {
        emit captureFailed();
    }
}

void Flameshot::full(const CaptureRequest& req)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    bool ok = true;
    QPixmap p(ScreenGrabber().grabFullDesktop(ok));
    if (ok) {
        QRect selection; // `flameshot full` does not support region selection
        exportCapture(p, selection, req);
    } else {
        emit captureFailed();
    }
}

void Flameshot::launcher()
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    if (m_launcherWindow == nullptr) {
        m_launcherWindow = new CaptureLauncher();
    }
    m_launcherWindow->show();
#if defined(Q_OS_MACOS)
    showDockIcon(m_launcherWindow);
#endif
}

void Flameshot::config()
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    if (m_configWindow == nullptr) {
        m_configWindow = new ConfigWindow();
        m_configWindow->show();
        // Call show() first, otherwise the correct geometry cannot be fetched
        // for centering the window on the screen
        QRect position = m_configWindow->frameGeometry();
        QScreen* currentScreen = QGuiAppCurrentScreen().currentScreen();
        position.moveCenter(currentScreen->availableGeometry().center());
        m_configWindow->move(position.topLeft());
#if defined(Q_OS_MACOS)
        showDockIcon(m_configWindow);
#endif
    }
}

void Flameshot::info()
{
    if (m_infoWindow == nullptr) {
        m_infoWindow = new InfoWindow();
#if defined(Q_OS_MACOS)
        showDockIcon(m_infoWindow);
#endif
    }
}

#ifdef ENABLE_IMGUR
void Flameshot::history()
{
    static UploadHistory* historyWidget = nullptr;
    if (historyWidget == nullptr) {
        historyWidget = new UploadHistory;
        historyWidget->loadHistory();
        connect(historyWidget, &QObject::destroyed, this, []() {
            historyWidget = nullptr;
        });
    }

    historyWidget->show();
    // Call show() first, otherwise the correct geometry cannot be fetched
    // for centering the window on the screen
    QRect position = historyWidget->frameGeometry();
    QScreen* currentScreen = QGuiAppCurrentScreen().currentScreen();
    position.moveCenter(currentScreen->availableGeometry().center());
    historyWidget->move(position.topLeft());

#if defined(Q_OS_MACOS)
    showDockIcon(historyWidget);
#endif
}
#endif

#if defined(Q_OS_MACOS)
void Flameshot::onWindowVisibilityChanged(QWindow::Visibility newVisibility)
{
    auto* qw = qobject_cast<QWindow*>(sender());
    if (!qw) {
        return;
    }

    if (newVisibility == QWindow::Hidden) {
        qw->setProperty(visibleInDockProperty, false);
        --m_dockIconVisibleCount;
        if (m_dockIconVisibleCount == 0) {
            setActivationPolicyAccessory();
        }
    } else {
        bool windowTrackedInDock = qw->property(visibleInDockProperty).toBool();
        if (!windowTrackedInDock) {
            qw->setProperty(visibleInDockProperty, true);
            ++m_dockIconVisibleCount;
            setActivationPolicyRegular();
        }
    }
}

void Flameshot::showDockIcon(QWidget* w)
{
    QWindow* qw = w->windowHandle();
    if (!qw) {
        return;
    }

    connect(qw,
            &QWindow::visibilityChanged,
            this,
            &Flameshot::onWindowVisibilityChanged);
}
#endif

void Flameshot::openSavePath()
{
    QString savePath = ConfigHandler().savePath();
    if (!savePath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(savePath));
    }
}

QVersionNumber Flameshot::getVersion()
{
    return QVersionNumber::fromString(
      QStringLiteral(APP_VERSION).replace("v", ""));
}

void Flameshot::setOrigin(Origin origin)
{
    m_origin = origin;
}

Flameshot::Origin Flameshot::origin()
{
    return m_origin;
}

/**
 * @brief Prompt the user to resolve config errors if necessary.
 * @return Whether errors were resolved.
 */
bool Flameshot::resolveAnyConfigErrors()
{
    bool resolved = true;
    ConfigHandler confighandler;
    if (!confighandler.checkUnrecognizedSettings() ||
        !confighandler.checkSemantics()) {
        auto* resolver = new ConfigResolver();
        QObject::connect(
          resolver, &ConfigResolver::rejected, [resolver, &resolved]() {
              resolved = false;
              resolver->deleteLater();
              if (origin() == CLI) {
                  exit(1);
              }
          });
        QObject::connect(
          resolver, &ConfigResolver::accepted, [resolver, &resolved]() {
              resolved = true;
              resolver->close();
              resolver->deleteLater();
              // Ensure that the dialog is closed before starting capture
              qApp->processEvents();
          });
        resolver->exec();
        qApp->processEvents();
    }
    return resolved;
}

void Flameshot::requestCapture(const CaptureRequest& request)
{
    if (!resolveAnyConfigErrors()) {
        return;
    }

    switch (request.captureMode()) {
        case CaptureRequest::FULLSCREEN_MODE:
            QTimer::singleShot(request.delay(),
                               [this, request] { full(request); });
            break;
        case CaptureRequest::SCREEN_MODE: {
            int&& number = request.data().toInt();
            QTimer::singleShot(request.delay(), [this, request, number]() {
                screen(request, number);
            });
            break;
        }
        case CaptureRequest::GRAPHICAL_MODE: {
            QTimer::singleShot(
              request.delay(), this, [this, request]() { gui(request); });
            break;
        }
        default:
            emit captureFailed();
            break;
    }
}

void Flameshot::exportCapture(const QPixmap& capture,
                              QRect& selection,
                              const CaptureRequest& req)
{
    using CR = CaptureRequest;
    int tasks = req.tasks(), mode = req.captureMode();
    QString path = req.path();

    if (tasks & CR::PRINT_GEOMETRY) {
        QTextStream(stdout)
          << selection.width() << "x" << selection.height() << "+"
          << selection.x() << "+" << selection.y() << "\n";
    }

    if (tasks & CR::PRINT_RAW) {
        QByteArray byteArray;
        QBuffer buffer(&byteArray);
        capture.save(&buffer, "PNG");
        if (QFile file; file.open(stdout, QIODevice::WriteOnly)) {
            file.write(byteArray);
            file.close();
        }
    }

    if (tasks & CR::SAVE) {
        if (req.path().isEmpty()) {
            saveToFilesystemGUI(capture);
        } else {
            saveToFilesystem(capture, path);
        }
    }

    if (tasks & CR::COPY) {
        FlameshotDaemon::copyToClipboard(capture);
    }

    if (tasks & CR::PIN) {
        FlameshotDaemon::createPin(capture, selection);
        if (mode == CR::SCREEN_MODE || mode == CR::FULLSCREEN_MODE) {
            AbstractLogger::info()
              << QObject::tr("Full screen screenshot pinned to screen");
        }
    }

#ifdef ENABLE_IMGUR
    if (tasks & CR::UPLOAD) {
        if (!ConfigHandler().uploadWithoutConfirmation()) {
            auto* dialog = new ImgUploadDialog();
            if (dialog->exec() == QDialog::Rejected) {
                return;
            }
        }

        ImgUploaderBase* widget = ImgUploaderManager().uploader(capture);
        widget->show();
        widget->activateWindow();
        // NOTE: lambda can't capture 'this' because it might be destroyed later
        CR::ExportTask tasks = tasks;
        QObject::connect(
          widget, &ImgUploaderBase::uploadOk, [=, this](const QUrl& url) {
              if (ConfigHandler().copyURLAfterUpload()) {
                  if (!(tasks & CR::COPY)) {
                      FlameshotDaemon::copyToClipboard(
                        url.toString(), tr("URL copied to clipboard."));
                  }
                  widget->showPostUploadDialog();
              }
          });
    }
#endif

    if (!(tasks & CR::UPLOAD)) {
        emit captureTaken(capture);
    }
}

void Flameshot::setExternalWidget(bool b)
{
    m_haveExternalWidget = b;
}
bool Flameshot::haveExternalWidget()
{
    return m_haveExternalWidget;
}

// STATIC ATTRIBUTES
Flameshot::Origin Flameshot::m_origin = Flameshot::DAEMON;
