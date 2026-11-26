CC        ?= clang
PREFIX    ?= /opt/homebrew
CFLAGS    ?= -O2 -Wall -Wextra

TARGET = yammidi-bridge
SRC    = src/yammidi-bridge.c

# CoreMIDI and CoreFoundation are the real dependencies. IOKit and Security are
# needed only when libusb is linked statically: the dylib carries them itself,
# the .a does not. Omitting Security yields undefined _SecTaskCreateFromSelf
# and _SecTaskCopyValueForEntitlement from darwin_detach_kernel_driver.
FRAMEWORKS = -framework CoreMIDI -framework CoreFoundation -framework IOKit -framework Security

LIBUSB_STATIC = $(PREFIX)/lib/libusb-1.0.a

all: $(TARGET)

# Static by default. A dynamic dependency on a Homebrew path breaks silently
# after `brew upgrade` or `brew cleanup`; a LaunchAgent with KeepAlive then
# thrashes trying to exec a binary whose dependency has vanished.
$(TARGET): $(SRC)
	@if [ -f "$(LIBUSB_STATIC)" ]; then \
		echo "linking statically: $(LIBUSB_STATIC)"; \
		$(CC) $(CFLAGS) -I$(PREFIX)/include -o $@ $(SRC) \
			$(LIBUSB_STATIC) $(FRAMEWORKS); \
	else \
		echo "warning: $(LIBUSB_STATIC) missing, falling back to dylib"; \
		$(CC) $(CFLAGS) -I$(PREFIX)/include -o $@ $(SRC) \
			-L$(PREFIX)/lib -lusb-1.0 $(FRAMEWORKS); \
	fi

deps:
	@echo "libusb via Homebrew:  brew install libusb"

clean:
	rm -f $(TARGET)

.PHONY: all deps clean
