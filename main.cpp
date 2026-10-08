#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

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
#include <TCollection_ExtendedString.hxx>
#include <TColStd_HSequenceOfExtendedString.hxx>
#include <TDF_Label.hxx>
#include <TDF_Tool.hxx>
#include <TDocStd_Application.hxx>
#include <TDocStd_Document.hxx>

// XCAF Data Tools
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_LayerTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

// STEP Reader Engine
#include <STEPCAFControl_Reader.hxx>
#include <Standard_Failure.hxx>

// Visualization & Presentation
#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_LightSource.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TexturedShape.hxx>
#include <AIS_TextLabel.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Aspect_TypeOfLine.hxx>
#include <Graphic3d_NameOfTextureEnv.hxx>
#include <Graphic3d_TextureEnv.hxx>
#include <Graphic3d_TransformPers.hxx>
#include <Graphic3d_TypeOfShadingModel.hxx>
#include <Graphic3d_MaterialAspect.hxx>
#include <Graphic3d_PBRMaterial.hxx>
#include <Graphic3d_ZLayerId.hxx>
#include <Image_Format.hxx>
#include <Image_PixMap.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <Prs3d_Drawer.hxx>
#include <Prs3d_LineAspect.hxx>
#include <Prs3d_ShadingAspect.hxx>
#include <StdSelect_BRepOwner.hxx>
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <XCAFPrs_AISObject.hxx>

// Modeling & Structural Helpers
#include <BRepIntCurveSurface_Inter.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Quantity_Color.hxx>
#include <Geom_Plane.hxx>
#include <Precision.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <gp_Trsf.hxx>

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

struct SourcePosition {
  std::string filePath;
  int row = 0;
  int column = 0;
};

struct CameraState {
  Standard_Real eyeX = 0.0;
  Standard_Real eyeY = 0.0;
  Standard_Real eyeZ = 0.0;
  Standard_Real atX = 0.0;
  Standard_Real atY = 0.0;
  Standard_Real atZ = 0.0;
  Standard_Real upX = 0.0;
  Standard_Real upY = 0.0;
  Standard_Real upZ = 0.0;
  bool hasScreenPoint = false;
  Standard_Real screenX = 0.0;
  Standard_Real screenY = 0.0;
};
struct VertexReplayQuery {
  std::string modelPath;
  Standard_Integer vertexIndex = 0;
};

struct MouseReplayQuery {
  std::string modelPath;
  Standard_Integer mouseX = 0;
  Standard_Integer mouseY = 0;
  CameraState camera;
};

struct SourceQuery {
  SourcePosition position;
  bool preferRightOnExact = false;
};

struct SourcePositionLess {
  bool operator()(const SourcePosition &lhs, const SourcePosition &rhs) const {
    if (lhs.filePath != rhs.filePath) {
      return lhs.filePath < rhs.filePath;
    }
    if (lhs.row != rhs.row) {
      return lhs.row < rhs.row;
    }
    return lhs.column < rhs.column;
  }
};

struct ForwardNavigationData {
  std::multimap<SourcePosition, std::string, SourcePositionLess> entryBySource;
};

#ifndef _WIN32
struct SockaddrUn {
  sockaddr_un addr{};
  socklen_t len = 0;
};
#endif

bool ParseStrictPositiveInt(std::string_view text, int &valueOut) {
  if (text.empty()) {
    return false;
  }

  long value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10 + static_cast<long>(ch - '0');
    if (value > static_cast<long>(std::numeric_limits<int>::max())) {
      return false;
    }
  }

  if (value <= 0) {
    return false;
  }

  valueOut = static_cast<int>(value);
  return true;
}

bool ParseSourcePosition(const std::string &source,
                         SourcePosition &positionOut) {
  const std::size_t firstColon = source.rfind(':');
  if (firstColon == std::string::npos || firstColon + 1 >= source.size()) {
    return false;
  }

  const std::size_t secondColon = source.rfind(':', firstColon - 1);
  if (secondColon == std::string::npos || secondColon == 0) {
    return false;
  }

  const std::string filePart = source.substr(0, secondColon);
  const std::string rowPart =
      source.substr(secondColon + 1, firstColon - secondColon - 1);
  const std::string columnPart = source.substr(firstColon + 1);

  int row = 0;
  int column = 0;
  if (!ParseStrictPositiveInt(rowPart, row) ||
      !ParseStrictPositiveInt(columnPart, column)) {
    return false;
  }

  positionOut.filePath = NormalizePath(filePart);
  if (positionOut.filePath.empty()) {
    return false;
  }

  positionOut.row = row;
  positionOut.column = column;
  return true;
}

bool ParseForwardQuery(std::string queryRaw, SourceQuery &queryOut,
                       std::string &canonicalSourceOut, std::string &errorOut) {
  while (!queryRaw.empty() &&
         (queryRaw.back() == '\n' || queryRaw.back() == '\r' ||
          std::isspace(static_cast<unsigned char>(queryRaw.back())))) {
    queryRaw.pop_back();
  }

  std::size_t start = 0;
  while (start < queryRaw.size() &&
         std::isspace(static_cast<unsigned char>(queryRaw[start]))) {
    ++start;
  }
  if (start > 0) {
    queryRaw.erase(0, start);
  }

  if (queryRaw.empty()) {
    errorOut = "query is empty";
    return false;
  }

  const std::size_t col2 = queryRaw.rfind(':');
  if (col2 == std::string::npos || col2 + 1 >= queryRaw.size()) {
    errorOut = "missing :column segment";
    return false;
  }

  const std::size_t col1 = queryRaw.rfind(':', col2 - 1);
  if (col1 == std::string::npos || col1 == 0) {
    errorOut = "missing :line segment";
    return false;
  }

  std::string filePart = queryRaw.substr(0, col1);
  std::string linePart = queryRaw.substr(col1 + 1, col2 - col1 - 1);
  std::string columnPart = queryRaw.substr(col2 + 1);

  bool lineHadPlus = false;
  bool columnHadPlus = false;

  if (!linePart.empty() && linePart.front() == '+') {
    lineHadPlus = true;
    linePart.erase(0, 1);
  }
  if (!columnPart.empty() && columnPart.front() == '+') {
    columnHadPlus = true;
    columnPart.erase(0, 1);
  }

  int row = 0;
  int column = 0;
  if (!ParseStrictPositiveInt(linePart, row) ||
      !ParseStrictPositiveInt(columnPart, column)) {
    errorOut = "line/column must be positive integers";
    return false;
  }

  queryOut.position.filePath = NormalizePath(filePart);
  if (queryOut.position.filePath.empty()) {
    errorOut = "file path is empty";
    return false;
  }

  queryOut.position.row = row;
  queryOut.position.column = column;
  queryOut.preferRightOnExact = lineHadPlus || columnHadPlus;

  canonicalSourceOut = queryOut.position.filePath + ":" + std::to_string(row) +
                       ":" + std::to_string(column);
  return true;
}

bool ParseVertexReplayQuery(const std::string &queryRaw,
                            VertexReplayQuery &queryOut) {
  std::string query = queryRaw;
  while (!query.empty() &&
         std::isspace(static_cast<unsigned char>(query.back()))) {
    query.pop_back();
  }
  const std::size_t firstNonSpace = query.find_first_not_of(" \t\n\r");
  if (firstNonSpace == std::string::npos) {
    return false;
  }
  if (firstNonSpace > 0) {
    query.erase(0, firstNonSpace);
  }

  const std::size_t vertexMarker = query.rfind(":v");
  if (vertexMarker == std::string::npos || vertexMarker + 2 >= query.size()) {
    return false;
  }

  int vertexIndex = 0;
  if (!ParseStrictPositiveInt(std::string_view(query).substr(vertexMarker + 2),
                              vertexIndex)) {
    return false;
  }

  const std::size_t entrySeparator = query.find(":0:");
  if (entrySeparator == std::string::npos || entrySeparator == 0 ||
      entrySeparator >= vertexMarker) {
    return false;
  }

  const std::string modelPath = query.substr(0, entrySeparator);
  const std::string entry =
      query.substr(entrySeparator + 1, vertexMarker - entrySeparator - 1);
  if (entry.size() < 2 || entry.compare(0, 2, "0:") != 0) {
    return false;
  }

  queryOut.modelPath = NormalizePath(modelPath);
  queryOut.vertexIndex = vertexIndex;
  return !queryOut.modelPath.empty();
}

bool TryGetFaceLabelFromEntry(const Handle(TDocStd_Document) & document,
                              const std::string &entry, TDF_Label &labelOut) {
  if (document.IsNull() || entry.empty()) {
    return false;
  }

  TDF_Label label;
  TDF_Tool::Label(document->GetData(), entry.c_str(), label, Standard_False);
  if (label.IsNull()) {
    return false;
  }

  labelOut = label;
  return true;
}

