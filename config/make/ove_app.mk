# oveRTOS out-of-tree application glue
#
# Include from your app Makefile after setting APP_DIR and OVE_DIR:
#
#   APP_DIR := $(CURDIR)
#   OVE_DIR := $(APP_DIR)/../../oveRTOS
#   include $(OVE_DIR)/config/make/ove_app.mk

VENV_DIR    := $(OVE_DIR)/.venv
VENV_STAMP  := $(VENV_DIR)/.stamp
OVE         := $(VENV_DIR)/bin/ove

export OVE_EXTERNAL_APPS := $(APP_DIR)
export PATH := $(OVE_DIR)/config/scripts:$(VENV_DIR)/bin:$(PATH)

$(VENV_STAMP):
	@$(MAKE) -C $(OVE_DIR) setup

.PHONY: menuconfig savedefconfig nuttx-menuconfig zephyr-menuconfig
menuconfig: $(VENV_STAMP)
	@$(OVE) menuconfig
savedefconfig: $(VENV_STAMP)
	@$(OVE) savedefconfig
nuttx-menuconfig: $(VENV_STAMP)
	@$(OVE) rtos-menuconfig nuttx
zephyr-menuconfig: $(VENV_STAMP)
	@$(OVE) rtos-menuconfig zephyr

%_defconfig: $(VENV_STAMP)
	@$(OVE) defconfig $@

# Fragment-based configuration: make <board>.<rtos>.<app>
_DOT_TARGETS := $(foreach t,$(MAKECMDGOALS),$(if $(findstring .,$(t)),$(t)))
ifneq ($(_DOT_TARGETS),)
.PHONY: $(_DOT_TARGETS)
$(_DOT_TARGETS): $(VENV_STAMP)
	@$(OVE) defconfig-fragments "$@"
endif

.PHONY: download configure all
download: $(VENV_STAMP)
	@$(OVE) download
configure: $(VENV_STAMP)
	@$(OVE) configure
all: $(VENV_STAMP)
	@$(OVE) download
	@$(OVE) configure
	@$(OVE) build

.PHONY: run flash
run: $(VENV_STAMP)
	@$(OVE) run $(if $(filter 1,$(HEADLESS)),--headless)
flash: $(VENV_STAMP)
	@$(OVE) flash

.PHONY: vscode
vscode: $(VENV_STAMP)
	@$(OVE) vscode $(if $(filter 1,$(NO_OPEN)),--no-open)

.PHONY: lint format
lint: $(VENV_STAMP)
	@$(OVE) lint
format: $(VENV_STAMP)
	@$(OVE) format

.PHONY: alldefconfigs
alldefconfigs: $(VENV_STAMP)
	@$(OVE) alldefconfigs "$(APP_DIR)"

.PHONY: clean
clean:
	rm -rf $(APP_DIR)/output

.PHONY: help
help:
	@echo ""
	@echo "oveRTOS out-of-tree app (OVE_DIR: $(OVE_DIR))"
	@echo ""
	@echo "  <board>.<rtos>.<app>  Configure from fragments"
	@echo "  <name>_defconfig   Load config    menuconfig   TUI config"
	@echo "  all (default)      Full build     run          Run firmware"
	@echo "  flash              Flash board    alldefconfigs Build all"
	@echo "  vscode             Open in VSCode clean        Remove output"
	@echo "  lint               Run all linters  format      Auto-format sources"
	@echo "  help               This message"
	@if [ -d "$(APP_DIR)/defconfigs" ] && \
	    [ -n "$$(find $(APP_DIR)/defconfigs -maxdepth 2 -name '*_defconfig' -type f 2>/dev/null)" ]; then \
		echo ""; \
		echo "Defconfigs:"; \
		find $(APP_DIR)/defconfigs -name '*_defconfig' -type f | sort | while read f; do \
			echo "  $$(basename $$f)"; \
		done; \
	fi
	@echo ""

.DEFAULT_GOAL := all
