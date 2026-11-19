# LEADIN - build the ESP32 firmware for each board.
# Works on Ubuntu (and Debian) and on CachyOS (and Arch).
#
# First time:
#   make deps                      install the system packages (uses sudo)
#   make setup                     download ESP-IDF (about 2.4 GB)
#
# Then:
#   make                           build the firmware for every board into dist/
#   make flash                     build and flash the Audio Kit (PORT=/dev/ttyUSB0)
#   make monitor                   show the board's log (Ctrl+] to stop)
#   make chip-info                 check the serial link and the flash chip
#     If flashing fails, try BAUD=115200, and then ESPTOOL_OPTS=--no-stub
#   make release                   make dist/leadin-$(VERSION).zip
#   make boards                    list the boards (audiokit, and qemu for tests)
#   make clean                     remove the build folders and dist/
#   make distclean                 also remove the downloaded tools
#
# Use an ESP-IDF that you have already:
#   make IDF=~/esp/esp-idf IDF_TOOLS=~/.espressif

SHELL := /bin/bash
.DEFAULT_GOAL := all

# The version: LEADIN_VERSION in main/leadin.h. The web UI shows it, and the
# release zip carries it.
VERSION     ?= $(shell sed -n 's/.*LEADIN_VERSION *"\([^"]*\)".*/\1/p' main/leadin.h)
BOARD       ?= audiokit
PORT        ?= $(firstword $(wildcard /dev/ttyUSB* /dev/ttyACM*))
BAUD        ?= 460800
ESPTOOL_OPTS ?=

TOOLS       ?= $(HOME)/.leadin-tools
IDF_VERSION ?= v5.4.1
IDF         ?= $(TOOLS)/esp-idf-$(IDF_VERSION)
IDF_TOOLS   ?= $(TOOLS)/espressif
PYBIN       := $(TOOLS)/python-bin

IDF_GIT     := https://github.com/espressif/esp-idf.git

