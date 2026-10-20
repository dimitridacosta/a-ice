---
date: 2026-10-20
type: projet
description: Brief de dev — pipeline voix streaming a-ice (VoiceStream C++ ↔ serveur ddvoice faster-qwen3-tts)
tags: [aice, voix, tts, ddvoice, streaming, brief-dev]
---

# Voix streaming a-ice — brief pour un agent de dev

> ⚠️ **Ce dossier ne contient plus de copies de code** (il en contenait 6 : instantanés
> de `VoiceStream.*`, `voix_serveur.py`, `ddvoice`, tests — déjà périmés 30 min après
> leur création, et dangereux à éditer par erreur). Le code source unique est dans
> `src/voice/`, `tests/` et `~/projects/ai/projects/faster-qwen3-tts/`. Ne recopie pas :
> référence.
> Version canonique (mémoire de l'assistant) :
> `~/.config/a-ice/memory/projects/voix-streaming-aice.md`.

## Le pipeline en une phrase
Le texte que l'LLM produit arrive par `LlamaClient::contentChunk` (SSE, jamais le
thinking) → `VoiceStream` (C++, dans a-ice) le découpe en chunks adaptatifs → envoyés
au **serveur ddvoice** (socket unix, Python/faster-qwen3-tts, CUDA graphs) → chunks PCM
joués par **ffplay** dès leur disponibilité (TTFA ~250-450 ms).

## Les deux repos

| Rôle | Chemin |
|---|---|
| Client a-ice (Qt6/C++17) | `~/projects/ai/projects/a-ice/` — repo git, travail voix sur la branche **`voice`** |
| Module voix | `src/voice/VoiceStream.h` (103 l.) + `src/voice/VoiceStream.cpp` (406 l.) |
| Branchements UI | `src/ChatWidget.cpp` → `setupVoice()` (appelée l.110, définie l.113), `/voice` → `handleVoiceCommand()` (l.805) |
| Source du flux texte | `src/llama_client.cpp` → signal `contentChunk` (émit l.375/482/494/501) |
| Tests | `tests/test_voicestream.cpp` (290 l.) → `QT_QPA_PLATFORM=offscreen build/test_voicestream` |
| Serveur TTS (Python) | `~/projects/ai/projects/faster-qwen3-tts/voix_serveur.py` — **repo git** depuis le 20/10 (branche `main`, commit racine `de3411a`) |
| CLI | `~/.local/bin/ddvoice` → symlink vers `faster-qwen3-tts/ddvoice` (enfile, statut, stop, extinction) |
| Venv serveur | `faster-qwen3-tts/.venv` (Python 3.12.13) : `qwen_tts_hf==0.1.1.post1`, `faster_qwen3_tts==0.5.4` — JAMAIS l'upstream `qwen-tts` |
| Voix de référence | `faster-qwen3-tts/refs/mymy_v1-0-24k.wav` (x-vector, `xvec_only=True`) |

⚠️ Les **numéros de ligne sont indicatifs** (état du 2026-10-20) : ils décalent dès
qu'on ajoute du code. S'orienter au **nom de fonction**.

## Protocole socket (`$XDG_RUNTIME_DIR/ddvoice.sock`, JSON 1 ligne/requête)
```
{"texte":"…", "sauver":"/tmp/x.wav", "silencieux":true} → {"enfile":n,"file":k}
{"action":"statut"} → {"modele":…,"attente":[…],"en_cours":…,"lecture":bool,"avance_audio_s":…,"vram_go":…}
{"action":"stop"}  → {"stop":true,"vides":n,"annule":bool}
{"action":"arret"} → extinction, VRAM libérée
```
Fichiers runtime : `ddvoice.sock`, `ddvoice.pid`, `ddvoice.log`, `ddvoice.diag`
(dernier bilan JSON : `premier_son_s`, `wall_s`, `audio_s`, `vram_go`).

## Logique VoiceStream (côté a-ice)
- **Chunker adaptatif** : 1ᵉʳ chunk agressif ≤120 car (coupe à la virgule → TTFA perçu) ;
  ensuite phrases puis paragraphes (`MAX_CHUNK_CHARS` = ≤1100 car ; le serveur redécoupe
  au-delà de 1200 = `SEUIL_D_UN_COUP`).
