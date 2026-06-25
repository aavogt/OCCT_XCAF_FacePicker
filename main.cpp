#include <atomic>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

#include <HeaderSection_FileDescription.hxx>
#include <Interface_HArray1OfHAsciiString.hxx>
#include <StepData_StepModel.hxx>

#include <GLFW/glfw3.h>

#include "GlfwOcctWindow.h"

#define DMON_IMPL
#include "dmon.h"

#include <XSControl.hxx>
#include <XSControl_WorkSession.hxx>
// OCCT Core / Framework Data
#include <BinXCAFDrivers.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TDocStd_Application.hxx>
#include <TDocStd_Document.hxx>

// XCAF Data Tools
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

// STEP Reader Engine
#include <STEPCAFControl_Reader.hxx>
#include <Standard_Failure.hxx>

// Visualization & Presentation
#include <AIS_LightSource.hxx>
#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_TypeOfLine.hxx>
#include <Graphic3d_NameOfTextureEnv.hxx>
#include <Graphic3d_TextureEnv.hxx>
#include <Graphic3d_TypeOfShadingModel.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <V3d_View.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_Viewer.hxx>
#include <XCAFPrs_AISObject.hxx>

// Modeling & Structural Helpers
#include <Quantity_Color.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

namespace {
std::string NormalizePath(std::string path) {
  for (char &ch : path) {
    if (ch == '\\') {
      ch = '/';
    }
#ifdef _WIN32
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
#endif
  }

  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }

  return path;
}

struct ModelWatchContext {
  std::atomic_bool *reloadRequested;
  std::string watchedFileAbsolutePath;
};

struct MouseScrollContext {
  double deltaY = 0.0;
};

bool SplitEntryAndSourceLocation(const std::string &line, std::string &entryOut,
                                 std::string &sourceOut) {
  std::size_t splitPos = std::string::npos;
  int colonCount = 0;

  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] != ':') {
      continue;
    }

    ++colonCount;
    if (colonCount == 5) {
      splitPos = i;
      break;
    }
  }

  if (splitPos == std::string::npos || splitPos == 0 ||
      splitPos + 1 >= line.size()) {
    return false;
  }

  entryOut = line.substr(0, splitPos);
  sourceOut = line.substr(splitPos + 1);
  return !entryOut.empty() && !sourceOut.empty();
}

void OnMouseScroll(GLFWwindow *window, double, double yoffset) {
  if (window == nullptr) {
    return;
  }

  auto *scrollContext =
      static_cast<MouseScrollContext *>(glfwGetWindowUserPointer(window));
  if (scrollContext == nullptr) {
    return;
  }

  scrollContext->deltaY += yoffset;
}

