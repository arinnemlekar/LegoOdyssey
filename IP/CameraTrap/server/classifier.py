"""Animal identification for camera-trap photos.

Uses CLIP, an AI model that learned to match pictures with written
descriptions. We give it a list of animal names (animals.txt) and it tells us
which description fits the photo best. Because the list is just text, you can
add any animal you expect to see without retraining anything.

We also give it some descriptions of EMPTY scenes (grass, leaves, darkness).
If one of those fits best, the photo is marked "no animal" — the PIR sensor
was probably triggered by wind, sun, or moving plants.
"""

from pathlib import Path

import torch
from PIL import Image
from transformers import CLIPModel, CLIPProcessor

MODEL_NAME = "openai/clip-vit-base-patch32"   # ~600 MB download the first time
ANIMALS_FILE = Path(__file__).parent / "animals.txt"

# Labels that are not animals. "person" is kept separate so people walking by
# don't count as wildlife.
NOT_ANIMAL = {
    "no animal": [
        "an empty outdoor scene with grass and trees",
        "a close-up of leaves and branches with no animal",
        "a dark night photo with nothing in it",
        "a blurry photo of the ground",
        "an empty backyard",
    ],
    "person": ["a person", "a person's legs walking by"],
}


def load_animals():
    """Read animals.txt into {label: description}."""
    animals = {}
    for line in ANIMALS_FILE.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        label, _, description = (part.strip() for part in line.partition("|"))
        animals[label] = description or f"a {label}"
    return animals


device = "cuda" if torch.cuda.is_available() else "cpu"
_model = CLIPModel.from_pretrained(MODEL_NAME, use_safetensors=True).to(device).eval()
_processor = CLIPProcessor.from_pretrained(MODEL_NAME)

# Build one list of (label, prompt) pairs. Every photo is compared to all of them.
_animals = load_animals()
_prompts = [(label, f"a camera trap photo of {desc}.") for label, desc in _animals.items()]
for label, descriptions in NOT_ANIMAL.items():
    _prompts += [(label, f"a camera trap photo of {d}.") for d in descriptions]

with torch.no_grad():
    _text = _processor(text=[p for _, p in _prompts], return_tensors="pt", padding=True).to(device)
    _text_features = _model.get_text_features(**_text)
    _text_features = _text_features / _text_features.norm(dim=-1, keepdim=True)


@torch.no_grad()
def classify(image_path):
    """Return a dict describing what's in the photo."""
    image = Image.open(image_path).convert("RGB")
    pixels = _processor(images=image, return_tensors="pt").to(device)
    image_features = _model.get_image_features(**pixels)
    image_features = image_features / image_features.norm(dim=-1, keepdim=True)

    # How well the photo matches each prompt, turned into probabilities.
    scores = (_model.logit_scale.exp() * image_features @ _text_features.T).softmax(dim=-1)[0]

    # Add up the probabilities for each label (the "no animal" label has several prompts).
    totals = {}
    for (label, _), p in zip(_prompts, scores.tolist()):
        totals[label] = totals.get(label, 0.0) + p
    ranked = sorted(totals.items(), key=lambda kv: kv[1], reverse=True)

    best_label, best_score = ranked[0]
    return {
        "is_animal": best_label not in NOT_ANIMAL,
        "label": best_label,
        "confidence": round(best_score, 3),
        "top_guesses": [{"label": l, "confidence": round(p, 3)} for l, p in ranked[:3]],
    }


if __name__ == "__main__":
    # Try it on any photos:  python classifier.py photo1.jpg photo2.jpg
    import json
    import sys
    print(f"Looking for {len(_animals)} kinds of animals (running on {device})")
    for path in sys.argv[1:]:
        print(path, json.dumps(classify(path), indent=2))
