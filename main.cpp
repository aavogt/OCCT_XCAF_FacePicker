#include <iostream>

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

  // 4. Initialize a visual processing context
  Handle(Aspect_DisplayConnection) displayConnection =
      new Aspect_DisplayConnection();
  Handle(OpenGl_GraphicDriver) graphicDriver =
      new OpenGl_GraphicDriver(displayConnection, Standard_False);
  Handle(V3d_Viewer) viewer = new V3d_Viewer(graphicDriver);
  Handle(AIS_InteractiveContext) context = new AIS_InteractiveContext(viewer);
  Handle(V3d_View) view = viewer->CreateView();

  // 5. Query Assembly Information and Attach Presentation Overrides
  TDF_LabelSequence freeShapes;
  shapeTool->GetFreeShapes(freeShapes);
  if (freeShapes.IsEmpty()) {
    std::cerr << "Error: Document context yields no free structural components."
              << std::endl;
    return 1;
  }

  // Capture the primary topological target node
  TDF_Label rootLabel = freeShapes.First();
  Handle(XCAFPrs_AISObject) xcafPresentation = new XCAFPrs_AISObject(rootLabel);

  // Display presentation and activate Face Sub-Shape Selectors (Mode 4)
  context->Display(xcafPresentation, Standard_True);
  context->SetSelectionModeActive(xcafPresentation, 4, Standard_True);

  // Update viewing matrices to encompass bounds of loaded models
  view->FitAll();
  context->UpdateCurrentViewer();

  std::cout << "\n--- Simulating Graphic Pick Event ---" << std::endl;

  // 6. Visual Selection Simulation Pipeline
  // Real applications run this under mouse interactions (e.g.
  // context->SelectDetected()) For offscreen mapping tracking, select
  // everything in the bounding canvas to test faces
  context->SelectDetected();

  // Loop through the selected items
  for (context->InitSelected(); context->MoreSelected();
       context->NextSelected()) {
    Handle(SelectMgr_EntityOwner) owner = context->SelectedOwner();
    Handle(StdSelect_BRepOwner) brepOwner =
        Handle(StdSelect_BRepOwner)::DownCast(owner);

    if (!brepOwner.IsNull()) {
      TopoDS_Shape pickedShape = brepOwner->Shape();

      if (pickedShape.ShapeType() == TopAbs_FACE) {
        TopoDS_Face pickedFace = TopoDS::Face(pickedShape);

        // 7. Directly Check the Per-Face Label Bindings inside XCAF
        TDF_Label targetFaceLabel;
        if (shapeTool->FindSubShape(rootLabel, pickedFace, targetFaceLabel)) {
          std::cout << "Detected Sub-Shape Label Reference: ";
          targetFaceLabel.EntryDump(std::cout);
          std::cout << std::endl;

          Quantity_Color exactColor;
          // Directly queries face label data bypassing hierarchical walking
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
    }
  }

  // Clean up allocated storage bounds before shutdown
  app->Close(doc);
  return 0;
}
