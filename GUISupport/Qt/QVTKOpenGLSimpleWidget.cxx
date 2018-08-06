/*=========================================================================

  Program:   Visualization Toolkit
  Module:    QVTKOpenGLSimpleWidget.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "QVTKOpenGLSimpleWidget.h"

#include <QApplication>
#include <QDesktopWidget>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLDebugLogger>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLTexture>
#include <QPointer>
#include <QScopedValueRollback>
#include <QtDebug>

#include "QVTKInteractor.h"
#include "QVTKInteractorAdapter.h"
#include "vtkCommand.h"
#include "vtkGenericOpenGLRenderWindow.h"
#include "vtkInteractorStyleTrackballCamera.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkOpenGLState.h"

#ifdef __APPLE__
#include "QVTKOpenGLWidget.h"
#include <QTimer>

/**
 * Unused except on MacOS.
 * In an application using both QVTKOpenGLWidget
 * and QVTKOpenGLSimpleWidget, this bug can appear:
 * https://bugreports.qt.io/browse/QTBUG-69644
 */
namespace
{
  /**
   * It is needed to switch the visibility of currently visible QVTKOpenGLWidget
   * back and forth (ie hide then show). Just after initialization of QVTKOpenGLSimpleWidget
   * (ie when we receive WindowActivate event, or PolishRequest event)
   * This method takes care of it.
   */
  void QVTKOpenGLWidgetMacOSCheck(QWidget* window)
  {
    // Used a static to ensure the fix is done only once
    static bool QVTKOpenGLWidgetMacOSFixed = false;

    if (QVTKOpenGLWidgetMacOSFixed || !window)
    {
      return;
    }

    // Switch visibility back and forth of visible QVTKOpenGLWidgets
    // that share the same window as this QVTKOpenGLSimpleWidget.
    // This ensures they come back on the front after the Qt bug happens.
    auto widgets = window->findChildren<QVTKOpenGLWidget*>();
    for (auto qvglWidget : widgets)
    {
      if (qvglWidget->isVisible())
      {
        qvglWidget->hide();
        qvglWidget->show();
        QVTKOpenGLWidgetMacOSFixed = true;
      }
    }
  }
}
#endif // __APPLE__

// #define DEBUG_QVTKOPENGL_WIDGET
#ifdef DEBUG_QVTKOPENGL_WIDGET
#define vtkQVTKOpenGLSimpleWidgetDebugMacro(msg)                                                         \
  cout << this << ": " msg << endl;                                                                \
  if (this->Logger)                                                                                \
  {                                                                                                \
    this->Logger->logMessage(                                                                      \
      QOpenGLDebugMessage::createApplicationMessage(QStringLiteral("QVTKOpenGLSimpleWidget::" msg)));    \
  }
#else
#define vtkQVTKOpenGLSimpleWidgetDebugMacro(x)
#endif

class QVTKOpenGLSimpleWidgetObserver : public vtkCommand
{
public:
  static QVTKOpenGLSimpleWidgetObserver* New() { return new QVTKOpenGLSimpleWidgetObserver(); }
  vtkTypeMacro(QVTKOpenGLSimpleWidgetObserver, vtkCommand);

  void SetTarget(QVTKOpenGLSimpleWidget* target) { this->Target = target; }

  void Execute(vtkObject*, unsigned long eventId, void* callData) override
  {
    if (this->Target)
    {
      switch (eventId)
      {
        case vtkCommand::WindowMakeCurrentEvent:
          {
            // We do not call QOpenGLWidget::makeCurrent() as that also makes the
            // frame buffer object used by QOpenGLWidget active. This can have
            // unintended side effects when MakeCurrent gets called in a
            // render-pass, for example. We should only be making the context
            // active. To do that, we use this trick. We rely on the
            // QOpenGLContext have been called makeCurrent() previously so we
            // can get to the surface that was used to do that. We simply
            // reactivate on that surface.
            QOpenGLContext* ctxt = this->Target->context();
            QSurface* surface = ctxt? ctxt->surface() : nullptr;
            if (surface)
            {
              ctxt->makeCurrent(surface);
            }
            Q_ASSERT(ctxt == nullptr || surface != nullptr);
          }
          break;

        case vtkCommand::WindowIsCurrentEvent:
        {
          bool& cstatus = *reinterpret_cast<bool*>(callData);
          cstatus = (QOpenGLContext::currentContext() == this->Target->context());
        }
        break;

        case vtkCommand::WindowFrameEvent:
          this->Target->windowFrameEventCallback();
          break;

        case vtkCommand::StartEvent:
          VTK_FALLTHROUGH;
        case vtkCommand::StartPickEvent:
          this->Target->startEventCallback();
          break;
      }
    }
  }

protected:
  QVTKOpenGLSimpleWidgetObserver() {}
  ~QVTKOpenGLSimpleWidgetObserver() override {}
  QPointer<QVTKOpenGLSimpleWidget> Target;
};