void OnModelFileChanged(dmon_watch_id, dmon_action, const char *rootdir,
                        const char *filepath, const char *oldfilepath,
                        void *user) {
  if (user == nullptr) {
    return;
  }

  auto *watchContext = static_cast<ModelWatchContext *>(user);
  auto matchesWatchedFile = [&](const char *relativePath) {
    if (relativePath == nullptr || relativePath[0] == '\0') {
      return false;
    }

    std::string fullPath = rootdir != nullptr ? rootdir : "";
    if (!fullPath.empty() && fullPath.back() != '/') {
      fullPath.push_back('/');
    }
    fullPath += relativePath;

    return NormalizePath(fullPath) == watchContext->watchedFileAbsolutePath;
  };

  if (matchesWatchedFile(filepath) || matchesWatchedFile(oldfilepath)) {
    watchContext->reloadRequested->store(true, std::memory_order_release);
  }
}
} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cout << "Usage: " << argv[0] << " <path_to_step_file.stp>"
              << std::endl;
    return 1;
  }

  const std::filesystem::path stepPathInput(argv[1]);
  const std::filesystem::path stepPathAbsolute =
      std::filesystem::absolute(stepPathInput).lexically_normal();
  const std::string stepPathForOcct = stepPathAbsolute.string();
  const std::string watchedStepFilePath = NormalizePath(stepPathForOcct);

  const std::filesystem::path watchRootPath =
      stepPathAbsolute.parent_path().empty() ? std::filesystem::current_path()
                                             : stepPathAbsolute.parent_path();
  const std::string watchRootForDmon =
      watchRootPath.lexically_normal().string();

  // 1. Initialize an OCAF/XCAF Application Document Context
  Handle(TDocStd_Application) app = new TDocStd_Application();
  BinXCAFDrivers::DefineFormat(app); // Ensure data drivers are registered

  // 2. Create a real GLFW/OpenGL window and bind OCCT view to it
  if (!glfwInit()) {
    std::cerr << "Error: glfwInit() failed." << std::endl;
    return 1;
  }

  glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

  Handle(GlfwOcctWindow) occtWindow =
      new GlfwOcctWindow(1280, 720, "OCCT XCAF Face Picker");
  if (occtWindow->getGlfwWindow() == nullptr) {
    std::cerr << "Error: glfwCreateWindow() failed." << std::endl;
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(occtWindow->getGlfwWindow());
  glfwSwapInterval(1);

  Handle(OpenGl_GraphicDriver) graphicDriver =
      new OpenGl_GraphicDriver(occtWindow->GetDisplay(), Standard_False);

  Handle(V3d_Viewer) viewer = new V3d_Viewer(graphicDriver);
  viewer->SetDefaultShadingModel(Graphic3d_TypeOfShadingModel_Pbr);

  // Custom light rig (dimmer than SetDefaultLights()).
  Handle(V3d_AmbientLight) ambientLight =
      new V3d_AmbientLight(Quantity_Color(0.20, 0.20, 0.20, Quantity_TOC_RGB));
  ambientLight->SetIntensity(0.50f);

  Handle(V3d_DirectionalLight) keyLight =
      new V3d_DirectionalLight(gp_Dir(-0.5, -0.4, -1.0));
  keyLight->SetIntensity(0.90f);

  Handle(V3d_DirectionalLight) fillLight =
      new V3d_DirectionalLight(gp_Dir(0.6, 0.3, -1.0));
  fillLight->SetIntensity(0.70f);

  viewer->AddLight(ambientLight);
  viewer->AddLight(keyLight);
  viewer->AddLight(fillLight);
  viewer->SetLightOn();

  Handle(AIS_InteractiveContext) context = new AIS_InteractiveContext(viewer);
  Handle(V3d_View) view = viewer->CreateView();
  view->SetWindow(occtWindow, occtWindow->NativeGlContext());
  if (!occtWindow->IsMapped()) {
    occtWindow->Map();
  }

  try {
    Handle(Graphic3d_TextureEnv) envTexture =
        new Graphic3d_TextureEnv(Graphic3d_NOT_ENV_SKY2);
    view->SetTextureEnv(envTexture);
    view->SetImageBasedLighting(Standard_True, Standard_False);
  } catch (const Standard_Failure &failure) {
    std::cerr << "Warning: could not enable environment/IBL: "
              << failure.GetMessageString() << std::endl;
  }

  Handle(AIS_LightSource) sceneLightSource;
  for (V3d_ListOfLightIterator lightIt(viewer->ActiveLights()); lightIt.More();
       lightIt.Next()) {
    const Handle(V3d_Light)& activeLight = lightIt.Value();
    if (activeLight.IsNull()) {
      continue;
    }

    sceneLightSource = new AIS_LightSource(activeLight);
    sceneLightSource->SetDisplayName(Standard_True);
    // context->Display(sceneLightSource, Standard_False);
    break;
  }

  Handle(Prs3d_Drawer) defaultDrawer = context->DefaultDrawer();
  defaultDrawer->SetFaceBoundaryDraw(Standard_True);
  defaultDrawer->SetFaceBoundaryAspect(
      new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.0f));
  Handle(Prs3d_ShadingAspect) defaultShadingAspect =
      defaultDrawer->ShadingAspect();
  if (!defaultShadingAspect.IsNull() &&
      !defaultShadingAspect->Aspect().IsNull()) {
    defaultShadingAspect->Aspect()->SetShadingModel(
        Graphic3d_TypeOfShadingModel_Pbr);
  }

  const Standard_Real kModelTransparency = 0.0; // 0.0 = opaque, 1.0 = invisible
  const Standard_Boolean kApplyTintColor = Standard_False;
  const Quantity_Color kTintColor(0.80, 0.88, 1.00, Quantity_TOC_RGB);

  Handle(TDocStd_Document) doc;
  Handle(XCAFDoc_ShapeTool) shapeTool;
  Handle(XCAFDoc_ColorTool) colorTool;
  TDF_Label rootLabel;
  Handle(XCAFPrs_AISObject) xcafPresentation;
  std::unordered_map<std::string, std::string> labelSourceByEntry;

  auto applyPresentationStyling =
      [&](const Handle(XCAFPrs_AISObject) &presentation) {
        if (presentation.IsNull()) {
          return;
        }

        // Reuse object drawer (do not replace XCAF internals), but ensure it is
        // linked to context defaults.
        Handle(Prs3d_Drawer) drawer = presentation->Attributes();
        if (drawer.IsNull()) {
          drawer = new Prs3d_Drawer();
        }
        drawer->Link(context->DefaultDrawer());

        // Black edge overlay in shaded mode.
        drawer->SetFaceBoundaryDraw(Standard_True);
        drawer->SetFaceBoundaryAspect(
            new Prs3d_LineAspect(Quantity_NOC_BLACK, Aspect_TOL_SOLID, 1.0f));

        // Restore PBR shading model per-object (XCAF presentations may have own
        // aspects that override context defaults).
        Handle(Prs3d_ShadingAspect) shadingAspect = drawer->ShadingAspect();
        if (shadingAspect.IsNull()) {
          shadingAspect = new Prs3d_ShadingAspect();
          drawer->SetShadingAspect(shadingAspect);
        }
        Handle(Graphic3d_AspectFillArea3d) fillAspect = shadingAspect->Aspect();
        if (!fillAspect.IsNull()) {
          fillAspect->SetShadingModel(Graphic3d_TypeOfShadingModel_Pbr);
        }

        presentation->SetAttributes(drawer);
        presentation->SynchronizeAspects();
      };

  auto loadModelFromDisk = [&]() -> bool {
    Handle(TDocStd_Document) newDoc;
    app->NewDocument("BinXCAF", newDoc);

    STEPCAFControl_Reader reader;
    reader.SetColorMode(Standard_True);
    reader.SetNameMode(Standard_True);
    reader.SetLayerMode(Standard_True);

    IFSelect_ReturnStatus readStatus = reader.ReadFile(stepPathForOcct.c_str());
    if (readStatus != IFSelect_RetDone) {
      std::cerr << "Error: Unable to parse or read file: " << stepPathForOcct
                << std::endl;
      app->Close(newDoc);
      return false;
    }

    labelSourceByEntry.clear();
    {
      Handle(StepData_StepModel) model =
          Handle(StepData_StepModel)::DownCast(reader.Reader().WS()->Model());
      if (!model.IsNull()) {
        Handle(HeaderSection_FileDescription) fileDescription =
            Handle(HeaderSection_FileDescription)::DownCast(model->HeaderEntity(
                STANDARD_TYPE(HeaderSection_FileDescription)));
        if (!fileDescription.IsNull()) {
          Handle(Interface_HArray1OfHAsciiString) descriptions =
              fileDescription->Description();
          if (!descriptions.IsNull()) {
            for (Standard_Integer i = descriptions->Lower();
                 i <= descriptions->Upper(); ++i) {
              const Handle(TCollection_HAsciiString) &lineH =
                  descriptions->Value(i);
              if (lineH.IsNull()) {
                continue;
              }

              const std::string line = lineH->ToCString();
              std::string entry;
              std::string source;
              if (SplitEntryAndSourceLocation(line, entry, source)) {
                labelSourceByEntry[entry] = source;
              }
            }
          }
        }
      }
    }

    if (!reader.Transfer(newDoc)) {
      std::cerr << "Error: Failsafe triggered. Could not transfer STEP data to "
                   "the XCAF document framework."
                << std::endl;
      app->Close(newDoc);
      return false;
    }

    Handle(XCAFDoc_ShapeTool) newShapeTool =
        XCAFDoc_DocumentTool::ShapeTool(newDoc->Main());
    Handle(XCAFDoc_ColorTool) newColorTool =
        XCAFDoc_DocumentTool::ColorTool(newDoc->Main());

    TDF_LabelSequence freeShapes;
    newShapeTool->GetFreeShapes(freeShapes);
    if (freeShapes.IsEmpty()) {
      std::cerr << "Error: Document context yields no free structural "
                   "components."
                << std::endl;
      app->Close(newDoc);
      return false;
    }

    const TDF_Label newRootLabel = freeShapes.First();
    Handle(XCAFPrs_AISObject) newPresentation =
        new XCAFPrs_AISObject(newRootLabel);
    applyPresentationStyling(newPresentation);

    Handle(TDocStd_Document) oldDoc = doc;
    Handle(XCAFPrs_AISObject) oldPresentation = xcafPresentation;

    doc = newDoc;
    shapeTool = newShapeTool;
    colorTool = newColorTool;
    rootLabel = newRootLabel;
    xcafPresentation = newPresentation;

    if (!oldPresentation.IsNull()) {
      context->Remove(oldPresentation, Standard_False);
    }

    context->Display(xcafPresentation, AIS_Shaded, 0, Standard_True);
    context->SetSelectionModeActive(xcafPresentation, 0, Standard_False);
    context->SetSelectionModeActive(xcafPresentation, 4, Standard_True);
    context->SetTransparency(xcafPresentation, kModelTransparency,
                             Standard_False);
    if (kApplyTintColor) {
      context->SetColor(xcafPresentation, kTintColor, Standard_False);
    }

    view->FitAll();
    view->ZFitAll();
    context->UpdateCurrentViewer();

    if (!oldDoc.IsNull()) {
      app->Close(oldDoc);
    }

    return true;
  };

  if (!loadModelFromDisk()) {
    glfwTerminate();
    return 1;
  }

  std::atomic_bool reloadRequested(false);
  ModelWatchContext watchContext{&reloadRequested, watchedStepFilePath};
  MouseScrollContext scrollContext;
  glfwSetWindowUserPointer(occtWindow->getGlfwWindow(), &scrollContext);
  glfwSetScrollCallback(occtWindow->getGlfwWindow(), OnMouseScroll);

  dmon_init();
  dmon_watch_id watchId = dmon_watch(watchRootForDmon.c_str(),
                                     OnModelFileChanged, 0, &watchContext);
  if (watchId.id == 0) {
    std::cerr << "Warning: dmon could not watch directory: " << watchRootForDmon
              << std::endl;
  }

  std::cout
      << "Left click a face to print its XCAF label/color. Press ESC to quit."
      << std::endl;
  std::cout << "Watching model file for changes: " << stepPathForOcct
            << std::endl;

  bool wasLeftPressed = false;
  bool wasRightPressed = false;
  bool wasMiddlePressed = false;

  double rotateStartX = 0.0;
  double rotateStartY = 0.0;
  double panStartX = 0.0;
  double panStartY = 0.0;

  int lastFbWidth = 0;
  int lastFbHeight = 0;
  glfwGetFramebufferSize(occtWindow->getGlfwWindow(), &lastFbWidth,
                         &lastFbHeight);

  // 3. Event/render loop
  while (!glfwWindowShouldClose(occtWindow->getGlfwWindow())) {
    glfwPollEvents();

    if (glfwGetKey(occtWindow->getGlfwWindow(), GLFW_KEY_ESCAPE) ==
        GLFW_PRESS) {
      glfwSetWindowShouldClose(occtWindow->getGlfwWindow(), GLFW_TRUE);
    }

    if (reloadRequested.exchange(false, std::memory_order_acquire)) {
      if (loadModelFromDisk()) {
        std::cout << "Model reloaded from disk." << std::endl;
      } else {
        std::cerr << "Model reload failed; keeping previous scene."
                  << std::endl;
      }
    }

    glfwMakeContextCurrent(occtWindow->getGlfwWindow());

    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(occtWindow->getGlfwWindow(), &fbWidth, &fbHeight);
    if (fbWidth != lastFbWidth || fbHeight != lastFbHeight) {
      occtWindow->DoResize();
      view->MustBeResized();
      lastFbWidth = fbWidth;
      lastFbHeight = fbHeight;
    }

    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(occtWindow->getGlfwWindow(), &mouseX, &mouseY);

    if (scrollContext.deltaY != 0.0) {
      const Standard_Real zoomFactor =
          (Standard_Real)std::pow(1.12, scrollContext.deltaY);
      view->SetZoom(zoomFactor, Standard_True);
      scrollContext.deltaY = 0.0;
    }

    context->MoveTo((Standard_Integer)mouseX, (Standard_Integer)mouseY, view,
                    Standard_True);

    const bool isLeftPressed =
        glfwGetMouseButton(occtWindow->getGlfwWindow(),
                           GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool isRightPressed =
        glfwGetMouseButton(occtWindow->getGlfwWindow(),
                           GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool isMiddlePressed =
        glfwGetMouseButton(occtWindow->getGlfwWindow(),
                           GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

    // Right mouse drag: orbit/rotate camera.
    if (isRightPressed) {
      if (!wasRightPressed) {
        rotateStartX = mouseX;
        rotateStartY = mouseY;
      }
      const Standard_Real dx = (Standard_Real)(mouseX - rotateStartX);
      const Standard_Real dy = (Standard_Real)(mouseY - rotateStartY);
      // Rotate(x,y,...)
      view->Rotate(dx * 0.005, -dy * 0.005, 0.0,
                   wasRightPressed ? Standard_False : Standard_True);
    }

    // Middle mouse drag: pan/translate camera.
    if (isMiddlePressed) {
      if (!wasMiddlePressed) {
        panStartX = mouseX;
        panStartY = mouseY;
      }
      const Standard_Real dx = (Standard_Real)(mouseX - panStartX);
      const Standard_Real dy = (Standard_Real)(mouseY - panStartY);
      const Standard_Real dxView = view->Convert((Standard_Integer)dx);
      const Standard_Real dyView = view->Convert((Standard_Integer)-dy);
      view->Panning(dxView, dyView, 1.0,
                    wasMiddlePressed ? Standard_False : Standard_True);
    }

    // Left click: select/detect face.
    if (isLeftPressed && !wasLeftPressed && !isRightPressed &&
        !isMiddlePressed) {
      context->SelectDetected();

      for (context->InitSelected(); context->MoreSelected();
           context->NextSelected()) {
        Handle(SelectMgr_EntityOwner) owner = context->SelectedOwner();
        Handle(StdSelect_BRepOwner) brepOwner =
            Handle(StdSelect_BRepOwner)::DownCast(owner);

        if (brepOwner.IsNull() || shapeTool.IsNull() || colorTool.IsNull()) {
          continue;
        }

        TopoDS_Shape pickedShape = brepOwner->Shape();
        if (pickedShape.ShapeType() != TopAbs_FACE) {
          continue;
        }

        TopoDS_Face pickedFace = TopoDS::Face(pickedShape);
        TDF_Label targetFaceLabel;
        if (!shapeTool->FindSubShape(rootLabel, pickedFace, targetFaceLabel)) {
          continue;
        }

        TCollection_AsciiString pickedEntry;
        TDF_Tool::Entry(targetFaceLabel, pickedEntry);
        const std::string pickedEntryStr = pickedEntry.ToCString();

        std::cout << "Detected Sub-Shape Label Reference: " << pickedEntryStr;
        const auto sourceIt = labelSourceByEntry.find(pickedEntryStr);
        if (sourceIt != labelSourceByEntry.end()) {
          std::cout << " (" << sourceIt->second << ")";
        }
        std::cout << std::endl;

        Quantity_Color exactColor;
        if (colorTool->GetColor(targetFaceLabel, XCAFDoc_ColorSurf,
                                exactColor)) {
          std::cout << "  -> Direct Face Surface Color (RGB): "
                    << exactColor.Red() << ", " << exactColor.Green() << ", "
                    << exactColor.Blue() << std::endl;
        } else {
          std::cout << "  -> Direct face color attribute missing from this "
                       "individual face sub-label."
                    << std::endl;
        }
      }
    }

    wasLeftPressed = isLeftPressed;
    wasRightPressed = isRightPressed;
    wasMiddlePressed = isMiddlePressed;

    view->Redraw();
  }

  if (watchId.id != 0) {
    dmon_unwatch(watchId);
  }
  dmon_deinit();

  if (!doc.IsNull()) {
    app->Close(doc); // calls glfwTerminate() no need to add it below
  }
  return 0;
}
