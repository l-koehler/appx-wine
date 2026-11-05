# usage: make APP=<extracted package> BUILD=<output dir> SDK=<dir with the SDK contract .winmd files>
CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -g -Wall -Wno-unused-function -Wno-missing-braces -Isrc
SRCS = $(wildcard src/*.c)
APP_WINMD = $(wildcard $(APP)/*.winmd)
SDK_WINMD = $(SDK)/Windows.Foundation.FoundationContract.winmd $(SDK)/Windows.Foundation.UniversalApiContract.winmd \
	$(filter-out %FoundationContract.winmd %UniversalApiContract.winmd,$(wildcard $(SDK)/*.winmd))

$(BUILD)/winrtshim.dll: $(SRCS) src/shim.h $(BUILD)/winrt_meta.c
	$(CC) $(CFLAGS) -shared -o $@ $(SRCS) $(BUILD)/winrt_meta.c -Wl,--kill-at -static-libgcc \
		-lole32 -lgdi32 -ladvapi32 -lcrypt32 -lruntimeobject -lshell32 -luuid -ldxguid

# the app's own .winmd files go last: SDK definitions win on name clashes
$(BUILD)/winrt_meta.c: tools/gen.py $(APP_WINMD)
	mkdir -p $(BUILD)
	python3 -I tools/gen.py $@ $(SDK_WINMD) $(APP_WINMD)

clean:
	rm -f $(BUILD)/winrtshim.dll $(BUILD)/winrt_meta.c

.PHONY: clean
