# Smart Conveyor Sorting System Makefile

.PHONY: install run-server build-firmware test clean

# Install python dependencies in virtual environment
install:
	python -m venv venv
	./venv/Scripts/pip install --upgrade pip
	./venv/Scripts/pip install -r requirements.txt
	./venv/Scripts/pip install platformio

# Run the Python TCP AI Camera Server
run-server:
	./venv/Scripts/python main.py

# Compile C++ ESP32 firmware using PlatformIO CLI
build-firmware:
	./venv/Scripts/pio run

# Run Pytest unit tests
test:
	./venv/Scripts/pytest -v

# Clean caches and PlatformIO intermediate files
clean:
	rm -rf __pycache__ .pytest_cache tests/__pycache__
	./venv/Scripts/pio run --target clean