- **Constantes** (bloc en tête de `VoiceStream.cpp`, l.18-30) : `CHARS_PER_SECOND=18.5`
  (débit mesuré mymy), `LEAD_COMFORT_S=2.0`, `LEAD_LOW_S=0.8`, `MAX_CHUNK_CHARS=1100`,
  `FIRST_CHUNK_MAX=120`, `MIN_CHUNK_CHARS=24`, `FLUSH_INTERVAL_MS=120` (heartbeat).
  ⚠️ `EMERGENCY_CUT_CHARS=500` (filet anti-blanc sans ponctuation) est **local à la
  fonction de chunking** (l.164).
- **Backpressure** : lit `avance_audio_s` du serveur (mesure RÉELLE ; l'estimation locale
  dérive avec les tours d'outils). `queued ≥ 6` ou `avance > 10 s` → requeue local.
  Le **6 est une marge de sécurité sous `FILE_MAX_TEXTES = 8`** du serveur : être refusé
  côté client avant que le serveur ne refuse brutalement.
- **Auto-spawn** du serveur si socket absent (verrou anti-avalanche : 1 spawn / 30 s,
  réarmé à chaque tour, commit `421d7ed`).
- **Cycle de vie** : `feed()` par chunk SSE ; `finish()` à `responseComplete` ET à
  `toolCallsReady` (le son suit le contenu, pas les tools) ; `stop()` à `requestError`,
  `/voice off`, bouton Stop.
- **`/voice [on|off]`** dans la barre de prompt ; état persisté dans
  `~/.config/a-ice/a-ice.conf` → `[voice] enabled=…`.

## Côté serveur (`voix_serveur.py`)
- `Qwen/Qwen3-TTS-12Hz-0.6B-Base`, attention sdpa, warmup CUDA graphs (~9 s de
  chargement, ~2,5 Go VRAM, RTX 3070 laptop).
- `generate_voice_clone_streaming(chunk_size=N)` → chunks écrits dans ffplay
  (`-f s16le -ar 24000 -ch_layout mono` via stdin).
- Anti-dérive : avance de lecture mesurée ; `SEUIL_D_UN_COUP = 1200` (l.45) ;
  `FILE_MAX_TEXTES = 8` (l.51, refus → `{"refus":"file pleine"}`).
- Veille : extinction après 300 s d'inactivité (`--inactivite`, l.431 ; env `DDVOICE_VEILLE`).
- Diagnostic live : `/run/user/1000/ddvoice.log` + `ddvoice.diag`.

## Build (⚠️ distrobox obligatoire)
```bash
distrobox enter --name dev-aice     # Fedora 44 (KF6/Qt6) — ne pas builder sur l'hôte Bazzite
cd ~/projects/ai/projects/a-ice
cmake -S . -B build && cmake --build build -j
QT_QPA_PLATFORM=offscreen ./build/test_voicestream
```
Vérifié : `cmake --build build` **sur l'hôte échoue** (ECM / Extra CMake Modules absent)
→ contrainte réelle, pas une préférence.

## Git / état (2026-10-20)
- Branche **`voice`** (HEAD), **non mergée dans `main`** : sur `main` ce pipeline n'existe pas.
- 4 commits voix : `63fa4e4` (VoiceStream) → `99a65d2` (débit 18.5 + filet) →
  `421d7ed` (backpressure sur l'avance serveur) → `f9ad8e1` (faux serveur de test,
  `serverLost`, persistance `/voice`, condition `/voice off`).
- Untracked en sus : `src/voice/VoiceStream.cpp.bak-20261020` — ⚠️ **il diffère des blobs
  git `99a65d2` ET `421d7ed`** : il contient un état intermédiaire qui n'est nulle part
  dans git. Ne pas supprimer à la légère.
- Le 20/10, le kit de code de ce dossier a été supprimé (6 fichiers, déjà périmés).

## Incident du 20/10 — « plus de son » alors que ddvoice marchait
- Le serveur synthétisait (premier son 258 ms) mais **rien n'était audible**.
- Cause : **Realtek ALC256, haut-parleurs coupés matériellement** —
  `numid=4 'Speaker Playback Switch' = off`, `numid=3 'Speaker Playback Volume' = 0`.
- Réparé : `amixer -c 0 cset numid=4 1,1 && amixer -c 0 cset numid=3 70,70`.
- Réflexe à garder : **si le son TTS ne sort pas, vérifier le mixer ALSA avant de
  suspecter VoiceStream** ; tester `ffplay qq.wav` hors pipeline.
- Le son sortait par le casque (numid=14 « Headphone Jack » = on) : le port actif
  bascule HP/speaker selon le branchement.

## Points morts réglés le 2026-10-20 (commit `f9ad8e1`, tests 10/10)
- **Tests du chunking réel** : faux serveur in-process (`FakeServerThread`, `QLocalServer`
  dans SA PROPRE thread — sinon deadlock avec les `waitForReadyRead` bloquants).
  Couvre : coupe ponctuation, coupe virgule à froid, backpressure (file pleine →
  requeue local), serveur mort en plein stream. `VoiceStream` a gagné `setSocketPath()`
  + `setAutoSpawnEnabled(false)` pour les tests.
- **Avance** : `avance_audio_s` du serveur = mesure PRINCIPALE. `m_estSecondsQueued`
  rétrogradé en simple FALLBACK (serveur injoignable / JSON sans avance). Doc dans `audioLead()`.
- **`/voice off`** : condition clarifiée → `if (m_isTyping) onStopClicked();` (l'ancienne
  `m_isTyping || m_interruptRequested == false` était tordue).
- **Serveur mort en plein stream** : `m_serverConfirmed` (≥1 envoi reçu) +
  `handleServerUnreachable()` → confirmé puis injoignable = MORT → `m_streamBroken`,
  buffer vidé, `feed()` n'accumule plus, signal `serverLost()` (ChatWidget → note système).
  Distinction « mort » vs « pas encore démarré » (requeue conservé). `statusLine` affiche « perdu ».
- **Persistance `/voice on`** : `QSettings("a-ice","a-ice")` → clé `voice/enabled`, écrite
  dans `handleVoiceCommand`, relue dans `setupVoice()`.

## Validation (comment savoir que le pipeline est vivant)
1. `QT_QPA_PLATFORM=offscreen ./build/test_voicestream` → `10 passed, 0 failed`.
2. `ddvoice statut` → JSON avec `modele`, `attente`, `lecture`, `avance_audio_s`, `vram_go`.
3. Dans a-ice : `/voice on`, poser une question → du son en ~250-450 ms.
4. Rien ne sort : `ffplay qq.wav` **hors pipeline**, puis mixer ALSA (§ incident).
5. Après toute modif de `VoiceStream.*` : **rebuild dans distrobox**, puis relancer 1.

## Pièges
1. **Son inaudible ≠ bug VoiceStream** : commencer par le mixer ALSA.
2. `VoiceStream` a des `waitForReadyRead` **bloquants** : tout faux serveur de test doit
   vivre dans son propre thread (`FakeServerThread`), sinon deadlock.
3. **Ne pas builder sur l'hôte Bazzite** (ECM absent).
4. **Ne jamais installer l'upstream `qwen-tts`** : le venv pointe sur `qwen_tts_hf`.
5. `faster-qwen3-tts` est **versionné depuis le 20/10** (git init, `.gitignore` :
   `.venv/`, `out/`, `refs/`, `*.log`, `*.bak-*`). Les `.bak` de `voix_serveur.py`
   ont été archivés sur la branche `archive/pre-antiderive` puis supprimés.
   Les `.bak` ne sont plus nécessaires : penser `git diff` avant toute modif.
6. **Un `stop()` n'est pas une mort serveur** : `stop()` neutralise `m_serverConfirmed`
   pour ne pas émettre `serverLost()` à tort.
7. Croire `avance_audio_s` (mesure réelle), pas l'estimation locale du temps audio.
8. Ne pas recréer de copies de code dans `docs/` : elles divergent en quelques heures.

## Pistes restantes (non bloquantes)
- **Merger `voice` dans `main`** une fois la fonction validée à l'usage.
- Reprise VRAIE (re-synthèse) si le serveur meurt en plein stream : aujourd'hui on
  abandonne le tour proprement sans rejouer le texte perdu.
- Faux serveur de test : ne simule pas encore l'auto-spawn (coupé en test).
- ~~Versionner `faster-qwen3-tts`~~ **fait le 20/10** : git init, commit racine
  `de3411a`, `.bak` archivés sur `archive/pre-antiderive`.
- Regarder `VoiceStream.cpp.bak-20261020` (état intermédiaire hors git) puis le supprimer
  ou le commit quelque part.

## Voir aussi
- `~/.config/a-ice/memory/projects/voix-streaming-aice.md` (version canonique)
- `~/.config/a-ice/memory/projects/faster-qwen3-tts.md` (moteur TTS)
- `~/.config/a-ice/memory/projects/breeze-tts2.md` (ancien moteur)
- `~/.config/a-ice/memory/projects/a-ice.md` · `~/.config/a-ice/memory/index.md`
