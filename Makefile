# SPDX-License-Identifier: MIT
# xr-tweak build. Normally run inside the docker image via ./build.sh, which builds both arches.
#   make ARCH=x86_64            -> out/x86_64/libxr_tweak.so (+ the test programs)
#   make ARCH=aarch64           -> out/aarch64/libxr_tweak.so (cross, aarch64-linux-gnu-g++)
ARCH ?= $(shell uname -m)
OPENXR ?= /opt/openxr
ifeq ($(ARCH),aarch64)
  CXX := $(if $(filter aarch64,$(shell uname -m)),g++,aarch64-linux-gnu-g++)
  STRIP := $(if $(filter aarch64,$(shell uname -m)),strip,aarch64-linux-gnu-strip)
else
  CXX := g++
  STRIP := strip
endif
OUT := out/$(ARCH)
GEN := out/gen
CXXFLAGS := -std=c++17 -O2 -g -fPIC -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -I$(OPENXR)/include -I$(GEN) -Isrc
LDFLAGS := -shared -Wl,--no-undefined -Wl,-z,defs -static-libstdc++ -static-libgcc -Wl,--version-script=src/exports.map
SRC := src/layer.cpp src/config.cpp src/sharpen.cpp

all: $(OUT)/libxr_tweak.so

$(GEN)/fullscreen_vert.h: shaders/fullscreen.vert
	@mkdir -p $(GEN)
	glslangValidator -V --vn fullscreen_vert -o $@ $<
$(GEN)/rcas_frag.h: shaders/rcas.frag
	@mkdir -p $(GEN)
	glslangValidator -V --vn rcas_frag -o $@ $<

$(OUT)/libxr_tweak.so: $(SRC) src/*.h $(GEN)/fullscreen_vert.h $(GEN)/rcas_frag.h src/exports.map
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(SRC) $(LDFLAGS) -o $@.debug -ldl -lpthread
	$(STRIP) --strip-unneeded -o $@ $@.debug

# off-headset tests (x86_64 only: they need lavapipe + the OpenXR loader from the image)
tests: $(OUT)/stub_runtime.so $(OUT)/test_app $(OUT)/test_config $(OUT)/fake_valve_layer.so
$(OUT)/stub_runtime.so: test/stub_runtime.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O1 -g -fPIC -shared -fvisibility=hidden -I$(OPENXR)/include $< -o $@ -lvulkan -ldl
$(OUT)/fake_valve_layer.so: test/fake_valve_layer.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O1 -g -fPIC -shared -fvisibility=hidden -I$(OPENXR)/include $< -o $@
$(OUT)/test_app: test/test_app.cpp
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O1 -g -I$(OPENXR)/include $< -o $@ -L$(OPENXR)/lib -lopenxr_loader -Wl,-rpath,$(OPENXR)/lib -lvulkan -ldl
$(OUT)/test_config: test/test_config.cpp src/config.cpp src/config.h
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O1 -g -Isrc test/test_config.cpp src/config.cpp -o $@

clean:
	rm -rf out
.PHONY: all tests clean
