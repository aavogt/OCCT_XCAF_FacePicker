cmake_minimum_required(VERSION 3.10)
project(OCCT_XCAF_FacePicker CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies
find_package(OpenCASCADE REQUIRED)
find_package(glfw3 REQUIRED)

# Define the source files
set(SOURCE_FILES
    main.cpp
    GlfwOcctWindow.cpp
)

add_executable(${PROJECT_NAME} ${SOURCE_FILES})

# Include OCCT headers
target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCASCADE_INCLUDE_DIR})

# Link the required OCCT Toolkits
# Notes:
# - TKBinXCAF provides BinXCAFDrivers::DefineFormat()
# - TKOpenGl provides OpenGl_GraphicDriver
# - TKXDESTEP provides STEPCAFControl_Reader symbols for XCAF/STEP transfer
target_link_libraries(${PROJECT_NAME} PRIVATE
    TKernel
    TKMath
    TKBRep
    TKXCAF
    TKCAF
    TKLCAF
    TKV3d
    TKService
    TKVCAF
    TKBinXCAF
    TKOpenGl
    TKDESTEP
    glfw
)
