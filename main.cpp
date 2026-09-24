#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
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
#include <AIS_DisplayMode.hxx>
#include <AIS_InteractiveContext.hxx>
#include <AIS_LightSource.hxx>
#include <AIS_Shape.hxx>
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
#include <V3d_AmbientLight.hxx>
#include <V3d_DirectionalLight.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <XCAFPrs_AISObject.hxx>

// Modeling & Structural Helpers
#include <BRepIntCurveSurface_Inter.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Quantity_Color.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

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
    std::cout << "\tLeft click to jump with nvim-remote.sh\n"
                 "\tShift-left click to print the vertex query to stdout\n"
                 "\tMiddle click to "
                 "pan\n\tRight click to rotate\n\tESC to quit."
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
    SourceQuery parsedQuery;
    std::string canonicalSource;
    std::string parseError;
    const char *queryArgument = argc == 2 ? argv[1] : argv[2];
    if (ParseForwardQuery(queryArgument, parsedQuery, canonicalSource,
                          parseError)) {
      cliForwardQuery = canonicalSource;
    } else if (argc == 3) {
      std::cerr << "Error: invalid camera state or forward-navigation query: "
                << parseError << std::endl;
      return 1;
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
    if (argc >= 3 && socketUnavailable) {
      std::cerr << "Forward navigation socket unavailable; opening viewer for "
                << argv[1] << " and applying query locally." << std::endl;
    } else {
      std::cerr << "Error: could not send forward-navigation query to "
                << socketPath << ": " << sendError << std::endl;
      return 1;
    }
#endif
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
  ForwardNavigationData forwardNavigationData;
  Handle(AIS_Shape) forwardNavHighlight;

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

  double rotateStartX = 0.0;
  double rotateStartY = 0.0;
  double panStartX = 0.0;
  double panStartY = 0.0;

  int lastFbWidth = 0;
  int lastFbHeight = 0;
  glfwGetFramebufferSize(occtWindow->getGlfwWindow(), &lastFbWidth,
                         &lastFbHeight);

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

  if (!cliForwardQuery.empty()) {
    applyForwardNavigationQuery(cliForwardQuery);
  }
  if (hasCameraState) {
    ApplyCameraState(view, cameraState);
  }

  // 3. Event/render loop
  while (!glfwWindowShouldClose(occtWindow->getGlfwWindow())) {
    glfwPollEvents();

    const bool isSpacePressed =
        glfwGetKey(occtWindow->getGlfwWindow(), GLFW_KEY_SPACE) == GLFW_PRESS;
    if (isSpacePressed && !wasSpacePressed) {
      std::cout << SerializeCameraState(view) << std::endl;
    }

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
        applyForwardNavigationQuery(receivedLine);
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

    // Left click selects a face; Shift-left-click selects a vertex.
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
        if (wasShiftPressed || isShiftPressed) {
          if (pickedShape.ShapeType() != TopAbs_VERTEX) {
            continue;
          }

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
              std::filesystem::path(stepPathForOcct).filename().string() + ":" +
              pickedEntryStr;
          std::cout << modelEntry << " snap:" << vertexPoint.X() << ','
                    << vertexPoint.Y() << ',' << vertexPoint.Z()
                    << " stab:" << std::setprecision(17) << stabPoint.X() << ','
                    << stabPoint.Y() << ',' << stabPoint.Z() << ' '
                    << SerializeCameraReplay(view, screenX, screenY)
                    << std::endl;
          continue;
        }

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

        std::cout << pickedEntryStr;
        const auto sourceIt = labelSourceByEntry.find(pickedEntryStr);
        if (sourceIt != labelSourceByEntry.end()) {
          std::cout << ":" << sourceIt->second;
          if (!LaunchNvimRemote(sourceIt->second)) {
            std::cerr << "Warning: failed to launch nvim-remote.sh for source: "
                      << sourceIt->second << std::endl;
          }
        }
        std::cout << std::endl;
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
