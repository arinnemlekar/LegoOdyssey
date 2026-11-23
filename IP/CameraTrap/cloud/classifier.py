"""Animal identification for camera-trap photos (cloud version).

Uses CLIP, an AI model that learned to match pictures with written
descriptions. We give it the team's animal list (edited on the website's
Animals page) and it tells us which description fits the photo best.

We also give it some descriptions of EMPTY scenes (grass, leaves, darkness).
If one of those fits best, the photo is marked "no animal".
"""

import os

import torch
from transformers import CLIPModel, CLIPProcessor

# The Docker image saves the model in /model so it doesn't download on every start.
MODEL = os.environ.get("CLIP_MODEL_DIR", "openai/clip-vit-base-patch32")

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

device = "cuda" if torch.cuda.is_available() else "cpu"
_model = CLIPModel.from_pretrained(MODEL, use_safetensors=True).to(device).eval()
_processor = CLIPProcessor.from_pretrained(MODEL)

_cache = {"list_text": None, "prompts": None, "features": None}


def parse_animal_list(text):
    """Turn the animal list into {label: description}.

    One animal per line. Lines starting with # are ignored. Optional
    description after a |, e.g.  opossum | a gray possum with a white face
    """
    animals = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        label, _, description = (part.strip() for part in line.partition("|"))
        if label:
            animals[label.lower()] = description or f"a {label}"
    return animals


@torch.no_grad()
def _prepare(list_text):
    """Turn the animal list into CLIP text features (only redone when the list changes)."""
    if _cache["list_text"] == list_text:
        return _cache["prompts"], _cache["features"]
    animals = parse_animal_list(list_text)
    prompts = [(label, f"a camera trap photo of {desc}.") for label, desc in animals.items()]
    for label, descriptions in NOT_ANIMAL.items():
        prompts += [(label, f"a camera trap photo of {d}.") for d in descriptions]
    tokens = _processor(text=[p for _, p in prompts], return_tensors="pt", padding=True).to(device)
    features = _model.get_text_features(**tokens)
    features = features / features.norm(dim=-1, keepdim=True)
    _cache.update(list_text=list_text, prompts=prompts, features=features)
    return prompts, features


@torch.no_grad()
def classify(image, animal_list_text):
    """Classify a PIL image. Returns a dict describing what's in the photo."""
    prompts, text_features = _prepare(animal_list_text)
    pixels = _processor(images=image.convert("RGB"), return_tensors="pt").to(device)
    image_features = _model.get_image_features(**pixels)
    image_features = image_features / image_features.norm(dim=-1, keepdim=True)
    scores = (_model.logit_scale.exp() * image_features @ text_features.T).softmax(dim=-1)[0]

    # Add up the probabilities for each label ("no animal" has several prompts).
    totals = {}
    for (label, _), p in zip(prompts, scores.tolist()):
        totals[label] = totals.get(label, 0.0) + p
    ranked = sorted(totals.items(), key=lambda kv: kv[1], reverse=True)

    best_label, best_score = ranked[0]
    return {
        "is_animal": best_label not in NOT_ANIMAL,
        "label": best_label,
        "confidence": round(best_score, 3),
        "top_guesses": [{"label": l, "confidence": round(p, 3)} for l, p in ranked[:3]],
    }
