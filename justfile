build:
  cmake -Bbuild
  cmake --build build

clean:
  rm -rf build

watch:
  ls main.cpp | entr -r sh -c "just build && ./build/OCCT_XCAF_FacePicker battery-adapter.step"
