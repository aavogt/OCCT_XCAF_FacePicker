#include "GlfwOcctWindow.h"

#if defined(__APPLE__)
  #undef Handle
  #define GLFW_EXPOSE_NATIVE_COCOA
  #define GLFW_EXPOSE_NATIVE_NSGL
#elif defined(_WIN32)
  #define GLFW_EXPOSE_NATIVE_WIN32
  #define GLFW_EXPOSE_NATIVE_WGL
#else
  #define GLFW_EXPOSE_NATIVE_X11
  #define GLFW_EXPOSE_NATIVE_GLX
#endif

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

GlfwOcctWindow::GlfwOcctWindow(int theWidth, int theHeight, const TCollection_AsciiString& theTitle)
    : myGlfwWindow(glfwCreateWindow(theWidth, theHeight, theTitle.ToCString(), NULL, NULL))
{
  if (myGlfwWindow != nullptr)
  {
    int aWidth = 0, aHeight = 0;
    glfwGetWindowPos(myGlfwWindow, &myXLeft, &myYTop);
    glfwGetWindowSize(myGlfwWindow, &aWidth, &aHeight);
    myXRight = myXLeft + aWidth;
    myYBottom = myYTop + aHeight;

#if !defined(_WIN32) && !defined(__APPLE__)
    myDisplay = new Aspect_DisplayConnection((Aspect_XDisplay*)glfwGetX11Display());
#else
    myDisplay = new Aspect_DisplayConnection();
#endif
  }
}

void GlfwOcctWindow::Close()
{
  if (myGlfwWindow != nullptr)
  {
    glfwDestroyWindow(myGlfwWindow);
    myGlfwWindow = nullptr;
  }
}

Aspect_Drawable GlfwOcctWindow::NativeHandle() const
{
#if defined(__APPLE__)
  return (Aspect_Drawable)glfwGetCocoaWindow(myGlfwWindow);
#elif defined(_WIN32)
  return (Aspect_Drawable)glfwGetWin32Window(myGlfwWindow);
#else
  return (Aspect_Drawable)glfwGetX11Window(myGlfwWindow);
#endif
}

Aspect_RenderingContext GlfwOcctWindow::NativeGlContext() const
{
#if defined(__APPLE__)
  return (NSOpenGLContext*)glfwGetNSGLContext(myGlfwWindow);
#elif defined(_WIN32)
  return glfwGetWGLContext(myGlfwWindow);
#else
  return glfwGetGLXContext(myGlfwWindow);
#endif
}

Standard_Boolean GlfwOcctWindow::IsMapped() const
{
  return glfwGetWindowAttrib(myGlfwWindow, GLFW_VISIBLE) != 0;
}

void GlfwOcctWindow::Map() const
{
  glfwShowWindow(myGlfwWindow);
}

void GlfwOcctWindow::Unmap() const
{
  glfwHideWindow(myGlfwWindow);
}

Aspect_TypeOfResize GlfwOcctWindow::DoResize()
{
  if (glfwGetWindowAttrib(myGlfwWindow, GLFW_VISIBLE) == 1)
  {
    int anXPos = 0, anYPos = 0, aWidth = 0, aHeight = 0;
    glfwGetWindowPos(myGlfwWindow, &anXPos, &anYPos);
    glfwGetWindowSize(myGlfwWindow, &aWidth, &aHeight);
    myXLeft = anXPos;
    myXRight = anXPos + aWidth;
    myYTop = anYPos;
    myYBottom = anYPos + aHeight;
  }
  return Aspect_TOR_UNKNOWN;
}

Graphic3d_Vec2i GlfwOcctWindow::CursorPosition() const
{
  Graphic3d_Vec2d aPos;
  glfwGetCursorPos(myGlfwWindow, &aPos.x(), &aPos.y());
  return Graphic3d_Vec2i((int)aPos.x(), (int)aPos.y());
}