void HighlightFacesForEntries(const std::vector<std::string> &entries,
                              const Handle(TDocStd_Document) & document,
                              const Handle(XCAFDoc_ShapeTool) & shapeTool,
                              const Handle(AIS_InteractiveContext) & context,
                              const Handle(V3d_View) & view,
                              Handle(AIS_Shape) & highlightPresentation) {
  if (context.IsNull() || view.IsNull()) {
    return;
  }

  if (!highlightPresentation.IsNull()) {
    context->Remove(highlightPresentation, Standard_False);
    highlightPresentation.Nullify();
  }

  if (document.IsNull() || shapeTool.IsNull() || entries.empty()) {
    return;
  }

  BRep_Builder builder;
  TopoDS_Compound compound;
  builder.MakeCompound(compound);

  bool hasFaces = false;
  for (const std::string &entry : entries) {
    TDF_Label faceLabel;
    if (!TryGetFaceLabelFromEntry(document, entry, faceLabel)) {
      continue;
    }

    TopoDS_Shape faceShape;
    if (!shapeTool->GetShape(faceLabel, faceShape) || faceShape.IsNull()) {
      continue;
    }

    if (faceShape.ShapeType() == TopAbs_FACE) {
      builder.Add(compound, faceShape);
      hasFaces = true;
      continue;
    }

    for (TopExp_Explorer exp(faceShape, TopAbs_FACE); exp.More(); exp.Next()) {
      builder.Add(compound, exp.Current());
      hasFaces = true;
    }
  }

  if (!hasFaces) {
    return;
  }

  context->UpdateCurrentViewer();
}

#ifndef _WIN32
bool ReadSingleDatagramLine(int socketFd, std::string &lineOut) {
  lineOut.clear();

  char buffer[1024];
  const ssize_t bytesRead = recv(socketFd, buffer, sizeof(buffer) - 1, 0);
  if (bytesRead < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return false;
    }
    std::cerr << "Warning: recv() failed on forward-nav socket: "
              << std::strerror(errno) << std::endl;
    return false;
  }

  if (bytesRead == 0) {
    return false;
  }

  buffer[bytesRead] = '\0';
  lineOut.assign(buffer, static_cast<std::size_t>(bytesRead));
  return true;
}

SockaddrUn MakeSockaddrUn(const std::filesystem::path &socketPath) {
  SockaddrUn result;
  result.addr.sun_family = AF_UNIX;

  const std::string pathString = socketPath.string();
  const std::size_t maxLen = sizeof(result.addr.sun_path) - 1;
  std::strncpy(result.addr.sun_path, pathString.c_str(), maxLen);
  result.addr.sun_path[maxLen] = '\0';

  result.len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                      std::strlen(result.addr.sun_path) + 1);
  return result;
}

bool SendForwardNavigationRequest(const std::filesystem::path &socketPath,
                                  const std::string &queryLine,
                                  std::string &errorOut, int *errorCodeOut) {
  errorOut.clear();
  if (errorCodeOut != nullptr) {
    *errorCodeOut = 0;
  }

  if (queryLine.empty()) {
    errorOut = "query is empty";
    return false;
  }

  const std::string socketPathString = socketPath.string();
  if (socketPathString.size() >= sizeof(sockaddr_un::sun_path)) {
    errorOut = "socket path is too long";
    return false;
  }

  const int clientFd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (clientFd < 0) {
    const int err = errno;
    if (errorCodeOut != nullptr) {
      *errorCodeOut = err;
    }
    errorOut = std::string("socket() failed: ") + std::strerror(err);
    return false;
  }

  const SockaddrUn addr = MakeSockaddrUn(socketPath);
  const std::string payload = queryLine + "\n";

  const ssize_t bytesSent =
      sendto(clientFd, payload.data(), payload.size(), 0,
             reinterpret_cast<const sockaddr *>(&addr.addr), addr.len);
  const int sendErr = (bytesSent < 0) ? errno : 0;
  const int closeRc = close(clientFd);
  (void)closeRc;

  if (bytesSent < 0) {
    if (errorCodeOut != nullptr) {
      *errorCodeOut = sendErr;
    }
    errorOut = std::string("sendto() failed: ") + std::strerror(sendErr);
    return false;
  }

  return true;
}
#endif

std::string ToCanonicalSource(const SourcePosition &position) {
  return position.filePath + ":" + std::to_string(position.row) + ":" +
         std::to_string(position.column);
}

void BuildForwardNavigationData(
    const std::unordered_map<std::string, std::string> &labelSourceByEntry,
    ForwardNavigationData &forwardNavigationData) {
  forwardNavigationData.entryBySource.clear();

  for (const auto &pair : labelSourceByEntry) {
    SourcePosition position;
    if (!ParseSourcePosition(pair.second, position)) {
      continue;
    }

    forwardNavigationData.entryBySource.emplace(position, pair.first);
  }
}

bool ResolveForwardNavigationQuery(
    const ForwardNavigationData &forwardNavigationData,
    const SourceQuery &query, std::string &resolvedSourceOut,
    std::vector<std::string> &resolvedEntriesOut) {
  resolvedSourceOut.clear();
  resolvedEntriesOut.clear();

  if (forwardNavigationData.entryBySource.empty()) {
    return false;
  }

  const SourcePosition &queryPos = query.position;
  const auto &entryBySource = forwardNavigationData.entryBySource;

  const auto exactRange = entryBySource.equal_range(queryPos);
  const bool hasExactMatch = exactRange.first != exactRange.second;

  auto collectEntriesAt = [&](const SourcePosition &key) {
    auto range = entryBySource.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
      resolvedEntriesOut.push_back(it->second);
    }
    resolvedSourceOut = ToCanonicalSource(key);
  };

  if (!query.preferRightOnExact && hasExactMatch) {
    collectEntriesAt(queryPos);
    return !resolvedEntriesOut.empty();
  }

  if (query.preferRightOnExact) {
    auto it = entryBySource.upper_bound(queryPos);
    if (it != entryBySource.end() && it->first.filePath == queryPos.filePath) {
      collectEntriesAt(it->first);
      return !resolvedEntriesOut.empty();
    }

    if (hasExactMatch) {
      collectEntriesAt(queryPos);
      return !resolvedEntriesOut.empty();
    }
    return false;
  }

  auto lower = entryBySource.lower_bound(queryPos);
  while (lower != entryBySource.begin()) {
    auto prev = std::prev(lower);
    if (prev->first.filePath != queryPos.filePath) {
      break;
    }

    collectEntriesAt(prev->first);
    return !resolvedEntriesOut.empty();
  }

  return false;
}

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

static std::atomic_bool dirty{true};

void OnWindowRefresh(GLFWwindow *) {
  dirty.store(true, std::memory_order_release);
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
  dirty = true;
}

bool LaunchNvimRemote(const std::string &sourceLocation) {
#ifdef _WIN32
  (void)sourceLocation;
  std::cerr << "Warning: nvim-remote.sh launch is not supported on Windows."
            << std::endl;
  return false;
#else
  if (sourceLocation.empty()) {
    return false;
  }
  signal(SIGINT, SIG_DFL);
  pid_t pid = fork();
  if (pid < 0) {
    std::cerr << "Warning: fork() failed while launching nvim-remote.sh."
              << std::endl;
    return false;
  }
  if (pid == 0) {
    if (daemon(1, 0) < 0) {
      perror("daemon");
      exit(1);
    }
    char *const args[] = {const_cast<char *>("nvim-remote.sh"),
                          const_cast<char *>(sourceLocation.c_str()), nullptr};
    execvp(args[0], args);
    _exit(127);
  }
  return true;
#endif
}

bool ParseCameraVector(const std::string &text, Standard_Real &x,
                       Standard_Real &y, Standard_Real &z) {
  std::istringstream values(text);
  char comma1 = '\0';
  char comma2 = '\0';
  if (!(values >> x >> comma1 >> y >> comma2 >> z) || comma1 != ',' ||
      comma2 != ',') {
    return false;
  }
  values >> std::ws;
  return values.eof() && std::isfinite(x) && std::isfinite(y) &&
         std::isfinite(z);
}

bool ParseCameraPoint(const std::string &text, Standard_Real &x,
                      Standard_Real &y) {
  std::istringstream values(text);
  char comma = '\0';
  if (!(values >> x >> comma >> y) || comma != ',') {
    return false;
  }
  values >> std::ws;
  return values.eof() && std::isfinite(x) && std::isfinite(y);
}

bool ParseCameraState(const std::string &text, CameraState &stateOut) {
  std::istringstream vectors(text);
  std::string eye;
  std::string at;
  std::string up;
  std::string screenPoint;
  if (!std::getline(vectors, eye, ':') || !std::getline(vectors, at, ':') ||
      !std::getline(vectors, up, ':')) {
    return false;
  }
  stateOut.hasScreenPoint =
      static_cast<bool>(std::getline(vectors, screenPoint, ':'));
  if (stateOut.hasScreenPoint && std::getline(vectors, screenPoint, ':')) {
    return false;
  }
  if (!ParseCameraVector(eye, stateOut.eyeX, stateOut.eyeY, stateOut.eyeZ) ||
      !ParseCameraVector(at, stateOut.atX, stateOut.atY, stateOut.atZ) ||
      !ParseCameraVector(up, stateOut.upX, stateOut.upY, stateOut.upZ)) {
    return false;
  }
  return !stateOut.hasScreenPoint ||
         ParseCameraPoint(screenPoint, stateOut.screenX, stateOut.screenY);
}

