
PDK         ?= sky130A
XSCHEMRC    ?= $(PDK_ROOT)/$(PDK)/libs.tech/xschem/xschemrc
WASM_RUNNER ?= wasmtime run --dir=.

XSCHEM_TEST_EXPECTED_FAIL_CELLS := doublepin \
                                   MSA-2643 \
                                   tb_symbol_include \
                                   test_doublepin \
                                   poweramp \
                                   poweramp_lcc

ROOT       := $(abspath $(CURDIR)/..)
REPORT_DIR := $(CURDIR)/reports

SCH_FILES := \
	$(shell find $(CURDIR)/schematics/sky130_schematics/schematics -name '*.sch' 2>/dev/null | sort) \
	$(shell find $(CURDIR)/schematics/xschem/xschem_library/examples -name '*.sch' 2>/dev/null | sort)
REPORTS   := $(patsubst $(CURDIR)/schematics/%.sch,$(REPORT_DIR)/%.report,$(SCH_FILES))

WASM_SMOKE_BINARY    = $(ROOT)/xschem2spice.wasm
WASM_SMOKE_SCHEMATIC = schematics/xschem/xschem_library/examples/diode_1.sch
WASM_SMOKE_GOLDEN    = wasm-smoke/diode_1.golden.spice
WASM_SMOKE_OUTPUT    = wasm-smoke/diode_1.out.spice
WASM_SMOKE_XSCHEMRC  = wasm-smoke/xschemrc

.PHONY: summary clean init wasm-smoke

summary: $(REPORTS)
	@for ext in report failed xfail xpass skipped; do \
	   printf "  %-8s %d\n" "$$ext" "$$(find $(REPORT_DIR) -name "*.$$ext" 2>/dev/null | wc -l)"; \
	 done
	@! find $(REPORT_DIR) -name '*.failed' 2>/dev/null | grep -q .

$(REPORT_DIR)/%.report: $(CURDIR)/schematics/%.sch $(CURDIR)/run_lvs.sh $(ROOT)/xschem2spice
	@mkdir -p $(dir $@)
	@cell=$$(basename "$<" .sch); \
	 expected_fail=0; \
	 for f in $(XSCHEM_TEST_EXPECTED_FAIL_CELLS); do \
	   if [ "$$cell" = "$$f" ]; then expected_fail=1; break; fi; \
	 done; \
	 EXPECTED_FAIL=$$expected_fail \
	 PDK_ROOT=$(PDK_ROOT) $(CURDIR)/run_lvs.sh "$<" "$@" "$(XSCHEMRC)" || true

wasm-smoke: $(WASM_SMOKE_BINARY)
	$(WASM_RUNNER) $(WASM_SMOKE_BINARY) --xschemrc $(WASM_SMOKE_XSCHEMRC) $(WASM_SMOKE_SCHEMATIC) \
	    | grep -v '^\*\* sch_path:' > $(WASM_SMOKE_OUTPUT)
	diff -u $(WASM_SMOKE_GOLDEN) $(WASM_SMOKE_OUTPUT)
	@echo "WASM smoke test passed."

# Build the binaries if not already there.
$(ROOT)/xschem2spice:
	$(MAKE) -C $(ROOT)

$(WASM_SMOKE_BINARY):
	$(MAKE) -C $(ROOT) wasm

init:
	cd $(ROOT) && git submodule update --init --recursive

clean:
	rm -rf $(REPORT_DIR) $(WASM_SMOKE_OUTPUT)
