# roadmap-corpus-survey.py: a roadmap with no items

`tools/roadmap-corpus-survey.py` prints, per field, the share of items that
lack it. With roadmaps found but no items among them, that share divided by
zero and the survey stopped with a traceback (ANTS-6630).

- **INV-1 — a roadmap with no items is reported, not fatal.** Run over a
  project whose `ROADMAP.md` holds no items, the survey exits 0 and prints
  its field-absence section.

Test: `test_roadmap_corpus_survey.py` writes a throwaway project with an
item-less `ROADMAP.md` and runs the survey on it.