bool ParseMouseReplayQuery(const std::string &queryRaw,
                           MouseReplayQuery &queryOut) {
  std::istringstream fields(queryRaw);
  std::string mousePart;
  std::string cameraText;
  if (!(fields >> queryOut.modelPath >> mousePart)) {
    return false;
  }
  std::getline(fields, cameraText);
  const std::size_t firstNonSpace = cameraText.find_first_not_of(" \t");
  if (firstNonSpace == std::string::npos || mousePart.rfind("mouse:", 0) != 0) {
    return false;
  }

  const std::string coordinates = mousePart.substr(6);
  const std::size_t comma = coordinates.find(',');
  if (comma == std::string::npos ||
      coordinates.find(',', comma + 1) != std::string::npos) {
    return false;
  }

  auto parseCoordinate = [](std::string_view text, Standard_Integer &valueOut) {
    if (text.empty()) {
      return false;
    }
    long value = 0;
    for (const char ch : text) {
      if (ch < '0' || ch > '9') {
        return false;
      }
      value = value * 10 + static_cast<long>(ch - '0');
      if (value > std::numeric_limits<Standard_Integer>::max()) {
        return false;
      }
    }
    valueOut = static_cast<Standard_Integer>(value);
    return true;
  };

  if (!parseCoordinate(std::string_view(coordinates).substr(0, comma),
                       queryOut.mouseX) ||
      !parseCoordinate(std::string_view(coordinates).substr(comma + 1),
                       queryOut.mouseY) ||
      !ParseCameraState(cameraText.substr(firstNonSpace), queryOut.camera)) {
    return false;
  }

  queryOut.modelPath = NormalizePath(queryOut.modelPath);
  return !queryOut.modelPath.empty();
}

bool FindMouseRayHit(const Handle(V3d_View) & view,
                     const Standard_Integer mouseX,
                     const Standard_Integer mouseY, const TopoDS_Shape &shape,
                     gp_Pnt &hitPoint) {
  if (view.IsNull() || shape.IsNull()) {
    return false;
  }

  Standard_Real projectedX = 0.0;
  Standard_Real projectedY = 0.0;
  Standard_Real projectedZ = 0.0;
  Standard_Real rayX = 0.0;
  Standard_Real rayY = 0.0;
  Standard_Real rayZ = 0.0;
  view->ConvertWithProj(mouseX, mouseY, projectedX, projectedY, projectedZ,
                        rayX, rayY, rayZ);

  Standard_Real eyeX = 0.0;
  Standard_Real eyeY = 0.0;
  Standard_Real eyeZ = 0.0;
  view->Eye(eyeX, eyeY, eyeZ);
  const gp_Pnt rayOrigin(eyeX, eyeY, eyeZ);
  const gp_Vec projectedRay(rayOrigin,
                            gp_Pnt(projectedX, projectedY, projectedZ));
  gp_Vec rayDirection(rayX, rayY, rayZ);
  if (rayDirection.SquareMagnitude() <= gp::Resolution()) {
    return false;
  }
  if (rayDirection.Dot(projectedRay) < 0.0) {
    rayDirection.Reverse();
  }

  BRepIntCurveSurface_Inter inter;
  inter.Init(shape, gp_Lin(rayOrigin, gp_Dir(rayDirection)), 1.0e-7);

  Standard_Real closestDistance = std::numeric_limits<Standard_Real>::max();
  bool found = false;
  for (; inter.More(); inter.Next()) {
    const gp_Vec fromEye(rayOrigin, inter.Pnt());
    const Standard_Real distance = fromEye.Dot(rayDirection);
    if (distance >= 0.0 && distance < closestDistance) {
      closestDistance = distance;
      hitPoint = inter.Pnt();
      found = true;
    }
  }
  return found;
}

bool FindVertexByIndex(const TopoDS_Shape &shape,
                       const Standard_Integer vertexIndex,
                       TopoDS_Vertex &vertexOut) {
  if (shape.IsNull() || vertexIndex <= 0) {
    return false;
  }

  TopTools_IndexedMapOfShape vertexMap;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertexMap);
  if (vertexIndex > vertexMap.Extent()) {
    return false;
  }

  vertexOut = TopoDS::Vertex(vertexMap(vertexIndex));
  return !vertexOut.IsNull();
}

Standard_Integer FindVertexIndex(const TopoDS_Shape &shape,
                                 const TopoDS_Vertex &vertex) {
  if (shape.IsNull() || vertex.IsNull()) {
    return 0;
  }

  TopTools_IndexedMapOfShape vertexMap;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertexMap);
  Standard_Integer vertexIndex = vertexMap.FindIndex(vertex);
  if (vertexIndex != 0) {
    return vertexIndex;
  }

  for (Standard_Integer i = 1; i <= vertexMap.Extent(); ++i) {
    if (vertexMap(i).IsSame(vertex)) {
      return i;
    }
  }
  return 0;
}

Standard_Integer FindNearestVertexIndex(const TopoDS_Shape &shape,
                                        const gp_Pnt &point,
                                        const Standard_Real maxDistance) {
  if (shape.IsNull()) {
    return 0;
  }

  TopTools_IndexedMapOfShape vertexMap;
  TopExp::MapShapes(shape, TopAbs_VERTEX, vertexMap);
  Standard_Integer nearestIndex = 0;
  Standard_Real nearestDistance = maxDistance * maxDistance;
  for (Standard_Integer i = 1; i <= vertexMap.Extent(); ++i) {
    const gp_Pnt vertexPoint = BRep_Tool::Pnt(TopoDS::Vertex(vertexMap(i)));
    const Standard_Real distance = vertexPoint.SquareDistance(point);
    if (distance <= nearestDistance) {
      nearestDistance = distance;
      nearestIndex = i;
    }
  }
  return nearestIndex;
}

std::string SerializeCameraState(const Handle(V3d_View) & view) {
  Standard_Real eyeX = 0.0, eyeY = 0.0, eyeZ = 0.0;
  Standard_Real atX = 0.0, atY = 0.0, atZ = 0.0;
  Standard_Real upX = 0.0, upY = 0.0, upZ = 0.0;
  view->Eye(eyeX, eyeY, eyeZ);
  view->At(atX, atY, atZ);
  view->Up(upX, upY, upZ);
  std::ostringstream result;
  result << std::setprecision(17) << eyeX << ',' << eyeY << ',' << eyeZ << ':'
         << atX << ',' << atY << ',' << atZ << ':' << upX << ',' << upY << ','
         << upZ;
  return result.str();
}

std::string SerializeCameraReplay(const Handle(V3d_View) & view,
                                  const Standard_Real screenX,
                                  const Standard_Real screenY) {
  std::ostringstream result;
  result << SerializeCameraState(view) << ':' << std::setprecision(17)
         << screenX << ',' << screenY;
  return result.str();
}

void ApplyCameraState(const Handle(V3d_View) & view, const CameraState &state) {
  view->SetEye(state.eyeX, state.eyeY, state.eyeZ);
  view->SetAt(state.atX, state.atY, state.atZ);
  view->SetUp(state.upX, state.upY, state.upZ);
  view->Redraw();
}
bool LaunchMatchingStepViewers(const char *programName) {
  const std::filesystem::path workingDirectory =
      std::filesystem::current_path();
  const std::string prefix = workingDirectory.filename().string();
  std::vector<std::filesystem::path> stepFiles;
  std::error_code directoryError;
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(workingDirectory, directoryError)) {
    if (directoryError) {
      break;
    }
    if (!entry.is_regular_file(directoryError) || directoryError) {
      directoryError.clear();
      continue;
    }

    const std::string filename = entry.path().filename().string();
    if (filename.compare(0, prefix.size(), prefix) == 0 &&
        entry.path().extension() == ".step") {
      stepFiles.push_back(entry.path().lexically_normal());
    }
  }

  if (directoryError) {
    std::cerr << "Error: could not enumerate " << workingDirectory << ": "
              << directoryError.message() << std::endl;
    return false;
  }

  std::sort(stepFiles.begin(), stepFiles.end());
  if (stepFiles.empty()) {
    std::cerr << "Error: no STEP files matching " << prefix << "*.step in "
              << workingDirectory << std::endl;
    return false;
  }

#ifdef _WIN32
  std::cerr << "Error: opening multiple STEP viewers without arguments is "
               "not supported on Windows."
            << std::endl;
  return false;