FW          := .
DIST        := dist
BOARDS      := $(sort $(patsubst $(FW)/boards/%/board.c,%,$(wildcard $(FW)/boards/*/board.c)))
REL_BOARDS  := $(filter-out qemu,$(BOARDS))

# Packages: the list from Espressif's Linux setup guide, plus zip. ESP-IDF
# itself needs Python 3.10 to 3.13.
APT_PKGS    := git wget curl flex bison gperf python3 python3-pip python3-venv cmake \
               ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 \
               zip unzip xz-utils
PACMAN_PKGS := gcc git make wget curl flex bison gperf python python-pip cmake ninja \
               ccache dfu-util libusb zip unzip xz

# Runs a command with ESP-IDF set up.
IDF_ENV = export IDF_PATH="$(IDF)" IDF_TOOLS_PATH="$(IDF_TOOLS)" IDF_COMPONENT_MANAGER=0 \
                 PATH="$(PYBIN):$$PATH" && . "$(IDF)/export.sh" > /dev/null

.PHONY: chip-info has-python all help deps setup idf python check-idf firmware firmware-one \
        flash monitor menuconfig release boards clean distclean

all: firmware
	@echo
	@echo "Done. The files are in $(DIST)/:"
	@ls $(DIST)

help:
	@sed -n '2,22p' $(firstword $(MAKEFILE_LIST)) | sed 's/^# \{0,1\}//'

boards:
	@for b in $(BOARDS); do printf '  %-10s %s\n' $$b "$$(sed -n '1s|^/\* *||p' $(FW)/boards/$$b/board.c)"; done

# ------------------------------------------------------------ system packages
deps:
	@. /etc/os-release; \
	case " $$ID $$ID_LIKE " in \
	  *" arch "*|*" cachyos "*) \
	    echo "Installing packages with pacman ($$NAME)"; \
	    if ! sudo pacman -S --needed --noconfirm $(PACMAN_PKGS); then \
	      echo; echo "pacman failed. If it could not download a file (error 404), the package"; \
	      echo "list is out of date. Run 'sudo pacman -Syu', then 'make deps' again."; exit 1; \
	    fi; \
	    if ! $(MAKE) --no-print-directory -s has-python && ! command -v uv > /dev/null; then \
	      echo "No Python 3.10 to 3.13. Installing uv, to get Python 3.13 for ESP-IDF."; \
	      sudo pacman -S --needed --noconfirm uv || \
	        { echo "Could not install uv. Run 'sudo pacman -Syu', then 'make deps' again."; exit 1; }; \
	    fi; \
	    group=uucp ;; \
	  *" ubuntu "*|*" debian "*) \
	    echo "Installing packages with apt ($$NAME)"; \
	    sudo apt-get update && sudo apt-get install -y $(APT_PKGS); group=dialout ;; \
	  *) echo "This Makefile knows Ubuntu, Debian, CachyOS and Arch. Your system: $$NAME"; \
	     echo "Install these packages yourself: $(APT_PKGS)"; exit 1 ;; \
	esac; \
	if ! id -nG | grep -qw $$group; then \
	  echo "Adding $$(id -un) to the '$$group' group, so that flashing can use the serial port."; \
	  sudo usermod -aG $$group "$$(id -un)"; \
	  echo "Log out and log in again before 'make flash'."; \
	fi

# -------------------------------------------------------------------- tools
setup: idf
	@echo "Tools ready in $(TOOLS). Now run: make"

# Exit status 0 if a Python that ESP-IDF supports is installed.
PY_CANDIDATES := python3.13 python3.12 python3.11 python3.10 python3
PY_OK := import sys, venv; sys.exit(not (3,10) <= sys.version_info[:2] <= (3,13))
has-python:
	@for p in $(PY_CANDIDATES); do \
	  command -v $$p > /dev/null && $$p -c '$(PY_OK)' 2> /dev/null && exit 0; \
	done; exit 1

# ESP-IDF 5.4 needs Python 3.10 to 3.13. Use the first one found. If there is
# none (for example, the system has Python 3.14), uv downloads Python 3.13
# for ESP-IDF only. The system Python does not change.
python: $(PYBIN)/python3
$(PYBIN)/python3:
	@mkdir -p $(PYBIN); \
	for p in $(PY_CANDIDATES); do \
	  if command -v $$p > /dev/null && $$p -c '$(PY_OK)' 2> /dev/null; then \
	    ln -sf "$$(command -v $$p)" $@; echo "Python for ESP-IDF: $$($@ --version)"; exit 0; \
	  fi; \
	done; \
	if command -v uv > /dev/null; then \
	  echo "No Python 3.10 to 3.13 found. uv installs Python 3.13 for ESP-IDF."; \
	  uv python install 3.13 && ln -sf "$$(uv python find 3.13)" $@ && echo "Python for ESP-IDF: $$($@ --version)" && exit 0; \
	fi; \
	echo "ESP-IDF $(IDF_VERSION) needs Python 3.10 to 3.13. Install uv (make deps) or one of these Python versions."; \
	exit 1

idf: $(PYBIN)/python3
	@if [ ! -f "$(IDF)/export.sh" ]; then \
	  echo "Downloading ESP-IDF $(IDF_VERSION) to $(IDF)"; \
	  git clone -b $(IDF_VERSION) --depth 1 --recursive --shallow-submodules $(IDF_GIT) "$(IDF)" || \
	    (cd "$(IDF)" && git submodule update --init --recursive); \
	fi
	cd "$(IDF)" && PATH="$(PYBIN):$$PATH" IDF_TOOLS_PATH="$(IDF_TOOLS)" ./install.sh esp32

check-idf:
	@test -f "$(IDF)/export.sh" || (echo "ESP-IDF not found in $(IDF). Run: make setup"; exit 1)
	@test -e "$(PYBIN)/python3" || $(MAKE) --no-print-directory python

# ----------------------------------------------------------------- firmware
# One build folder for each board: build-<board>. When the settings
# files change, the board's sdkconfig is made again from them.
firmware: $(addprefix fw-,$(BOARDS))

fw-%: check-idf
	@test -f $(FW)/boards/$*/board.c || (echo "No board '$*'. Boards: $(BOARDS)"; exit 1)
	@echo "=== Firmware for $*"
	cd $(FW) && $(IDF_ENV) && \
	  b=build-$*; reconf=; \
	  if [ ! -f $$b/sdkconfig ] || [ -n "$$(find sdkconfig.defaults partitions.csv boards/$*/sdkconfig.board main/Kconfig.projbuild -newer $$b/sdkconfig)" ]; then \
	    rm -f $$b/sdkconfig; reconf=reconfigure; \
	  fi; \
	  idf.py -B $$b -D SDKCONFIG=$$b/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/$*/sdkconfig.board" $$reconf build
	@mkdir -p $(DIST)/firmware/$*
	cd $(FW)/build-$* && cp leadin.bin bootloader/bootloader.bin partition_table/partition-table.bin \
	  ota_data_initial.bin $(CURDIR)/$(DIST)/firmware/$*/
	cd $(FW)/build-$* && $(IDF_ENV) && \
	  python -m esptool --chip esp32 merge_bin -o $(CURDIR)/$(DIST)/firmware/$*/leadin-$*-full.bin @flash_args > /dev/null
	if [ -f $(FW)/boards/$*/FLASH.TXT ]; then cp $(FW)/boards/$*/FLASH.TXT $(DIST)/firmware/$*/; fi

firmware-one: fw-$(BOARD)

flash: fw-$(BOARD)
	@test -n "$(PORT)" || (echo "No serial port found. Connect the board, or give PORT=/dev/ttyUSB0"; exit 1)
	cd $(FW) && $(IDF_ENV) && python -m esptool --chip esp32 -p $(PORT) -b $(BAUD) $(ESPTOOL_OPTS) \
	  write_flash 0x0 $(CURDIR)/$(DIST)/firmware/$(BOARD)/leadin-$(BOARD)-full.bin

chip-info: check-idf
	@test -n "$(PORT)" || (echo "No serial port found. Give PORT=/dev/ttyUSB0"; exit 1)
	cd $(FW) && $(IDF_ENV) && python -m esptool --chip esp32 -p $(PORT) -b $(BAUD) $(ESPTOOL_OPTS) flash_id

monitor: check-idf
	@test -n "$(PORT)" || (echo "No serial port found. Give PORT=/dev/ttyUSB0"; exit 1)
	cd $(FW) && $(IDF_ENV) && idf.py -B build-$(BOARD) -p $(PORT) monitor

menuconfig: check-idf
	cd $(FW) && $(IDF_ENV) && idf.py -B build-$(BOARD) -D SDKCONFIG=build-$(BOARD)/sdkconfig \
	  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/$(BOARD)/sdkconfig.board" menuconfig

# ------------------------------------------------------------------ release
release: all
	rm -f $(DIST)/leadin-$(VERSION).zip
	cp README.md PROTOCOL.md $(DIST)/
	cd $(DIST) && zip -qr leadin-$(VERSION).zip README.md PROTOCOL.md $(addprefix firmware/,$(REL_BOARDS))
	@echo "Release: $(DIST)/leadin-$(VERSION).zip"

clean:
	rm -rf $(addprefix $(FW)/build-,$(BOARDS)) $(DIST)

distclean: clean
	rm -rf "$(TOOLS)"
