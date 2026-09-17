PY        := .venv/bin/python
MODEL     := app/models/hand_landmarker.task
MODEL_URL := https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task

.PHONY: setup model core test run

# The stamp file is written only after the install succeeds. Depending on
# .venv/bin/python instead would treat a venv whose install failed half-way as
# finished, and never retry.
STAMP := .venv/.installed

setup: $(STAMP) model

$(STAMP): requirements.txt
	test -x $(PY) || uv venv --python 3.12 .venv
	uv pip install --python $(PY) -r requirements.txt
	touch $(STAMP)

model: $(MODEL)

$(MODEL):
	mkdir -p $(dir $(MODEL))
	curl -sSfL -o $(MODEL) $(MODEL_URL)

core:
	$(MAKE) -C core all

# C unit tests (under ASan + UBSan), the Python <-> C end-to-end test, and the
# memory-growth test. None of them touches the camera or the real system volume.
test: setup core
	$(MAKE) -C core test
	$(PY) app/test_bridge.py
	$(PY) app/test_leak.py

# Extra arguments: make run ARGS="--record traces/slide_up.csv"
run: setup core
	$(PY) app/main.py $(ARGS)
