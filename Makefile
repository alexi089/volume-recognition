PY        := .venv/bin/python
MODEL     := app/models/hand_landmarker.task
MODEL_URL := https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task

.PHONY: setup model run

setup: $(PY) model

$(PY):
	uv venv --python 3.12 .venv
	uv pip install --python $(PY) -r requirements.txt

model: $(MODEL)

$(MODEL):
	mkdir -p $(dir $(MODEL))
	curl -sSfL -o $(MODEL) $(MODEL_URL)

run: setup
	$(PY) app/main.py
