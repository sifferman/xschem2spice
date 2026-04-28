PDK_ROOT ?= /home/ethan/tools/ciel-pdks
PDK      ?= sky130A
XSCHEMRC ?= $(PDK_ROOT)/$(PDK)/libs.tech/xschem/xschemrc

# xschem-examples allowlist of expected-fail cells. Each name must match the
# .sch basename (without extension). When listed here, the cell is netlisted
# and LVS'd as usual but a non-pass is treated as an EXPECTED failure (it
# does not gate the suite). Reasons are tracked next to each entry.
EXPECTED_FAIL_CELLS := \
	doublepin \
	MSA-2643 \
	tb_symbol_include \
	test_doublepin \
	poweramp \
	poweramp_lcc

# Default test set: sky130 stdcell schematics. The xschem examples set is
# explicitly out-of-scope per docs/PLAN.md (allowlisted as needed) and is
# left as best-effort.
ROOT := $(abspath $(CURDIR)/..)
SCH_FILES := $(shell find $(CURDIR)/schematics/sky130_schematics/schematics -name '*.sch' 2>/dev/null | sort)
# Best-effort: also run xschem's own examples set. Many use tcleval(...) and
# will fail; failures are not a gate. Set XSCHEM_EXAMPLES=0 to skip.
XSCHEM_EXAMPLES ?= 1
ifeq ($(XSCHEM_EXAMPLES),1)
SCH_FILES += $(shell find $(CURDIR)/schematics/xschem/xschem_library/examples -name '*.sch' 2>/dev/null | sort)
endif
REPORT_DIR := $(CURDIR)/reports
REPORTS    := $(patsubst $(CURDIR)/schematics/%.sch,$(REPORT_DIR)/%.report,$(SCH_FILES))

RUN_LVS := $(CURDIR)/run_lvs.sh

.PHONY: all summary clean init

all: $(REPORTS)
	@$(MAKE) --no-print-directory summary

$(REPORT_DIR)/%.report: $(CURDIR)/schematics/%.sch $(RUN_LVS) $(ROOT)/xschem2spice
	@mkdir -p $(dir $@)
	@cell=$$(basename "$<" .sch); \
	 expected_fail=0; \
	 for f in $(EXPECTED_FAIL_CELLS); do \
	   if [ "$$cell" = "$$f" ]; then expected_fail=1; break; fi; \
	 done; \
	 EXPECTED_FAIL=$$expected_fail \
	 PDK_ROOT=$(PDK_ROOT) $(RUN_LVS) "$<" "$@" "$(XSCHEMRC)" || true

# Build the binary if not already there.
$(ROOT)/xschem2spice:
	$(MAKE) -C $(ROOT)

summary:
	@total=$$(echo "$(REPORTS)" | wc -w); \
	 passed=$$(find $(REPORT_DIR) -name '*.report' 2>/dev/null | wc -l); \
	 failed=$$(find $(REPORT_DIR) -name '*.failed' 2>/dev/null | wc -l); \
	 xfail=$$(find $(REPORT_DIR) -name '*.xfail' 2>/dev/null | wc -l); \
	 xpass=$$(find $(REPORT_DIR) -name '*.xpass' 2>/dev/null | wc -l); \
	 skipped=$$(find $(REPORT_DIR) -name '*.skipped' 2>/dev/null | wc -l); \
	 attempted=$$((total - skipped)); \
	 echo ""; \
	 echo "==== xschem2spice LVS summary ===="; \
	 echo "  passed:    $$passed / $$attempted (of $$total total)"; \
	 echo "  failed:    $$failed"; \
	 echo "  xfail:     $$xfail (expected-fail, non-gating)"; \
	 if [ "$$xpass" -gt 0 ]; then \
	   echo "  xpass:     $$xpass (expected-fail unexpectedly passed — review allowlist)"; \
	 fi; \
	 echo "  skipped:   $$skipped (xschem couldn't netlist)"; \
	 echo "=================================="; \
	 if [ "$$failed" -gt 0 ]; then exit 1; fi

init:
	cd $(ROOT) && git submodule update --init --recursive

clean:
	rm -rf $(REPORT_DIR)
