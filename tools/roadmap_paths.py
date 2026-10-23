"""Where the tools/roadmap-*.py scripts find a project's roadmap.

One copy, so a fix to discovery reaches every script. Two rules:

  root_roadmap    roadmap_migrate's rule (ANTS-3757 § 2.2): the file
                  directly in the root whose name case-folds to roadmap.md.
                  Not recursive; a docs/ROADMAP.md is not a candidate.
  layout_roadmap  project_layout's rule: `.ants/project.json`'s `roadmap`
                  key (ANTS-2160), then root_roadmap in each of
                  kRoadmapCandidates' folders (src/projectlayoutengine.cpp),
                  in its order (ANTS-4409).

The name matches case-INSENSITIVELY: RetroDB names its roadmap `roadmap.md`,
and an uppercase-only probe once left it out of a survey's figures.
"""
import json
import os

# kRoadmapCandidates' folders, in its order.
ROADMAP_DIRS = ("", "docs/private", "docs/internal", "docs/fork")


def root_roadmap(root):
    """The roadmap directly in `root`, as a str, or None."""
    try:
        entries = sorted(os.listdir(root))
    except OSError:
        return None
    for entry in entries:
        path = os.path.join(root, entry)
        if entry.lower() == "roadmap.md" and os.path.isfile(path):
            return path
    return None


def layout_roadmap(directory):
    """The roadmap project_layout would report for `directory`, or None."""
    try:
        with open(os.path.join(directory, ".ants", "project.json"),
                  encoding="utf-8") as f:
            declared = json.load(f).get("roadmap")
    except (OSError, ValueError, AttributeError):
        declared = None
    if isinstance(declared, str) and declared:
        path = os.path.join(directory, declared)
        if os.path.isfile(path):
            return path
    for sub in ROADMAP_DIRS:
        found = root_roadmap(os.path.join(directory, sub))
        if found:
            return found
    return None
