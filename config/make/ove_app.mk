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

# Fragment-based configuration: make <board>.<rtos>.<app> [ZEROHEAP=1]
_DOT_TARGETS := $(foreach t,$(MAKECMDGOALS),$(if $(findstring .,$(t)),$(t)))
ifneq ($(_DOT_TARGETS),)
.PHONY: $(_DOT_TARGETS)
$(_DOT_TARGETS): $(VENV_STAMP)
	@$(OVE) defconfig-fragments "$@" $(if $(ZEROHEAP),--zeroheap)
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

.PHONY: alldefconfigs
alldefconfigs: $(VENV_STAMP)
	@CONFIGS=$$(find $(APP_DIR)/defconfigs -name '*_defconfig' -type f | sort); \
	TOTAL=$$(echo "$$CONFIGS" | wc -l); \
	CURRENT=0; FAILED=""; \
	for cfg in $$CONFIGS; do \
		NAME=$$(basename $$cfg); \
		CURRENT=$$((CURRENT + 1)); \
		echo ""; \
		echo "============================================================"; \
		echo "[$$CURRENT/$$TOTAL] Building $$NAME"; \
		echo "============================================================"; \
		if $(MAKE) $$NAME && $(MAKE); then \
			echo "[$$CURRENT/$$TOTAL] $$NAME: OK"; \
		else \
			echo "[$$CURRENT/$$TOTAL] $$NAME: FAILED"; \
			FAILED="$$FAILED $$NAME"; \
		fi; \
	done; \
	echo ""; \
	echo "============================================================"; \
	if [ -n "$$FAILED" ]; then \
		echo "FAILED:$$FAILED"; exit 1; \
	else \
		echo "All $$TOTAL configurations built successfully"; \
	fi

.PHONY: clean
clean:
	rm -rf $(APP_DIR)/output

.PHONY: help
help:
	@echo ""
	@echo "oveRTOS out-of-tree app (OVE_DIR: $(OVE_DIR))"
	@echo ""
	@echo "  <board>.<rtos>.<app>  Configure from fragments [ZEROHEAP=1]"
	@echo "  <name>_defconfig   Load config    menuconfig   TUI config"
	@echo "  all (default)      Full build     run          Run firmware"
	@echo "  flash              Flash board    alldefconfigs Build all"
	@echo "  clean              Remove output  help         This message"
	@echo ""
	@echo "Defconfigs:"
	@for f in $$(find $(APP_DIR)/defconfigs -name "*_defconfig" -type f | sort); do \
		echo "  $$(basename $$f)"; \
	done
	@echo ""

.DEFAULT_GOAL := all