#else
  for (const std::filesystem::path &stepFile : stepFiles) {
    const pid_t child = fork();
    if (child < 0) {
      std::cerr << "Error: could not launch viewer for " << stepFile << ": "
                << std::strerror(errno) << std::endl;
      return false;
    }
    if (child == 0) {
      char *childArguments[] = {
          const_cast<char *>(programName),
          const_cast<char *>(stepFile.c_str()),
          nullptr,
      };
      execvp(programName, childArguments);
      std::cerr << "Error: could not open " << stepFile << ": "
                << std::strerror(errno) << std::endl;
      _exit(127);
    }
    std::cout << "Opening " << stepFile << " (reloads independently)."
              << std::endl;
  }
  return true;
#endif
}

// After the prefix, a root pair uses: <expression>_<expression>.
// An expression is RRGGBB or a parenthesized <expression>_<expression>.
constexpr char kCheckerboardLayerPrefix[] = "CHECKERBOARD_";

struct CheckerboardNode {
  std::array<Standard_Byte, 3> color{};
  Standard_Integer left = -1;
  Standard_Integer right = -1;
  Standard_Integer depth = 0;

  bool IsLeaf() const { return left < 0; }
};

struct CheckerboardTree {
  std::vector<CheckerboardNode> nodes;
  Standard_Integer root = -1;
  Standard_Integer maxNestedDepth = 0;
};

bool ParseCheckerboardColor(const TCollection_ExtendedString &layerName,
                            Standard_Integer firstPosition,
                            std::array<Standard_Byte, 3> &colorOut) {
  if (firstPosition < 1 || firstPosition + 5 > layerName.Length()) {
    return false;
  }
  Standard_Integer rgb = 0;
  for (Standard_Integer i = 0; i < 6; ++i) {
    const Standard_ExtCharacter character = layerName.Value(firstPosition + i);
    Standard_Integer digit = -1;
    if (character >= '0' && character <= '9') {
      digit = character - '0';
    } else if (character >= 'A' && character <= 'F') {
      digit = character - 'A' + 10;
    } else if (character >= 'a' && character <= 'f') {
      digit = character - 'a' + 10;
    }
    if (digit < 0) {
      return false;
    }
    rgb = (rgb << 4) | digit;
  }
  colorOut = {static_cast<Standard_Byte>((rgb >> 16) & 0xff),
              static_cast<Standard_Byte>((rgb >> 8) & 0xff),
              static_cast<Standard_Byte>(rgb & 0xff)};
  return true;
}

bool ParseCheckerboardExpression(const TCollection_ExtendedString &layerName,
                                 Standard_Integer &position,
                                 std::vector<CheckerboardNode> &nodes,
                                 Standard_Integer &nodeIndex) {
  if (position > layerName.Length()) {
    return false;
  }

  CheckerboardNode node;
  if (layerName.Value(position) == '(') {
    ++position;
    Standard_Integer left = -1;
    Standard_Integer right = -1;
    if (!ParseCheckerboardExpression(layerName, position, nodes, left) ||
        position > layerName.Length() || layerName.Value(position) != '_') {
      return false;
    }
    ++position;
    if (!ParseCheckerboardExpression(layerName, position, nodes, right) ||
        position > layerName.Length() || layerName.Value(position) != ')') {
      return false;
    }
    ++position;
    node.left = left;
    node.right = right;
    node.depth = 1 + std::max(nodes[left].depth, nodes[right].depth);
  } else {
    if (!ParseCheckerboardColor(layerName, position, node.color)) {
      return false;
    }
    position += 6;
  }

  nodeIndex = static_cast<Standard_Integer>(nodes.size());
  nodes.push_back(node);
  return true;
}

bool ParseCheckerboardLayer(const TCollection_ExtendedString &layerName,
                            CheckerboardTree &treeOut) {
  constexpr Standard_Integer kPrefixLength =
      sizeof(kCheckerboardLayerPrefix) - 1;
  const Standard_Integer nameLength = layerName.Length();
  if (nameLength < kPrefixLength + 13) {
    return false;
  }
  for (Standard_Integer i = 0; i < kPrefixLength; ++i) {
    Standard_ExtCharacter character = layerName.Value(i + 1);
    if (character >= 'a' && character <= 'z') {
      character = character - 'a' + 'A';
    }
    if (character != kCheckerboardLayerPrefix[i]) {
      return false;
    }
  }

  std::vector<CheckerboardNode> parsedNodes;
  Standard_Integer position = kPrefixLength + 1;
  Standard_Integer left = -1;
  Standard_Integer right = -1;
  if (!ParseCheckerboardExpression(layerName, position, parsedNodes, left) ||
      position > nameLength || layerName.Value(position) != '_') {
    return false;
  }
  ++position;
  if (!ParseCheckerboardExpression(layerName, position, parsedNodes, right) ||
      position != nameLength + 1) {
    return false;
  }

  CheckerboardNode root;
  root.left = left;
  root.right = right;
  root.depth = 1 + std::max(parsedNodes[left].depth, parsedNodes[right].depth);
  treeOut.root = static_cast<Standard_Integer>(parsedNodes.size());
  treeOut.maxNestedDepth = root.depth - 1;
  parsedNodes.push_back(root);
  treeOut.nodes.swap(parsedNodes);
  return true;
}

Handle(AIS_TexturedShape)
MakeCheckerboardPresentation(const TopoDS_Face &face,
                             const CheckerboardTree &tree) {
  const Handle(Geom_Plane) plane =
      Handle(Geom_Plane)::DownCast(BRep_Tool::Surface(face));
  if (plane.IsNull()) {
    return {};
  }

  Standard_Real uMin = 0.0;
  Standard_Real uMax = 0.0;
  Standard_Real vMin = 0.0;
  Standard_Real vMax = 0.0;
  BRepTools::UVBounds(face, uMin, uMax, vMin, vMax);
  const Standard_Real uWidth = std::abs(uMax - uMin);
  const Standard_Real vWidth = std::abs(vMax - vMin);

  GProp_GProps surfaceProperties;
  BRepGProp::SurfaceProperties(face, surfaceProperties);
  const Standard_Real area = surfaceProperties.Mass();
  if (uWidth <= Precision::Confusion() || vWidth <= Precision::Confusion() ||
      area <= Precision::SquareConfusion()) {
    return {};
  }

  // Use the actual planar area and UV extents to keep texture cells square.
  // The plane's U/V axes rotate the pattern with the face instead of world XYZ.
  const Standard_Real targetCellWidth = std::sqrt(area) / 4.0;
  constexpr Standard_Integer kMaxCellsPerSide = 64;
  const auto cellCount = [targetCellWidth, kMaxCellsPerSide](Standard_Real extent) {
    return static_cast<Standard_Integer>(std::clamp(
        std::lround(extent / targetCellWidth), 1L,
        static_cast<long>(kMaxCellsPerSide)));
  };
  const Standard_Integer cellsU = cellCount(uWidth);
  const Standard_Integer cellsV = cellCount(vWidth);
  constexpr Standard_Integer kMaxTextureDimension = 1024;
  constexpr Standard_Integer kMinPixelsPerCell = 4;
  constexpr Standard_Integer kMaxPixelsPerCell = 64;
  const Standard_Integer maxBaseCells = std::max(cellsU, cellsV);
  const Standard_Integer maxNestedScale =
      kMaxTextureDimension / (maxBaseCells * kMinPixelsPerCell);
  Standard_Integer nestedScale = 1;
  for (Standard_Integer nestedDepth = 0;
       nestedDepth < tree.maxNestedDepth; ++nestedDepth) {
    if (nestedScale > maxNestedScale / 4) {
      break;
    }
    nestedScale *= 4;
  }
  const Standard_Integer pixelsPerCell = std::min(
      kMaxPixelsPerCell,
      kMaxTextureDimension / (maxBaseCells * nestedScale));
  const Standard_Integer rootCellPixels = nestedScale * pixelsPerCell;
  const Standard_Integer textureWidth = cellsU * rootCellPixels;
  const Standard_Integer textureHeight = cellsV * rootCellPixels;

  Handle(Image_PixMap) checkerTexture = new Image_PixMap();
  if (!checkerTexture->InitZero(Image_Format_RGBA, textureWidth,
                                textureHeight)) {
    return {};
  }
  // Raster dimensions cap detail; each texel still walks the tree to a leaf.
  const CheckerboardNode &root = tree.nodes[tree.root];
  for (Standard_Integer y = 0; y < textureHeight; ++y) {
    Standard_Byte *row = checkerTexture->ChangeRow(y);
    for (Standard_Integer x = 0; x < textureWidth; ++x) {
      const Standard_Integer rootU = x / rootCellPixels;
      const Standard_Integer rootV = y / rootCellPixels;
      Standard_Integer selectedIndex =
          (rootU + rootV) % 2 == 0 ? root.left : root.right;
      Standard_Integer localX = x % rootCellPixels;
      Standard_Integer localY = y % rootCellPixels;
      Standard_Real u =
          (static_cast<Standard_Real>(localX) + 0.5) / rootCellPixels;
      Standard_Real v =
          (static_cast<Standard_Real>(localY) + 0.5) / rootCellPixels;
      while (!tree.nodes[selectedIndex].IsLeaf()) {
        const Standard_Integer cellU =
            std::min(3, static_cast<Standard_Integer>(u * 4.0));
        const Standard_Integer cellV =
            std::min(3, static_cast<Standard_Integer>(v * 4.0));
        const CheckerboardNode &node = tree.nodes[selectedIndex];
        selectedIndex = (cellU + cellV) % 2 == 0 ? node.left : node.right;
        u = u * 4.0 - cellU;
        v = v * 4.0 - cellV;
      }

      const std::array<Standard_Byte, 3> &color =
          tree.nodes[selectedIndex].color;
      Standard_Byte *pixel = row + 4 * x;
      pixel[0] = color[0];
      pixel[1] = color[1];
      pixel[2] = color[2];
      pixel[3] = 255;
    }
  }

  gp_Dir normal = plane->Pln().Axis().Direction();
  if (face.Orientation() == TopAbs_REVERSED) {
    normal.Reverse();
  }
  gp_Vec separation(normal);
  separation *= std::max(uWidth, vWidth) * 1.0e-5;
  gp_Trsf lift;
  lift.SetTranslation(separation);
  BRepBuilderAPI_Transform liftedFace(face, lift, Standard_True);

  Handle(AIS_TexturedShape) presentation =
      new AIS_TexturedShape(liftedFace.Shape());
  presentation->SetTexturePixMap(checkerTexture);
  presentation->SetTextureRepeat(Standard_False);
  presentation->SetTextureMapOn();
  presentation->DisableTextureModulate();
  return presentation;
}
} // namespace

