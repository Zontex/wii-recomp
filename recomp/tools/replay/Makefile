# Builds the recording player from the runtime's graphics files, with no game.
#   make VULKAN_SDK=<folder>     where the Vulkan and shaderc headers and libraries are,
#                                if they aren't installed system-wide.
#   make EFB_SCALE=1 BUILD=build-1x     the picture's size against the Wii's. Each scale
#                                needs its own BUILD folder.
EFB_SCALE ?= 2
BUILD ?= build
RUNTIME = ../../runtime
GX_SOURCES = debug gaps guest_memory gx_backend gx_copy gx_draw gx_fifo gx_inspect gx_light gx_shader gx_tev gx_texture gx_transform gx_vertex gx_vulkan gx_vulkan_device gx_vulkan_pipeline gx_vulkan_window dff

CFLAGS = -O2 -w -fno-strict-aliasing -fwrapv -fsigned-char -ffp-contract=off -I$(RUNTIME) -I/usr/local/include -DGX_WITH_VULKAN "-DEFB_SCALE=$(EFB_SCALE)"
LDLIBS = -L/usr/local/lib -lglfw -lz -lm -lpthread
ifneq ($(VULKAN_SDK),)
CFLAGS += -I$(VULKAN_SDK)/include
LDLIBS += -L$(VULKAN_SDK)/lib -Wl,-rpath,$(VULKAN_SDK)/lib
endif
ifeq ($(shell uname),Linux)
LDLIBS += -lvulkan -lshaderc  # Debian's name for it.
else
LDLIBS += -lvulkan -lshaderc_shared
endif

OBJECTS = $(addprefix $(BUILD)/,$(GX_SOURCES:=.o) replay.o stubs.o)

$(BUILD)/replay: $(OBJECTS)
	clang $(OBJECTS) -o $@ $(LDLIBS)

$(BUILD)/%.o: $(RUNTIME)/%.c | $(BUILD)
	clang $(CFLAGS) -MMD -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	clang $(CFLAGS) -MMD -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)
.PHONY: clean
