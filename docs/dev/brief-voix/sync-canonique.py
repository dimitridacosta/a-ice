#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Miroir → version canonique du brief voix, SANS risque de perte.

Rien n'est tapé à la main, rien n'est repris du fichier canonique : la tête est
RECONSTRUITE par ce script (ligne 07 du brief : « on référence, on ne recopie pas »).
Le corps, lui, vient intégralement du miroir engagé dans git. Écrire la tête par un
slice d'indices était le piège : mal calibré, il a déjà fait perdre deux notes.

Usage :
  .venv-hote python docs/dev/brief-voix/sync-canonique.py            # écrire le canonique
  python3 docs/dev/brief-voix/sync-canonique.py --verifier           # ne rien écrire, comparer
"""
import argparse
import datetime
import difflib
import os
import re
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
MIROIR = os.path.join(BASE, "README.md")
CANONIQUE = os.path.expanduser("~/.config/a-ice/memory/projects/voix-streaming-aice.md")
ANCRE = "## Le pipeline en une phrase"   # premier titre du corps, identique des deux côtés

TETE = """---
date: 2026-10-20
maj: {maj}
type: projet
description: Brief de dev — pipeline voix streaming a-ice (VoiceStream C++ ↔ serveur ddvoice faster-qwen3-tts)
tags: [aice, voix, tts, ddvoice, streaming, brief-dev]
---

# Voix streaming a-ice — brief pour un agent de dev

> Ce fichier est la **version canonique** du brief. Miroir dans le repo client :
> `~/projects/ai/projects/a-ice/docs/dev/brief-voix/README.md`. Le corps est **repris
> intégralement du miroir à partir de l'ancre `## Le pipeline en une phrase`** : seuls les
> préambules diffèrent (volontairement), et cette tête est **produite par
> `docs/dev/brief-voix/sync-canonique.py`**, jamais tapée à la main. Contrôler avec
> `sync-canonique.py --verifier` (et non en lisant ce préambule). Le kit de 6 copies de code
> que contenait ce dossier miroir a été **supprimé le 20/10** : instantanés déjà périmés 30 min
> après création, et dangereux à éditer par erreur. Le code unique est dans `src/voice/`,
> `tests/` et `~/projects/ai/projects/faster-qwen3-tts/` — on **référence**, on ne recopie pas.

"""


def construire():
    with open(MIROIR, encoding="utf-8") as flux:
        miroir = flux.read()
    position = miroir.find(ANCRE)
    if position < 0:
        raise SystemExit(f"ancre {ANCRE!r} introuvable dans {MIROIR} : "
                         "le miroir a été restructuré, ne rien écrira à l'aveugle")
    tete = TETE.format(maj=datetime.date.today().isoformat())
    return tete + miroir[position:]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--verifier", action="store_true",
                    help="comparer sans écrire (sort 1 si le canonique diverge)")
    args = ap.parse_args()

    attendu = construire()
    existant = ""
    if os.path.exists(CANONIQUE):
        with open(CANONIQUE, encoding="utf-8") as flux:
            existant = flux.read()

    # `maj:` seul ne compte pas comme divergence : le brief vit, la date aussi.
    def hors_maj(texte):
        return re.sub(r"\nmaj: [0-9-]+", "", texte, count=1)

    if hors_maj(existant) == hors_maj(attendu):
        etat = "identique au miroir"
        code = 0
    else:
        divergences = list(difflib.unified_diff(
            existant.splitlines(), attendu.splitlines(),
            fromfile="canonique actuel", tofile="construit du miroir", lineterm="", n=2))
        etat = f"divergent ({len(divergences)} lignes de diff)"
        code = 1
        for ligne in divergences[:40]:
            print(ligne)

    if args.verifier:
        print(f"{CANONIQUE} : {etat}")
        return code

    # Un canonique qui rétrécit de plus de 40 % = ancre mal placée, pas une vraie mise à jour.
    if existant and len(attendu) < 0.6 * len(existant):
        print(f"REFUSÉ : {len(attendu)} octets attendus pour {len(existant)} existants "
              "(le corps aurait été perdu). Vérifier l'ancre.")
        return 1

    with open(CANONIQUE, "w", encoding="utf-8") as flux:
        flux.write(attendu)
    print(f"écrit : {CANONIQUE} ({len(attendu.splitlines())} lignes) — {etat}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