int main(int argc, char *argv[]) {
  bool wanthelp = (argc >= 2 && (strcmp(argv[1], "--help") == 0 ||
                                 strcmp(argv[1], "-h") == 0));
  if (wanthelp) {
    std::cout << "Usage: " << argv[0]
              << " <path_to_step_file.stp> [source:line:col] "
                 "[camera_position:[camera_target:[camera_up]]]"
              << std::endl;
    std::cout << "       " << argv[0] << " <source:line:col>" << std::endl;
    std::cout << "       " << argv[0]
              << "                 (open <directory-name>*.step)" << std::endl;
    std::cout
        << "\tLeft click to jump with nvim-remote.sh\n"
           "\tShift-left click to query a vertex, otherwise face color\n"
           "\tMiddle click to pan\n\tRight click to rotate\n"
           "\tSpace to print camera state\n\tH to cycle on-screen help modes\n"
           "\tESC to quit."
        << std::endl;
    return 0;
  }
  if (argc == 1) {
    return LaunchMatchingStepViewers(argv[0]) ? 0 : 1;
  }
  if (argc > 4) {
    std::cout << "Usage: " << argv[0]
              << " <path_to_step_file.stp> [source:line:col] "
                 "[camera_position:[camera_target:[camera_up]]]"
              << std::endl;
    return 1;
  }

#ifndef _WIN32
  const std::filesystem::path socketPath =
      std::filesystem::current_path() / ".OCCT_XCAF_FacePicker.sock";
#endif

  CameraState cameraState;
  bool hasCameraState = false;
  std::string cliForwardQuery;
  std::string cliModelPath;
  if (argc == 4) {
    if (!ParseCameraState(argv[3], cameraState)) {
      std::cerr << "Error: invalid camera state; expected "
                   "x,y,z:x,y,z:x,y,z"
                << std::endl;
      return 1;
    }
    hasCameraState = true;
    SourceQuery parsedQuery;
    std::string canonicalSource;
    std::string parseError;
    if (!ParseForwardQuery(argv[2], parsedQuery, canonicalSource, parseError)) {
      std::cerr << "Error: invalid forward-navigation query: " << parseError
                << std::endl;
      return 1;
    }
    cliForwardQuery = canonicalSource;
  } else if (argc == 3 && ParseCameraState(argv[2], cameraState)) {
    hasCameraState = true;
  } else if (argc == 2 || argc == 3) {
    const char *queryArgument = argc == 2 ? argv[1] : argv[2];
    VertexReplayQuery vertexQuery;
    MouseReplayQuery mouseQuery;
    if (ParseMouseReplayQuery(queryArgument, mouseQuery)) {
      cliForwardQuery = queryArgument;
      cliModelPath = mouseQuery.modelPath;
    } else if (ParseVertexReplayQuery(queryArgument, vertexQuery)) {
      cliForwardQuery = queryArgument;
      cliModelPath = vertexQuery.modelPath;
    } else {
      SourceQuery parsedQuery;
      std::string canonicalSource;
      std::string parseError;
      if (ParseForwardQuery(queryArgument, parsedQuery, canonicalSource,
                            parseError)) {
        cliForwardQuery = canonicalSource;
      } else if (argc == 3) {
        std::cerr << "Error: invalid camera state or forward-navigation query: "
                  << parseError << std::endl;
        return 1;
      }
    }
  }

  if (!cliForwardQuery.empty()) {
#ifdef _WIN32
    std::cerr << "Error: forward navigation IPC is not supported on Windows."
              << std::endl;
    return 1;
#else
    std::string sendError;
    int sendErrno = 0;
    if (SendForwardNavigationRequest(socketPath, cliForwardQuery, sendError,
                                     &sendErrno)) {
      return 0;
    }

    const bool socketUnavailable =
        (sendErrno == ENOENT || sendErrno == ECONNREFUSED);
    if (!cliModelPath.empty() && socketUnavailable) {
      std::cerr << "Forward navigation socket unavailable; opening viewer for "
                << cliModelPath << " and applying query locally." << std::endl;
    } else if (argc >= 3 && socketUnavailable) {
      std::cerr << "Forward navigation socket unavailable; opening viewer for "
                << argv[1] << " and applying query locally." << std::endl;
    } else {
      std::cerr << "Error: could not send forward-navigation query to "
                << socketPath << ": " << sendError << std::endl;
      return 1;
    }
#endif
  }

  const std::filesystem::path stepPathInput(
      cliModelPath.empty() ? argv[1] : cliModelPath);
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

  Handle(AIS_LightSource) sceneLightSource;
  for (V3d_ListOfLightIterator lightIt(viewer->ActiveLights()); lightIt.More();
       lightIt.Next()) {
    const Handle(V3d_Light) &activeLight = lightIt.Value();
    if (activeLight.IsNull()) {
      continue;
    }

    sceneLightSource = new AIS_LightSource(activeLight);
    sceneLightSource->SetDisplayName(Standard_True);
    // context->Display(sceneLightSource, Standard_False);
    break;
  }

  auto configurePbrMaterial = [](const Handle(Graphic3d_AspectFillArea3d) &aspect) {
    if (aspect.IsNull()) {
      return;
    }

    auto configureMaterial = [](Graphic3d_MaterialAspect &material) {
      material.SetSpecularColor(
          Quantity_Color(0.33, 0.33, 0.33, Quantity_TOC_RGB));
      Graphic3d_PBRMaterial pbrMaterial = material.PBRMaterial();
      pbrMaterial.SetRoughness(0.2f);
      material.SetPBRMaterial(pbrMaterial);
    };
    configureMaterial(aspect->ChangeFrontMaterial());
    configureMaterial(aspect->ChangeBackMaterial());
  };

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
  configurePbrMaterial(defaultShadingAspect->Aspect());

  const Standard_Real kModelTransparency = 0.0; // 0.0 = opaque, 1.0 = invisible
  const Standard_Boolean kApplyTintColor = Standard_False;
  const Quantity_Color kTintColor(0.80, 0.88, 1.00, Quantity_TOC_RGB);

  Handle(TDocStd_Document) doc;
  Handle(XCAFDoc_ShapeTool) shapeTool;
  Handle(XCAFDoc_ColorTool) colorTool;
  TDF_Label rootLabel;
  Handle(XCAFPrs_AISObject) xcafPresentation;
  std::unordered_map<std::string, std::string> labelSourceByEntry;
  ForwardNavigationData forwardNavigationData;
  Handle(AIS_Shape) forwardNavHighlight;
  std::vector<Handle(AIS_TexturedShape)> checkerboardPresentations;

#ifndef _WIN32
  int forwardNavSocketFd = -1;
#endif

  auto applyPresentationStyling = [&](const Handle(XCAFPrs_AISObject) &
                                      presentation) {
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
    configurePbrMaterial(fillAspect);
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
    BuildForwardNavigationData(labelSourceByEntry, forwardNavigationData);

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
    Handle(XCAFDoc_LayerTool) newLayerTool =
        XCAFDoc_DocumentTool::LayerTool(newDoc->Main());

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
    std::vector<Handle(AIS_TexturedShape)> newCheckerboardPresentations;
    TopoDS_Shape newRootShape;
    if (newShapeTool->GetShape(newRootLabel, newRootShape)) {
      for (TopExp_Explorer faceIt(newRootShape, TopAbs_FACE); faceIt.More();
           faceIt.Next()) {
        const TopoDS_Face face = TopoDS::Face(faceIt.Current());
        TDF_Label faceLabel;
        if (!newShapeTool->FindSubShape(newRootLabel, face, faceLabel)) {
          continue;
        }
        Handle(TColStd_HSequenceOfExtendedString) faceLayers =
            newLayerTool->GetLayers(faceLabel);
        CheckerboardTree tree;
        bool hasCheckerboardColors = false;
        if (!faceLayers.IsNull()) {
          for (Standard_Integer i = faceLayers->Lower();
               i <= faceLayers->Upper(); ++i) {
            if (ParseCheckerboardLayer(faceLayers->Value(i), tree)) {
              hasCheckerboardColors = true;
              break;
            }
          }
        }
        if (!hasCheckerboardColors) {
          continue;
        }

        Handle(AIS_TexturedShape) checkerboard =
            MakeCheckerboardPresentation(face, tree);
        if (checkerboard.IsNull()) {
          std::cerr << "Warning: could not render marked checkerboard face."
                    << std::endl;
          continue;
        }
        newCheckerboardPresentations.push_back(checkerboard);
      }
    }

    Handle(XCAFPrs_AISObject) newPresentation =
        new XCAFPrs_AISObject(newRootLabel);
    applyPresentationStyling(newPresentation);

    Handle(TDocStd_Document) oldDoc = doc;
    Handle(XCAFPrs_AISObject) oldPresentation = xcafPresentation;
    std::vector<Handle(AIS_TexturedShape)> oldCheckerboards;
    oldCheckerboards.swap(checkerboardPresentations);
    checkerboardPresentations.swap(newCheckerboardPresentations);

    doc = newDoc;
    shapeTool = newShapeTool;
    colorTool = newColorTool;
    rootLabel = newRootLabel;
    xcafPresentation = newPresentation;

    if (!oldPresentation.IsNull()) {
      context->Remove(oldPresentation, Standard_False);
    }
    for (const Handle(AIS_TexturedShape) &oldCheckerboard : oldCheckerboards) {
      context->Remove(oldCheckerboard, Standard_False);
    }

    context->Display(xcafPresentation, AIS_Shaded, 0, Standard_True);
    context->SetSelectionModeActive(xcafPresentation, 0, Standard_False);
    context->SetSelectionModeActive(xcafPresentation, 4, Standard_True);
    context->SetTransparency(xcafPresentation, kModelTransparency,
                             Standard_False);
    if (kApplyTintColor) {
      context->SetColor(xcafPresentation, kTintColor, Standard_False);
    }
    for (const Handle(AIS_TexturedShape) &checkerboard :
         checkerboardPresentations) {
      context->Display(checkerboard, 3, 0, Standard_False);
      context->Deactivate(checkerboard);
    }

    view->FitAll();
    view->ZFitAll();
    if (!forwardNavHighlight.IsNull()) {
      context->Remove(forwardNavHighlight, Standard_False);
      forwardNavHighlight.Nullify();
    }
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
  enum class HelpOverlayMode { KeysOnly, Full, Collapsed };
  HelpOverlayMode helpOverlayMode = HelpOverlayMode::Full;
  const Quantity_Color helpTextColor(0.74, 0.74, 0.74, Quantity_TOC_RGB);
  const Quantity_Color activeHelpTextColor(1.0, 1.0, 1.0, Quantity_TOC_RGB);
  auto createHelpLabel = [&](const char *theText, const Standard_Integer theX,
                             const Standard_Integer theY) {
    Handle(AIS_TextLabel) label = new AIS_TextLabel();
    label->SetText(TCollection_ExtendedString(theText));
    label->SetPosition(gp_Pnt(0.0, 0.0, 0.0));
    label->SetOwnAnchorPoint(Standard_True);
    label->SetHJustification(Graphic3d_HTA_LEFT);
    label->SetVJustification(Graphic3d_VTA_TOPFIRSTLINE);
    label->SetHeight(16.0);
    label->SetColor(helpTextColor);
    label->SetZLayer(Graphic3d_ZLayerId_TopOSD);
    label->SetTransformPersistence(new Graphic3d_TransformPers(
        Graphic3d_TMF_2d, Aspect_TOTP_LEFT_UPPER, Graphic3d_Vec2i(theX, theY)));
    context->Display(label, Standard_False);
    return label;
  };

  const std::vector<const char *> helpKeys = {
      "Left click", "Shift+LMB", "Middle drag", "Right drag",
      "Space",      "H",         "Esc"};
  const std::vector<const char *> helpDescriptions = {
      "jump to source",
      "vertex query; face color fallback",
      "pan",
      "rotate",
      "print camera state",
      "cycle help modes",
      "quit"};
  std::vector<Handle(AIS_TextLabel)> helpLabels1;
  std::vector<Handle(AIS_TextLabel)> helpLabels2;
  for (std::size_t i = 0; i < helpKeys.size(); ++i) {
    const Standard_Integer y = 14 + static_cast<Standard_Integer>(i) * 22;
    helpLabels1.push_back(createHelpLabel(helpKeys[i], 14, y));
    helpLabels2.push_back(createHelpLabel(helpDescriptions[i], 100, y));
  }
  const Handle(AIS_TextLabel) modifierHelpLabel = helpLabels1[1];
  const Handle(AIS_TextLabel) collapsedHelpLabel =
      createHelpLabel("h(elp)", 14, 14);
  context->Erase(collapsedHelpLabel, Standard_False);
  context->UpdateCurrentViewer();

  std::atomic_bool reloadRequested(false);
  ModelWatchContext watchContext{&reloadRequested, watchedStepFilePath};
  MouseScrollContext scrollContext;
  glfwSetWindowUserPointer(occtWindow->getGlfwWindow(), &scrollContext);
  glfwSetScrollCallback(occtWindow->getGlfwWindow(), OnMouseScroll);
  glfwSetWindowRefreshCallback(occtWindow->getGlfwWindow(), OnWindowRefresh);

#ifndef _WIN32
  {
    std::error_code removeEc;
    std::filesystem::remove(socketPath, removeEc);

    const std::string socketPathString = socketPath.string();
    if (socketPathString.size() >= sizeof(sockaddr_un::sun_path)) {
      std::cerr << "Warning: forward-nav socket path is too long: "
                << socketPath << std::endl;
    } else {
      forwardNavSocketFd = socket(AF_UNIX, SOCK_DGRAM, 0);
      if (forwardNavSocketFd < 0) {
        std::cerr << "Warning: could not create forward-nav socket "
                  << socketPath << ": " << std::strerror(errno) << std::endl;
      } else {
        const int flags = fcntl(forwardNavSocketFd, F_GETFL, 0);
        if (flags >= 0) {
          (void)fcntl(forwardNavSocketFd, F_SETFL, flags | O_NONBLOCK);
        }

        const SockaddrUn addr = MakeSockaddrUn(socketPath);
        if (bind(forwardNavSocketFd,
                 reinterpret_cast<const sockaddr *>(&addr.addr),
                 addr.len) != 0) {
          std::cerr << "Warning: could not bind forward-nav socket "
                    << socketPath << ": " << std::strerror(errno) << std::endl;
          close(forwardNavSocketFd);
          forwardNavSocketFd = -1;
        }
      }
    }
  }
#endif

  dmon_init();
  dmon_watch_id watchId = dmon_watch(watchRootForDmon.c_str(),
                                     OnModelFileChanged, 0, &watchContext);
  if (watchId.id == 0) {
    std::cerr << "Warning: dmon could not watch directory: " << watchRootForDmon
              << std::endl;
  }

  bool wasLeftPressed = false;
  bool wasRightPressed = false;
  bool wasMiddlePressed = false;
  bool wasShiftPressed = false;
  bool wasSpacePressed = false;
  bool wasHelpTogglePressed = false;

  double rotateStartX = 0.0;
  double rotateStartY = 0.0;
  double panStartX = 0.0;
  double panStartY = 0.0;

  int lastFbWidth = 0;
  int lastFbHeight = 0;
  glfwGetFramebufferSize(occtWindow->getGlfwWindow(), &lastFbWidth,
                         &lastFbHeight);

  auto getRootShape = [&](TopoDS_Shape &rootShapeOut) {
    return !shapeTool.IsNull() &&
           shapeTool->GetShape(rootLabel, rootShapeOut) &&
           !rootShapeOut.IsNull();
  };

  auto vertexEntry = [&](const Standard_Integer vertexIndex) {
    TCollection_AsciiString rootEntry;
    TDF_Tool::Entry(rootLabel, rootEntry);
    return std::string(rootEntry.ToCString()) + ":v" +
           std::to_string(vertexIndex);
  };

  auto printVertexReplay = [&](const TopoDS_Shape &rootShape,
                               const Standard_Integer vertexIndex,
                               const gp_Pnt &stabPoint, const bool hasStab,
                               const bool includeEntry) {
    TopoDS_Vertex vertex;
    if (!FindVertexByIndex(rootShape, vertexIndex, vertex)) {
      return false;
    }

    const gp_Pnt vertexPoint = BRep_Tool::Pnt(vertex);
    if (includeEntry) {
      const std::string modelEntry =
          std::filesystem::path(stepPathForOcct).filename().string() + ":" +
          vertexEntry(vertexIndex);
      std::cout << modelEntry << ' ';
    }
    std::cout << "snap:" << vertexPoint.X() << ',' << vertexPoint.Y() << ','
              << vertexPoint.Z();
    if (hasStab) {
      std::cout << " stab:" << std::setprecision(17) << stabPoint.X() << ','
                << stabPoint.Y() << ',' << stabPoint.Z();
    }
    std::cout << std::endl;
    return true;
  };

  auto applyVertexReplayQuery = [&](const std::string &rawQueryLine) {
    VertexReplayQuery query;
    TopoDS_Shape rootShape;
    if (!ParseVertexReplayQuery(rawQueryLine, query) ||
        !getRootShape(rootShape) ||
        !printVertexReplay(rootShape, query.vertexIndex, gp_Pnt(), false,
                           false)) {
      std::cerr << "Vertex replay ignored (query='" << rawQueryLine << "')"
                << std::endl;
    }
  };

  auto applyMouseReplayQuery = [&](const std::string &rawQueryLine) {
    MouseReplayQuery query;
    TopoDS_Shape rootShape;
    if (!ParseMouseReplayQuery(rawQueryLine, query) ||
        !getRootShape(rootShape)) {
      return false;
    }

    ApplyCameraState(view, query.camera);
    context->SetSelectionModeActive(xcafPresentation, 4, Standard_False);
    context->SetSelectionModeActive(xcafPresentation, 1, Standard_True);
    context->UpdateCurrentViewer();
    context->MoveTo(query.mouseX, query.mouseY, view, Standard_True);
    context->SelectDetected();

    Standard_Integer vertexIndex = 0;
    for (context->InitSelected(); context->MoreSelected();
         context->NextSelected()) {
      Handle(StdSelect_BRepOwner) owner =
          Handle(StdSelect_BRepOwner)::DownCast(context->SelectedOwner());
      if (!owner.IsNull() && owner->Shape().ShapeType() == TopAbs_VERTEX) {
        vertexIndex =
            FindVertexIndex(rootShape, TopoDS::Vertex(owner->Shape()));
        if (vertexIndex != 0) {
          break;
        }
      }
    }

    gp_Pnt stabPoint;
    if (!FindMouseRayHit(view, query.mouseX, query.mouseY, rootShape,
                         stabPoint)) {
      std::cerr << "Mouse vertex replay ignored (query='" << rawQueryLine
                << "')" << std::endl;
      return false;
    }
    if (vertexIndex == 0) {
      vertexIndex = FindNearestVertexIndex(rootShape, stabPoint, 1.0e-6);
    }
    if (vertexIndex == 0 ||
        !printVertexReplay(rootShape, vertexIndex, stabPoint, true, true)) {
      std::cerr << "Mouse vertex replay ignored (query='" << rawQueryLine
                << "')" << std::endl;
      return false;
    }
    return true;
  };

  auto applyForwardNavigationQuery = [&](const std::string &rawQueryLine) {
    SourceQuery query;
    std::string canonicalQuerySource;
    std::string parseError;
    if (!ParseForwardQuery(rawQueryLine, query, canonicalQuerySource,
                           parseError)) {
      std::cerr << "Forward navigation ignored: " << parseError << " (query='"
                << rawQueryLine << "')" << std::endl;
      return;
    }

    std::string resolvedSource;
    std::vector<std::string> resolvedEntries;
    if (!ResolveForwardNavigationQuery(forwardNavigationData, query,
                                       resolvedSource, resolvedEntries)) {
      std::cerr << "Forward navigation: no mapped face for "
                << canonicalQuerySource << std::endl;
      HighlightFacesForEntries({}, doc, shapeTool, context, view,
                               forwardNavHighlight);
      return;
    }

    HighlightFacesForEntries(resolvedEntries, doc, shapeTool, context, view,
                             forwardNavHighlight);
    if (!forwardNavHighlight.IsNull()) {
      view->FitAll();
      view->ZFitAll();
      context->UpdateCurrentViewer();
    }

    std::cout << "Forward navigation: " << canonicalQuerySource << " -> "
              << resolvedSource;
    if (!resolvedEntries.empty()) {
      std::cout << " [";
      for (std::size_t i = 0; i < resolvedEntries.size(); ++i) {
        if (i > 0) {
          std::cout << ",";
        }
        std::cout << resolvedEntries[i];
      }
      std::cout << "]";
    }
    std::cout << std::endl;
  };
  auto applyQuery = [&](const std::string &rawQueryLine) {
    MouseReplayQuery mouseQuery;
    if (ParseMouseReplayQuery(rawQueryLine, mouseQuery)) {
      (void)applyMouseReplayQuery(rawQueryLine);
      return;
    }

    VertexReplayQuery vertexQuery;
    if (ParseVertexReplayQuery(rawQueryLine, vertexQuery)) {
      applyVertexReplayQuery(rawQueryLine);
      return;
    }

    applyForwardNavigationQuery(rawQueryLine);
  };

  if (!cliForwardQuery.empty()) {
    applyQuery(cliForwardQuery);
    cliForwardQuery = "";
  }
  if (hasCameraState) {
    ApplyCameraState(view, cameraState);
    hasCameraState = false;
  }

  // 3. Event/render loop
  while (!glfwWindowShouldClose(occtWindow->getGlfwWindow())) {
    glfwWaitEventsTimeout(0.1);

    const bool isSpacePressed =
        glfwGetKey(occtWindow->getGlfwWindow(), GLFW_KEY_SPACE) == GLFW_PRESS;
    if (isSpacePressed && !wasSpacePressed) {
      std::cout << SerializeCameraState(view) << std::endl;
    }

    const bool isHelpTogglePressed =
        glfwGetKey(occtWindow->getGlfwWindow(), GLFW_KEY_H) == GLFW_PRESS;
    if (isHelpTogglePressed && !wasHelpTogglePressed) {
      if (helpOverlayMode == HelpOverlayMode::Full) {
        helpOverlayMode = HelpOverlayMode::KeysOnly;
      } else if (helpOverlayMode == HelpOverlayMode::KeysOnly) {
        helpOverlayMode = HelpOverlayMode::Collapsed;
      } else {
        helpOverlayMode = HelpOverlayMode::Full;
      }

      const bool showKeys = helpOverlayMode != HelpOverlayMode::Collapsed;
      const bool showDescriptions = helpOverlayMode == HelpOverlayMode::Full;
      for (const Handle(AIS_TextLabel) & label : helpLabels1) {
        if (showKeys) {
          context->Display(label, Standard_False);
        } else {
          context->Erase(label, Standard_False);
        }
      }
      for (const Handle(AIS_TextLabel) & label : helpLabels2) {
        if (showDescriptions) {
          context->Display(label, Standard_False);
        } else {
          context->Erase(label, Standard_False);
        }
      }
      if (helpOverlayMode == HelpOverlayMode::Collapsed) {
        context->Display(collapsedHelpLabel, Standard_False);
      } else {
        context->Erase(collapsedHelpLabel, Standard_False);
      }
      context->UpdateCurrentViewer();
      dirty = true;
    }
    wasHelpTogglePressed = isHelpTogglePressed;

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

#ifndef _WIN32
    if (forwardNavSocketFd >= 0) {
      std::string receivedLine;
      while (ReadSingleDatagramLine(forwardNavSocketFd, receivedLine)) {
        applyQuery(receivedLine);
      }
    }
#endif

    glfwMakeContextCurrent(occtWindow->getGlfwWindow());

    int fbWidth = 0;
    int fbHeight = 0;
    glfwGetFramebufferSize(occtWindow->getGlfwWindow(), &fbWidth, &fbHeight);
    if (fbWidth != lastFbWidth || fbHeight != lastFbHeight) {
      occtWindow->DoResize();
      view->MustBeResized();
      lastFbWidth = fbWidth;
      lastFbHeight = fbHeight;
      dirty = true;
    }

    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(occtWindow->getGlfwWindow(), &mouseX, &mouseY);

    if (scrollContext.deltaY != 0.0) {
      const Standard_Real zoomFactor =
          (Standard_Real)std::pow(1.12, scrollContext.deltaY);
      view->SetZoom(zoomFactor, Standard_True);
      scrollContext.deltaY = 0.0;
      dirty = true;
    }

    const bool isShiftPressed = glfwGetKey(occtWindow->getGlfwWindow(),
                                           GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ||
                                glfwGetKey(occtWindow->getGlfwWindow(),
                                           GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
    if (isShiftPressed != wasShiftPressed) {
      context->SetSelectionModeActive(xcafPresentation, 4, !isShiftPressed);
      context->SetSelectionModeActive(xcafPresentation, 1, isShiftPressed);
      context->UpdateCurrentViewer();
      modifierHelpLabel->SetColor(isShiftPressed ? activeHelpTextColor
                                                 : helpTextColor);
      context->Redisplay(modifierHelpLabel, Standard_False);
      dirty = true;
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
      dirty = true;
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
      dirty = true;
    }

    // Modifier-left-click prefers a vertex, then falls back to face color.
    if (isLeftPressed && !wasLeftPressed && !isRightPressed &&
        !isMiddlePressed) {
      context->SelectDetected();

      const int selectionPassCount = isShiftPressed ? 2 : 1;
      bool handledVertex = false;
      for (int selectionPass = 0; selectionPass < selectionPassCount;
           ++selectionPass) {
        const bool vertexPass = isShiftPressed && selectionPass == 0;
        if (selectionPass > 0) {
          context->SetSelectionModeActive(xcafPresentation, 1, Standard_False);
          context->SetSelectionModeActive(xcafPresentation, 4, Standard_True);
          context->UpdateCurrentViewer();
          context->MoveTo((Standard_Integer)mouseX, (Standard_Integer)mouseY,
                          view, Standard_True);
          context->SelectDetected();
        }
        for (context->InitSelected(); context->MoreSelected();
             context->NextSelected()) {
          Handle(SelectMgr_EntityOwner) owner = context->SelectedOwner();
          Handle(StdSelect_BRepOwner) brepOwner =
              Handle(StdSelect_BRepOwner)::DownCast(owner);

          if (brepOwner.IsNull() || shapeTool.IsNull() || colorTool.IsNull()) {
            continue;
          }

          TopoDS_Shape pickedShape = brepOwner->Shape();
          const bool isVertex = pickedShape.ShapeType() == TopAbs_VERTEX;
          if ((vertexPass && !isVertex) || (!vertexPass && isVertex)) {
            continue;
          }

          if (vertexPass) {

            const TopoDS_Vertex pickedVertex = TopoDS::Vertex(pickedShape);
            TopoDS_Shape rootShape;
            if (!shapeTool->GetShape(rootLabel, rootShape) ||
                rootShape.IsNull()) {
              continue;
            }

            TopTools_IndexedMapOfShape vertexMap;
            TopExp::MapShapes(rootShape, TopAbs_VERTEX, vertexMap);
            Standard_Integer vertexIndex = vertexMap.FindIndex(pickedVertex);
            if (vertexIndex == 0) {
              for (Standard_Integer i = 1; i <= vertexMap.Extent(); ++i) {
                if (vertexMap(i).IsSame(pickedVertex)) {
                  vertexIndex = i;
                  break;
                }
              }
            }
            if (vertexIndex == 0) {
              continue;
            }

            TCollection_AsciiString rootEntry;
            TDF_Tool::Entry(rootLabel, rootEntry);
            const std::string pickedEntryStr =
                std::string(rootEntry.ToCString()) + ":v" +
                std::to_string(vertexIndex);
            const gp_Pnt vertexPoint = BRep_Tool::Pnt(pickedVertex);
            gp_Pnt stabPoint;
            if (!FindMouseRayHit(view, (Standard_Integer)mouseX,
                                 (Standard_Integer)mouseY, rootShape,
                                 stabPoint)) {
              continue;
            }
            Standard_Real screenX = 0.0;
            Standard_Real screenY = 0.0;
            view->Project(vertexPoint.X(), vertexPoint.Y(), vertexPoint.Z(),
                          screenX, screenY);
            const std::string modelEntry =
                std::filesystem::path(stepPathForOcct).filename().string() +
                ":" + pickedEntryStr;
            TopTools_IndexedDataMapOfShapeListOfShape vertexFaces;
            TopExp::MapShapesAndAncestors(rootShape, TopAbs_VERTEX, TopAbs_FACE,
                                          vertexFaces);

            std::cout << modelEntry << " snap:" << vertexPoint.X() << ','
                      << vertexPoint.Y() << ',' << vertexPoint.Z()
                      << " stab:" << std::setprecision(17) << stabPoint.X()
                      << ',' << stabPoint.Y() << ',' << stabPoint.Z()
                      << " mouse:" << mouseX << "," << mouseY << ' '
                      << SerializeCameraReplay(view, screenX, screenY) << ' ';
            if (vertexFaces.Contains(pickedVertex)) {
              const TopTools_ListOfShape &incidentFaces =
                  vertexFaces.FindFromKey(pickedVertex);
              TopTools_IndexedMapOfShape seenFaces;
              bool printedFaceColor = false;
              for (TopTools_ListIteratorOfListOfShape faceIt(incidentFaces);
                   faceIt.More(); faceIt.Next()) {
                const TopoDS_Shape &incidentFace = faceIt.Value();
                if (seenFaces.Contains(incidentFace)) {
                  continue;
                }
                seenFaces.Add(incidentFace);

                TDF_Label faceLabel;
                if (!shapeTool->FindSubShape(rootLabel, incidentFace,
                                             faceLabel)) {
                  continue;
                }

                Quantity_Color faceColor;
                if (XCAFDoc_ColorTool::GetColor(faceLabel, XCAFDoc_ColorSurf,
                                                faceColor) ||
                    XCAFDoc_ColorTool::GetColor(faceLabel, XCAFDoc_ColorGen,
                                                faceColor)) {
                  if (!printedFaceColor) {
                    std::printf("[colorQuery|");
                    printedFaceColor = true;
                  }
                  std::printf("%02x%02x%02x", (int)round(255 * faceColor.Red()),
                              (int)round(255 * faceColor.Green()),
                              (int)round(255 * faceColor.Blue()));
                }
              }
              if (printedFaceColor)
                std::printf("|]");
            }
            std::cout << std::endl;
            handledVertex = true;
            break;
          }
          if (pickedShape.ShapeType() != TopAbs_FACE) {
            continue;
          }

          TopoDS_Face pickedFace = TopoDS::Face(pickedShape);
          TDF_Label targetFaceLabel;
          if (!shapeTool->FindSubShape(rootLabel, pickedFace,
                                       targetFaceLabel)) {
            continue;
          }

          TCollection_AsciiString pickedEntry;
          TDF_Tool::Entry(targetFaceLabel, pickedEntry);
          const std::string pickedEntryStr = pickedEntry.ToCString();

          if (isShiftPressed) {
            TopoDS_Shape rootShape;
            Standard_Integer faceIndex = 0;
            if (shapeTool->GetShape(rootLabel, rootShape) &&
                !rootShape.IsNull()) {
              TopTools_IndexedMapOfShape faceMap;
              TopExp::MapShapes(rootShape, TopAbs_FACE, faceMap);
              faceIndex = faceMap.FindIndex(pickedFace);
              if (faceIndex == 0) {
                for (Standard_Integer i = 1; i <= faceMap.Extent(); ++i) {
                  if (faceMap(i).IsSame(pickedFace)) {
                    faceIndex = i;
                    break;
                  }
                }
              }
            }

            Quantity_Color faceColor;
            if (XCAFDoc_ColorTool::GetColor(targetFaceLabel, XCAFDoc_ColorSurf,
                                            faceColor) ||
                XCAFDoc_ColorTool::GetColor(targetFaceLabel, XCAFDoc_ColorGen,
                                            faceColor)) {
              std::printf("#%02x%02x%02x face:%d\n",
                          (int)round(255 * faceColor.Red()),
                          (int)round(255 * faceColor.Green()),
                          (int)round(255 * faceColor.Blue()), faceIndex);
            } else {
              std::cout << pickedEntryStr << " face:" << faceIndex
                        << " color:unset" << std::endl;
            }
            break;
          }

          std::cout << pickedEntryStr;
          const auto sourceIt = labelSourceByEntry.find(pickedEntryStr);
          if (sourceIt != labelSourceByEntry.end()) {
            std::cout << ":" << sourceIt->second;
            if (!LaunchNvimRemote(sourceIt->second)) {
              std::cerr
                  << "Warning: failed to launch nvim-remote.sh for source: "
                  << sourceIt->second << std::endl;
            }
          }
          std::cout << std::endl;
        }
        if (handledVertex) {
          break;
        }
      }
      if (isShiftPressed) {
        context->SetSelectionModeActive(xcafPresentation, 4, Standard_False);
        context->SetSelectionModeActive(xcafPresentation, 1, Standard_True);
        context->UpdateCurrentViewer();
        context->MoveTo((Standard_Integer)mouseX, (Standard_Integer)mouseY,
                        view, Standard_True);
      }
      dirty = true;
    }

    wasLeftPressed = isLeftPressed;
    wasRightPressed = isRightPressed;
    wasMiddlePressed = isMiddlePressed;
    wasSpacePressed = isSpacePressed;
    wasShiftPressed = isShiftPressed;

    if (dirty.exchange(false, std::memory_order_acquire)) {
      view->Redraw();
    }
  }

  if (watchId.id != 0) {
    dmon_unwatch(watchId);
  }
  dmon_deinit();

#ifndef _WIN32
  if (forwardNavSocketFd >= 0) {
    close(forwardNavSocketFd);
    forwardNavSocketFd = -1;
  }
  std::error_code removeEc;
  std::filesystem::remove(socketPath, removeEc);
#endif

  if (!doc.IsNull()) {
    app->Close(doc); // calls glfwTerminate() no need to add it below
  }
  return 0;
}