//-----------------------------------------------------------------------------
QVTKOpenGLSimpleWidget::QVTKOpenGLSimpleWidget(QWidget* parentWdg, Qt::WindowFlags f)
  : Superclass(parentWdg, f)
  , InteractorAdapter(nullptr)
  , EnableHiDPI(false)
  , OriginalDPI(0)
  , FBO(nullptr)
  , InPaintGL(false)
  , DoVTKRenderInPaintGL(false)
  , Logger(nullptr)
{
  this->Observer->SetTarget(this);

  // default to strong focus
  this->setFocusPolicy(Qt::StrongFocus);

  this->setUpdateBehavior(QOpenGLWidget::PartialUpdate);

  this->InteractorAdapter = new QVTKInteractorAdapter(this);
  this->InteractorAdapter->SetDevicePixelRatio(this->devicePixelRatio());

  this->setMouseTracking(true);

  // QOpenGLWidget::resized() is triggered when the default FBO in QOpenGLWidget is recreated.
  // We use the same signal to recreate our FBO.
  this->connect(this, SIGNAL(resized()), SLOT(recreateFBO()));
}

//-----------------------------------------------------------------------------
QVTKOpenGLSimpleWidget::~QVTKOpenGLSimpleWidget()
{
  vtkQVTKOpenGLSimpleWidgetDebugMacro("~QVTKOpenGLSimpleWidget");
  // essential to cleanup context so that the render window finalizes and
  // releases any graphics resources it may have allocated.
  this->cleanupContext();
  this->SetRenderWindow(static_cast<vtkGenericOpenGLRenderWindow*>(nullptr));
  this->Observer->SetTarget(nullptr);
  delete this->InteractorAdapter;
  delete this->Logger;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::SetRenderWindow(vtkRenderWindow* win)
{
  vtkGenericOpenGLRenderWindow* gwin = vtkGenericOpenGLRenderWindow::SafeDownCast(win);
  this->SetRenderWindow(gwin);
  if (gwin == nullptr && win != nullptr)
  {
    qDebug() << "QVTKOpenGLSimpleWidget requires a `vtkGenericOpenGLRenderWindow`. `"
             << win->GetClassName() << "` is not supported.";
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::SetRenderWindow(vtkGenericOpenGLRenderWindow* win)
{
  if (this->RenderWindow == win)
  {
    return;
  }

  if (this->RenderWindow)
  {
    this->RenderWindow->RemoveObserver(this->Observer);
    this->RenderWindow->SetReadyForRendering(false);
  }
  this->RenderWindow = win;
  this->requireRenderWindowInitialization();
  if (this->RenderWindow)
  {
    // set this to 0 to reinitialize it before setting the RenderWindow DPI
    this->OriginalDPI = 0;

    // if an interactor wasn't provided, we'll make one by default
    if (!this->RenderWindow->GetInteractor())
    {
      // create a default interactor
      vtkNew<QVTKInteractor> iren;
      // iren->SetUseTDx(this->UseTDx);
      this->RenderWindow->SetInteractor(iren);
      iren->Initialize();

      // now set the default style
      vtkNew<vtkInteractorStyleTrackballCamera> style;
      iren->SetInteractorStyle(style);
    }

    this->RenderWindow->AddObserver(vtkCommand::WindowMakeCurrentEvent, this->Observer);
    this->RenderWindow->AddObserver(vtkCommand::WindowIsCurrentEvent, this->Observer);
    this->RenderWindow->AddObserver(vtkCommand::WindowFrameEvent, this->Observer);
    this->RenderWindow->AddObserver(vtkCommand::StartEvent, this->Observer);
    this->RenderWindow->AddObserver(vtkCommand::StartPickEvent, this->Observer);

    if (this->FBO)
    {
      this->makeCurrent();
      this->recreateFBO();
    }
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::startEventCallback()
{
  vtkQVTKOpenGLSimpleWidgetDebugMacro("startEventCallback");
  this->makeCurrent();
  if (this->FBO)
  {
    // ensure that before vtkRenderWindow starts to render, we activate the FBO
    // to render into. VTK code can be a bit lax with it. This just ensures that
    // we have the FBO activated.
    this->FBO->bind();
  }
}

//-----------------------------------------------------------------------------
vtkRenderWindow* QVTKOpenGLSimpleWidget::GetRenderWindow()
{
  if (!this->RenderWindow)
  {
    // create a default
    vtkGenericOpenGLRenderWindow* win = vtkGenericOpenGLRenderWindow::New();
    this->SetRenderWindow(win);
    win->Delete();
  }
  return this->RenderWindow;
}

//-----------------------------------------------------------------------------
QVTKInteractor* QVTKOpenGLSimpleWidget::GetInteractor()
{
  return QVTKInteractor::SafeDownCast(this->GetRenderWindow()->GetInteractor());
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::copyFromFormat(const QSurfaceFormat& format, vtkRenderWindow* win)
{
  if (vtkOpenGLRenderWindow* oglWin = vtkOpenGLRenderWindow::SafeDownCast(win))
  {
    oglWin->SetStereoCapableWindow(format.stereo() ? 1 : 0);
    // samples may not be correct if format is obtained from
    // QOpenGLWidget::format() after the context is created. That's because
    // QOpenGLWidget always created context to samples=0.
    oglWin->SetMultiSamples(format.samples());
    oglWin->SetStencilCapable(format.stencilBufferSize() > 0);
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::copyToFormat(vtkRenderWindow* win, QSurfaceFormat& format)
{
  if (vtkOpenGLRenderWindow* oglWin = vtkOpenGLRenderWindow::SafeDownCast(win))
  {
    format.setStereo(oglWin->GetStereoCapableWindow());
    format.setSamples(oglWin->GetMultiSamples());
    format.setStencilBufferSize(oglWin->GetStencilCapable() ? 8 : 0);
  }
}

//-----------------------------------------------------------------------------
QSurfaceFormat QVTKOpenGLSimpleWidget::defaultFormat()
{
  QSurfaceFormat fmt;
  fmt.setRenderableType(QSurfaceFormat::OpenGL);
  fmt.setVersion(3, 2);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
  fmt.setRedBufferSize(1);
  fmt.setGreenBufferSize(1);
  fmt.setBlueBufferSize(1);
  fmt.setDepthBufferSize(1);
  fmt.setStencilBufferSize(0);
  fmt.setAlphaBufferSize(1);
  fmt.setStereo(false);
  fmt.setSamples(vtkOpenGLRenderWindow::GetGlobalMaximumNumberOfMultiSamples());
#ifdef DEBUG_QVTKOPENGL_WIDGET
  fmt.setOption(QSurfaceFormat::DebugContext);
#endif
  return fmt;
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::setEnableHiDPI(bool enable)
{
  this->EnableHiDPI = enable;

  if (this->RenderWindow)
  {
    if (this->OriginalDPI == 0)
    {
      this->OriginalDPI = this->RenderWindow->GetDPI();
    }
    if (this->EnableHiDPI)
    {
      this->RenderWindow->SetDPI(this->OriginalDPI * this->devicePixelRatio());
    }
    else
    {
      this->RenderWindow->SetDPI(this->OriginalDPI);
    }
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::setQVTKCursor(const QCursor &cursor)
{
  this->setCursor(cursor);
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::recreateFBO()
{
  vtkQVTKOpenGLSimpleWidgetDebugMacro("recreateFBO");
  delete this->FBO;
  this->FBO = nullptr;
  if (!this->RenderWindow)
  {
    return;
  }

  // Since QVTKOpenGLSimpleWidget::initializeGL() cannot set multi-samples
  // state on the RenderWindow correctly, we do it here.
  QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
  GLint samples;
  f->glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);

  // Some graphics drivers report the number of samples as 1 when
  // multisampling is off. Set the number of samples to 0 in this case.
  samples = samples > 1 ? samples : 0;
  this->RenderWindow->SetMultiSamples(static_cast<int>(samples));

  QOpenGLFramebufferObjectFormat format;
  format.setAttachment(QOpenGLFramebufferObject::Depth);
  format.setSamples(samples);

  const int devicePixelRatio_ = this->devicePixelRatio();
  const QSize widgetSize = this->size();
  const QSize deviceSize = widgetSize * devicePixelRatio_;

  // This is as good an opportunity as any to communicate size to the render
  // window.
  this->InteractorAdapter->SetDevicePixelRatio(devicePixelRatio_);
  if (vtkRenderWindowInteractor* iren = this->RenderWindow->GetInteractor())
  {
    iren->SetSize(deviceSize.width(), deviceSize.height());
  }
  this->RenderWindow->SetSize(deviceSize.width(), deviceSize.height());
  this->RenderWindow->SetPosition(this->x() * devicePixelRatio_, this->y() * devicePixelRatio_);

  // Set screen size on render window.
  const QRect screenGeometry = QApplication::desktop()->screenGeometry(this);
  this->RenderWindow->SetScreenSize(screenGeometry.width(), screenGeometry.height());

  this->FBO = new QOpenGLFramebufferObject(deviceSize, format);
  this->FBO->bind();
  this->RenderWindow->SetForceMaximumHardwareLineWidth(1);
  this->RenderWindow->SetReadyForRendering(true);
  this->RenderWindow->InitializeFromCurrentContext();

  this->setEnableHiDPI(this->EnableHiDPI);

  // Since the context or frame buffer was recreated, if a paintGL call ensues,
  // we need to ensure we're requesting VTK to render.
  this->DoVTKRenderInPaintGL = true;

  // Clear to ensure that an uninitialized framebuffer is never displayed.
  vtkOpenGLState *ostate = this->RenderWindow->GetState();
  // have to keep vtk state up to date as well
  ostate->vtkglDisable(GL_SCISSOR_TEST);
  ostate->vtkglClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  ostate->vtkglViewport(0, 0, deviceSize.width(), deviceSize.height());
  f->glDisable(GL_SCISSOR_TEST);
  f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  f->glClear(GL_COLOR_BUFFER_BIT);
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::initializeGL()
{
  this->Superclass::initializeGL();

#ifdef DEBUG_QVTKOPENGL_WIDGET
  delete this->Logger;
  this->Logger = new QOpenGLDebugLogger(this);
  this->Logger->initialize(); // initializes in the current context.
#endif

  vtkQVTKOpenGLSimpleWidgetDebugMacro("initializeGL");
  if (this->RenderWindow)
  {
    // use QSurfaceFormat for the widget, update ivars on the vtkRenderWindow.
    QVTKOpenGLSimpleWidget::copyFromFormat(this->format(), this->RenderWindow);
    // When a QOpenGLWidget is told to use a QSurfaceFormat with samples > 0,
    // QOpenGLWidget doesn't actually create a context with multi-samples and
    // internally changes the QSurfaceFormat to be samples=0. Thus, we can't
    // rely on the QSurfaceFormat to indicate to us if multisampling is being
    // used. We should use glGetRenderbufferParameteriv(..) to get
    // GL_RENDERBUFFER_SAMPLES to determine the samples used. This is done by
    // in recreateFBO().
  }
  this->connect(
    this->context(), SIGNAL(aboutToBeDestroyed()), SLOT(cleanupContext()), Qt::UniqueConnection);
  this->requireRenderWindowInitialization();
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::requireRenderWindowInitialization()
{
  if (this->RenderWindow)
  {
    this->RenderWindow->SetReadyForRendering(false);
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::resizeGL(int w, int h)
{
  vtkQVTKOpenGLSimpleWidgetDebugMacro("resizeGL");
  this->Superclass::resizeGL(w, h);
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::paintGL()
{
  if (this->InPaintGL)
  {
    return;
  }

  if (!this->RenderWindow)
  {
    return;
  }

  if (!this->FBO
    || this->FBO->handle() != this->RenderWindow->GetDefaultFrameBufferId())
  {
    this->recreateFBO();
  }

  QScopedValueRollback<bool> var(this->InPaintGL, true);
  this->Superclass::paintGL();

  if (this->DoVTKRenderInPaintGL && !this->renderVTK())
  {
    vtkQVTKOpenGLSimpleWidgetDebugMacro("paintGL:skipped-renderVTK");
    // This should be very rare, but it's conceivable that subclasses of
    // QVTKOpenGLSimpleWidget are simply not ready to do a
    // render on VTK render window when widget is being painted.
    // Leave the buffer unchanged.
    return;
  }

  // We just did a render, if we needed it. Turn the flag off.
  this->DoVTKRenderInPaintGL = false;

  // If render was triggered by above calls, that may change the current context
  // due to things like progress events triggering updates on other widgets
  // (e.g. progress bar). Hence we need to make sure to call makeCurrent()
  // before proceeding with blit-ing.
  this->makeCurrent();

  // blit from this->FBO to QOpenGLWidget's FBO.
  vtkQVTKOpenGLSimpleWidgetDebugMacro("paintGL::blit-to-defaultFBO");
  QOpenGLFunctions_3_2_Core* f =
    QOpenGLContext::currentContext()->versionFunctions<QOpenGLFunctions_3_2_Core>();
  if (f)
  {
    vtkOpenGLState *ostate = this->RenderWindow->GetState();

    f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, this->defaultFramebufferObject());
    f->glDrawBuffer(GL_COLOR_ATTACHMENT0);

    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, this->FBO->handle());
    f->glReadBuffer(GL_COLOR_ATTACHMENT0);
    f->glDisable(GL_SCISSOR_TEST); // Scissor affects glBindFramebuffer.
    ostate->vtkglDisable(GL_SCISSOR_TEST); // Scissor affects glBindFramebuffer.
    f->glBlitFramebuffer(0, 0, this->RenderWindow->GetSize()[0], this->RenderWindow->GetSize()[1],
      0, 0, this->RenderWindow->GetSize()[0], this->RenderWindow->GetSize()[1], GL_COLOR_BUFFER_BIT,
      GL_NEAREST);

    // now clear alpha otherwise we end up blending the rendering with
    // background windows in certain cases. It happens on OsX
    // (if QSurfaceFormat::alphaBufferSize() > 0) or when using Mesa on Linux
    // (see paraview/paraview#17159).
    GLboolean colorMask[4];
    f->glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    f->glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);

    GLfloat clearColor[4];
    f->glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    f->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);

    f->glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    f->glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::cleanupContext()
{
  // logger gets uninitialized when this gets called. We get errors from
  // QOpenGLDebugLogger if logMessage is called here. So we just destroy the
  // logger.
  delete this->Logger;
  this->Logger = nullptr;

  vtkQVTKOpenGLSimpleWidgetDebugMacro("cleanupContext");

  // QOpenGLWidget says when this slot is called, the context may not be current
  // and hence is a good practice to make it so.
  this->makeCurrent();
  if (this->RenderWindow)
  {
    if (this->FBO)
    {
      this->FBO->bind();
    }
    this->RenderWindow->Finalize();
    this->RenderWindow->SetReadyForRendering(false);
  }
  delete this->FBO;
  this->FBO = nullptr;
  this->requireRenderWindowInitialization();
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::windowFrameEventCallback()
{
  Q_ASSERT(this->RenderWindow);
  vtkQVTKOpenGLSimpleWidgetDebugMacro("frame");

  if (!this->InPaintGL)
  {
    // Handing vtkOpenGLRenderWindow::Frame is tricky. VTK code traditionally
    // calls `Frame` to indicate that VTK is done rendering 1 frame. Now, when
    // that happens, should we tell Qt to update the widget -- that's the
    // question? In general, yes, but sometimes VTK does things in the
    // background i.e. back buffer without wanting to update the front buffer
    // e.g. when making selections. In that case, we don't want to update Qt
    // widget either, since whatever it did was not meant to be visible.
    // To handle that, we check if vtkOpenGLRenderWindow::SwapBuffers is true,
    // and request an update only when it is.
    if (this->RenderWindow->GetSwapBuffers() || this->RenderWindow->GetDoubleBuffer() == 0)
    {
      // Means that the vtkRenderWindow rendered outside a paintGL call. That can
      // happen when application code call vtkRenderWindow::Render() directly,
      // instead of calling QVTKOpenGLSimpleWidget::update() or letting Qt update the
      // widget. In that case, since QOpenGLWidget rendering into an offscreen
      // FBO, the result still needs to be composed by Qt widget stack. We request
      // that using `update()`.
      vtkQVTKOpenGLSimpleWidgetDebugMacro("update");
      this->update();

      this->DoVTKRenderInPaintGL = false;
    }
    else
    {
      vtkQVTKOpenGLSimpleWidgetDebugMacro("buffer bad -- do not show");

      // Since this->FBO right now is garbage, if paint event is received before
      // a Render request is made on the render window, we will have to Render
      // explicitly.
      this->DoVTKRenderInPaintGL = true;
    }
  }
}

//-----------------------------------------------------------------------------
bool QVTKOpenGLSimpleWidget::renderVTK()
{
  vtkQVTKOpenGLSimpleWidgetDebugMacro("renderVTK");
  Q_ASSERT(this->FBO);
  Q_ASSERT(this->FBO->handle() == this->RenderWindow->GetDefaultFrameBufferId());

  // Bind the FBO we'll be rendering into. This may not be needed, since VTK will
  // bind it anyways, but we'll be extra cautious.
  this->FBO->bind();

  vtkRenderWindowInteractor* iren = this->RenderWindow ? this->RenderWindow->GetInteractor() : nullptr;
  if (iren)
  {
    iren->Render();
  }
  else if (this->RenderWindow)
  {
    this->RenderWindow->Render();
  }
  else
  {
    // no render window set, just fill with white.
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    f->glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    f->glClear(GL_COLOR_BUFFER_BIT);
  }
  return true;
}

//-----------------------------------------------------------------------------
bool QVTKOpenGLSimpleWidget::event(QEvent* evt)
{
  switch (evt->type())
  {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseButtonDblClick:
      // skip events that are explicitly handled by overrides to avoid duplicate
      // calls to InteractorAdapter->ProcessEvent().
      break;

    case QEvent::Resize:
      // we don't let QVTKInteractorAdapter process resize since we handle it
      // in this->recreateFBO().
      break;

#ifdef __APPLE__
    // On MacOS, because of https://bugreports.qt.io/browse/QTBUG-69644
    // It is needed to hide/show currently visible QVTKOpenGLWidget
    // Just after initialization of QVTKOpenGLSimpleWidget
    // This triggers a timer so as soon as Qt is able to process events,
    // it will fix the broken QVTKOpenGLWidgets.
    case QEvent::WindowActivate:
    case QEvent::PolishRequest:
    {
      QWidget* window = this->window();
      QTimer::singleShot(1, [window]() {::QVTKOpenGLWidgetMacOSCheck(window);});
    }
    break;
#endif // __APPLE__

    default:
      if (this->RenderWindow && this->RenderWindow->GetInteractor())
      {
        this->InteractorAdapter->ProcessEvent(evt, this->RenderWindow->GetInteractor());
      }
  }
  return this->Superclass::event(evt);
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::mousePressEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdapter->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::mouseMoveEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdapter->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::mouseReleaseEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdapter->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}

//-----------------------------------------------------------------------------
void QVTKOpenGLSimpleWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
  emit mouseEvent(event);

  if (this->RenderWindow && this->RenderWindow->GetInteractor())
  {
    this->InteractorAdapter->ProcessEvent(event,
                                          this->RenderWindow->GetInteractor());
  }
}
