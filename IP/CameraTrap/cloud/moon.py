"""Moon phase calculator.

The moon goes from new moon to new moon every 29.53 days (a "synodic month").
If we know the date of one new moon, we can count forward to find where the
moon is in its cycle on any other date. Accurate to within about half a day.
"""

import math
from datetime import datetime, timezone

SYNODIC_MONTH_DAYS = 29.530588853
KNOWN_NEW_MOON = datetime(2000, 1, 6, 18, 14, tzinfo=timezone.utc)

PHASE_NAMES = [
    "New Moon", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
    "Full Moon", "Waning Gibbous", "Last Quarter", "Waning Crescent",
]
PHASE_ICONS = ["🌑", "🌒", "🌓", "🌔", "🌕", "🌖", "🌗", "🌘"]


def moon_phase(when_utc):
    """Return (phase, name, illumination) for a UTC datetime.

    phase:        0.0 = new moon, 0.5 = full moon, back to 1.0 = new moon
    name:         one of PHASE_NAMES
    illumination: fraction of the moon's face that is lit, 0.0 to 1.0
    """
    days_since = (when_utc - KNOWN_NEW_MOON).total_seconds() / 86400
    phase = (days_since / SYNODIC_MONTH_DAYS) % 1.0
    illumination = (1 - math.cos(2 * math.pi * phase)) / 2
    name = PHASE_NAMES[round(phase * 8) % 8]
    return round(phase, 4), name, round(illumination, 3)


def icon_for(name):
    return PHASE_ICONS[PHASE_NAMES.index(name)] if name in PHASE_NAMES else ""


if __name__ == "__main__":
    print("Right now:", moon_phase(datetime.now(timezone.utc)))
