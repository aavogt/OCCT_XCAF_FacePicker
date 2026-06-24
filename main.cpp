#include <iostream>

#include <GLFW/glfw3.h>

#include "GlfwOcctWindow.h"

// OCCT Core / Framework Data
#include <BinXCAFDrivers.hxx>
#include <TDF_Label.hxx>
#include <TDocStd_Application.hxx>
#include <TDocStd_Document.hxx>

// XCAF Data Tools
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

// STEP Reader Engine
#include <STEPCAFControl_Reader.hxx>

// Visualization & Presentation
#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <XCAFPrs_AISObject.hxx>

// Modeling & Structural Helpers
#include <Quantity_Color.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cout << "Usage: " << argv[0] << " <path_to_step_file.stp>"
              << std::endl;
    return 1;
  }
  TCollection_AsciiString filePath(argv[1]);

  // 1. Initialize an OCAF/XCAF Application Document Context
  Handle(TDocStd_Application) app = new TDocStd_Application();
  BinXCAFDrivers::DefineFormat(app); // Ensure data drivers are registered
  Handle(TDocStd_Document) doc;
  app->NewDocument("BinXCAF", doc);

  // 2. Read the STEP File into the XCAF Document with Color/Name parsing
  // enabled
  STEPCAFControl_Reader reader;
  reader.SetColorMode(Standard_True);
  reader.SetNameMode(Standard_True);
  reader.SetLayerMode(Standard_True);

  IFSelect_ReturnStatus readStatus = reader.ReadFile(filePath.ToCString());
  if (readStatus != IFSelect_RetDone) {
    std::cerr << "Error: Unable to parse or read file: " << filePath.ToCString()
              << std::endl;
    return 1;
  }

  if (!reader.Transfer(doc)) {
    std::cerr << "Error: Failsafe triggered. Could not transfer STEP data to "
                 "the XCAF document framework."
              << std::endl;
    return 1;
  }

  // 3. Extract the Primary Model Tools from the Root Node
  Handle(XCAFDoc_ShapeTool) shapeTool =
      XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  Handle(XCAFDoc_ColorTool) colorTool =
      XCAFDoc_DocumentTool::ColorTool(doc->Main());

  // 4. Create a real GLFW/OpenGL window and bind OCCT view to it
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
  viewer->SetDefaultLights();
  viewer->SetLightOn();

  Handle(AIS_InteractiveContext) context = new AIS_InteractiveContext(viewer);
  Handle(V3d_View) view = viewer->CreateView();
  view->SetWindow(occtWindow, occtWindow->NativeGlContext());
  if (!occtWindow->IsMapped()) {
    occtWindow->Map();
  }

  // 5. Query Assembly Information and attach presentation
  TDF_LabelSequence freeShapes;
  shapeTool->GetFreeShapes(freeShapes);
  if (freeShapes.IsEmpty()) {
    std::cerr << "Error: Document context yields no free structural components."
              << std::endl;
    glfwTerminate();
    return 1;
  }

  TDF_Label rootLabel = freeShapes.First();
  Handle(XCAFPrs_AISObject) xcafPresentation = new XCAFPrs_AISObject(rootLabel);

  // Render as solid shaded faces (instead of wireframe).
  context->Display(xcafPresentation, AIS_Shaded, 0, Standard_True);
  context->SetSelectionModeActive(xcafPresentation, 4, Standard_True);

  // Optional visual tuning.
  const Standard_Real kModelTransparency = 0.15; // 0.0 = opaque, 1.0 = invisible
  const Standard_Boolean kApplyTintColor = Standard_False;
  const Quantity_Color kTintColor(0.80, 0.88, 1.00, Quantity_TOC_RGB);
  context->SetTransparency(xcafPresentation, kModelTransparency, Standard_False);
  if (kApplyTintColor) {
    context->SetColor(xcafPresentation, kTintColor, Standard_False);
  }

  view->FitAll();
  view->ZFitAll();
  context->UpdateCurrentViewer();

  std::cout
      << "Left click a face to print its XCAF label/color. Press ESC to quit."
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

  // 6. Event/render loop
  while (!glfwWindowShouldClose(occtWindow->getGlfwWindow())) {
    glfwPollEvents();

    if (glfwGetKey(occtWindow->getGlfwWindow(), GLFW_KEY_ESCAPE) ==
        GLFW_PRESS) {
      glfwSetWindowShouldClose(occtWindow->getGlfwWindow(), GLFW_TRUE);
    }

    glfwMakeContextCurrent(occtWindow->getGlfwWindow());

    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(occtWindow->getGlfwWindow(), &fbWidth, &fbHeight);
    if (fbWidth != lastFbWidth || fbHeight != lastFbHeight) {
      view->MustBeResized();
      lastFbWidth = fbWidth;
      lastFbHeight = fbHeight;
    }

    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(occtWindow->getGlfwWindow(), &mouseX, &mouseY);
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
    if (isLeftPressed && !wasLeftPressed && !isRightPressed && !isMiddlePressed) {
      context->SelectDetected();

      for (context->InitSelected(); context->MoreSelected();
           context->NextSelected()) {
        Handle(SelectMgr_EntityOwner) owner = context->SelectedOwner();
        Handle(StdSelect_BRepOwner) brepOwner =
            Handle(StdSelect_BRepOwner)::DownCast(owner);

        if (brepOwner.IsNull()) {
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

        std::cout << "Detected Sub-Shape Label Reference: ";
        targetFaceLabel.EntryDump(std::cout);
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

  // Clean up allocated storage bounds before shutdown
  app->Close(doc);
  return 0;
}
