# Container for the Cloud Run upload service.
FROM python:3.12-slim

WORKDIR /app
ENV PYTHONUNBUFFERED=1 \
    HF_HUB_DISABLE_TELEMETRY=1 \
    CLIP_MODEL_DIR=/model

# CPU-only PyTorch: Cloud Run has no GPU, and this version is ~5x smaller.
RUN pip install --no-cache-dir torch --index-url https://download.pytorch.org/whl/cpu
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Download the AI model once, while building, so the service starts faster.
RUN python -c "from transformers import CLIPModel, CLIPProcessor; \
m='openai/clip-vit-base-patch32'; \
CLIPModel.from_pretrained(m, use_safetensors=True).save_pretrained('/model'); \
CLIPProcessor.from_pretrained(m).save_pretrained('/model')"

COPY *.py animals.txt ./

# One photo at a time per instance; long timeout covers the first (cold) start.
CMD exec gunicorn --bind :$PORT --workers 1 --threads 1 --timeout 120 main:app
