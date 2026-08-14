#include "main_window.h"

#include <QApplication>

int main(int argc, char *argv[]) {
  // SceneViewport embeds the engine's Vulkan renderer directly into the
  // window via an XCB handle (see ensure_engine_initialized(), which
  // qFatal()s if QNativeInterface::QX11Application isn't available) --
  // this app cannot run under any other platform plugin. Forcing it here,
  // before QApplication reads QT_QPA_PLATFORM, means the app still opens
  // under a native-Wayland session whose QT_QPA_PLATFORM lists "wayland"
  // ahead of "xcb" (Qt would otherwise pick native Wayland, which
  // initializes "successfully" from Qt's own point of view and only fails
  // later, confusingly, on first paint) -- without stopping an explicit
  // `-platform` command-line override from still taking priority, since
  // Qt only falls back to QT_QPA_PLATFORM when no such argument is given.
  qputenv("QT_QPA_PLATFORM", "xcb");

  QApplication app(argc, argv);

  SdfEditorWindow window;
  window.show();

  return app.exec();
}
