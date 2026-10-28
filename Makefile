VENV := .venv
PYTHON := python3
BIN := $(VENV)/bin
PIP := $(BIN)/pip
PY := $(BIN)/python

.DEFAULT_GOAL := help

.PHONY: help venv install run test bench compare clean

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-10s\033[0m %s\n", $$1, $$2}'

venv: $(VENV)/.stamp ## Create the virtual environment

$(VENV)/.stamp: requirements.txt
	$(PYTHON) -m venv $(VENV)
	$(PIP) install --upgrade pip
	$(PIP) install -r requirements.txt
	@touch $(VENV)/.stamp

install: venv ## Alias for venv

run: venv ## Run the matching engine
	$(PY) -m python_engine

test: venv ## Run the test suite
	$(PY) -m pytest

bench: venv ## Run benchmarks and write results/python_microbench.json
	PYTHONPATH=. $(PY) benchmark/bench.py

compare: venv ## Replay the same order flow through Python and C++; check fills, compare time + memory
	$(MAKE) -C cpp_engine replay
	$(PY) benchmark/compare.py

clean: ## Remove the venv and caches
	rm -rf $(VENV)
	find . -type d -name __pycache__ -exec rm -rf {} +
