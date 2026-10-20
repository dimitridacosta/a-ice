---
date: 2026-10-20
maj: 2026-10-21
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
| Module voix | `src/voice/VoiceStream.h` (108 l.) + `src/voice/VoiceStream.cpp` (431 l.) |
| Branchements UI | `src/ChatWidget.cpp` → `setupVoice()` (appelée l.110, définie l.113), `/voice` → `handleVoiceCommand()` (l.805) |
| Source du flux texte | `src/llama_client.cpp` → signal `contentChunk` (émit l.375/482/494/501) |
| Tests (in-process, faux serveur) | `tests/test_voicestream.cpp` (15 tests) → `QT_QPA_PLATFORM=offscreen build/test_voicestream` |
| Test protocole client ↔ serveur | `faster-qwen3-tts/test_protocol.py` — parle au **vrai** socket ; textes `silencieux` → synthétisés mais **pas joués** |
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

**Durée de vie de la connexion** — le point qui a coûté des heures : depuis `c599783`
une connexion peut porter **plusieurs** requêtes (le serveur lit ligne par ligne tant
que le client est vivant), mais `VoiceStream` ouvre **une connexion neuve par requête**
(`connexionFraiche()`), donc les deux styles sont servis. Ne jamais supposer que « la
connexion est encore ouverte » : voir l'incident ci-dessous.

## Logique VoiceStream (côté a-ice)
- **Découpe hiérarchique — naturelle d'abord** (choix assumé du 21/10, `a66f599`) :
  1. **fin de paragraphe** (ligne blanche, ou retour à la ligne en fin de buffer)
  2. **fin de phrase** (`. ! ? …` suivi d'un espace ou d'une fin)
  3. **virgule / `;` / `:`** — secours **seulement** si le texte s'empile au-delà de
     `SECOURS_A_PARTIR_DE = 400` car sans aucune limite naturelle
  4. **coupe dure à 500 car** — filet anti-blanc (bloc sans AUCUNE ponctuation : code,
     URL, énumération)
  Et si aucune limite naturelle n'existe encore sous 400 car : **on n'invente pas de
  coupe**, on attend le heartbeat (120 ms) ; `finish()` enverra le reste à la fin du tour.
  ⚠️ Ne pas « optimiser le TTFA » en recoupant plus petit : mesuré côté serveur, le
  `premier_son_s` est de **0,29 s pour 14 car comme de 0,259 s pour 1022 car** (le moteur
  synthétise en streaming intra-phrase). Couper à la virgule ne gagnait **rien** et
  faisait seulement redémarrer la prosodie du modèle à chaque morceau — le reproche de
  Dimitri (« ça sonne artificiel »). Chaque chunk = une reprise de synthèse = une couture.
- **Constantes** (bloc en tête de `VoiceStream.cpp`, + 2 locales à `flush()`) :
  `CHARS_PER_SECOND=18.5` (débit mesuré mymy), `MAX_CHUNK_CHARS=1100` (fenêtre de
  recherche ; le serveur redécoupe lui-même au-delà de 1200 = `SEUIL_D_UN_COUP`),
  `MIN_CHUNK_CHARS=24`, `FLUSH_INTERVAL_MS=120`, `SECOURS_A_PARTIR_DE=400`,
  `COUPE_DURE_A_PARTIR_DE=500`. Supprimés le 21/10 : `FIRST_CHUNK_MAX`, `LEAD_COMFORT_S`,
  `LEAD_LOW_S` — l'avance ne pilote plus la taille des chunks, elle reste la backpressure
  de `sendChunk` et la mesure affichée (`audioLead()`, `statusLine()`).
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

## Git / état (2026-10-21)
- Client : branche **`voice`** (HEAD), **non mergée dans `main`** — sur `main` ce pipeline
  n'existe pas. 8 commits voix : `63fa4e4` (VoiceStream) → `99a65d2` (débit 18.5 + filet)
  → `421d7ed` (backpressure sur l'avance serveur) → `f9ad8e1` (faux serveur de test,
  `serverLost`, persistance `/voice`, `/voice off`) → `11e7df8` + `22a198e` (docs)
  → `fb4ab81` (`m_voice->reset()` à chaque tour) → `12c328c` (connexion neuve par requête)
  → `b42115f` (brief) → `a66f599` (découpe naturelle : paragraphe → phrase → virgule en secours).
- Serveur : `faster-qwen3-tts` sur `main` = `de3411a` (commit racine) → `c599783`
  (multi-requêtes par connexion + journal fondé + `test_protocol.py`).
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

## Incident du 20→21/10 — voix silencieuse : le poll « statut » volait la connexion
Symptôme : `/voice on`, question posée → **rien d'audible**, alors que `ddvoice "texte"`
en CLI marchait (donc modèle, serveur, ffplay, casque sains) et que le texte arrivait
bien dans le stream. Cet incident **n'est pas** le précédent : ici le mixer était bon.

**Cause = protocole, pas audio.** `sendChunk()` fait un poll `statut` (backpressure,
ajouté par `421d7ed`) **puis écrit le `texte` sur la même connexion**. Or l'ancien
serveur clôt après chaque réponse : l'écriture part dans un socket déjà fermé. Côté
Qt, `write()` + `flush()` sur un socket fermé par le pair **ne lève aucune erreur** : le
chunk est compté « envoyé » (`m_serverConfirmed = true`, `chunkSent` émis) et la voix
disparaît en silence.

Preuves croisées relevées sur la machine (avant correctif) :
- serveur instrumenté : **51 `requête recue : statut`, zéro `texte`** sur la session ;
- client instrumenté (`QT_LOGGING_RULES=aice.voice.debug=true`) : `chunk 66 car ≈ 3,6 s`
  → l'écriture a bien eu lieu côté client ;
- `ddvoice.diag` figé à `id: 1` (un test CLI) → aucune synthèse demandée.

Corrections committées — client `12c328c`, serveur `c599783` :
1. `VoiceStream::connexionFraiche()` (déconnecte puis `ensureConnected()`) : chaque
   requête part sur une connexion neuve — avant l'écriture du texte, dans `serverStatus()`
   et dans `stop()`. Le client ne dépend plus du fait que le serveur garde la connexion.
2. Serveur : handler en **boucle ligne par ligne** (reliquat de `tampon` gardé) ; requête
   inachevée (client fermé avant le saut de ligne) ignorée proprement — l'ancien
   `tampon.split(b"\n", 1)[1]` levait `IndexError`.
3. Journal du serveur écrit **aussi** dans `$XDG_RUNTIME_DIR/ddvoice.log`, avec trace de
   toute requête entrante : ni a-ice ni `ddvoice` ne redirigent la sortie du process
   qu'ils spawnent → sans ça le serveur ne laisse aucune trace exploitable.
4. Logs de décision côté client (`pas de coupe · buffer N car · cible · avance · warm`,
   `finish · reste N car`) : sans eux, « la voix ne sort pas » est indistinguable de
   « le texte n'arrive jamais ».
5. Bug indépendant corrigé par `fb4ab81` : `m_voice->reset()` appelé avant
   `sendMessages()` dans `ChatWidget` — avant, il ne l'était que sur `/new`, donc un
   `m_streamBroken` collé condamnait toute la conversation (le commentaire du code
   disait déjà « Nouveau tour »).

**Ce que les tests masquaient** : le faux serveur lisait plusieurs requêtes par
connexion, contrairement au vrai → 10/10 verts alors que la production était muette.
`FakeServerThread` a gagné `setSingleShot()` (1 requête = 1 connexion, comme ddvoice) et
`statutPollDoesNotStealTheChunk()` **FAIL sans le correctif** (vérifié) / PASS avec.

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
1. `QT_QPA_PLATFORM=offscreen ./build/test_voicestream` → **`15 passed, 0 failed`**.
2. Protocole client ↔ serveur (serveur lancé) : `.venv/bin/python test_protocol.py` →
   « Protocole OK : multi-requêtes et mono-requête sont servis ».
3. `ddvoice --statut` → JSON avec `modele`, `attente`, `lecture`, `avance_audio_s`, `vram_go`.
   ⚠️ **deux tirets** : `ddvoice statut` enfile le mot comme texte à dire.
4. Le log qui tranche : `tail -f /run/user/1000/ddvoice.log` doit montrer
   `requête recue : texte (N car)` → `enfilé #N` → `premier son en … ms`.
   **Si on n'y voit que des `statut`, le chunk n'a jamais atteint le serveur** : le
   problème est côté client (c'est le diagnostic qui a clos l'incident du 21/10).
5. Dans a-ice : `/voice on`, une question → du son en ~250-450 ms et `ddvoice.diag` dont
   le `id` avance.
6. Rien ne sort **alors que** le log serveur montre `enfilé #N` + `premier son` : là
   seulement `ffplay qq.wav` hors pipeline puis mixer ALSA (§ incident 20/10).
7. Après toute modif de `VoiceStream.*` : **rebuild dans distrobox**, puis relancer 1.

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
9. **Un socket fermé ne dit rien à Qt** : `write()`/`flush()` sur un socket que le pair a
   fermé ne lève pas d'erreur visible. Distinguer « envoyé » (log client) de « reçu »
   (log serveur) : **croire le log serveur**.
10. **Jamais le vrai `ddvoice.sock` dans un test** : un `stop()` de test vidait la file de
    production et coupait le son en pleine conversation. Utiliser `cheminSansServeur()`
    (`QTemporaryDir` + `QLocalServer::removeServer`) et `setAutoSpawnEnabled(false)`.
11. Un faux serveur de test doit reproduire le **protocole de connexion** du vrai, pas
    seulement son JSON (`setSingleShot()` ; sinon tous les tests passent en production muette).
12. CLI : `--statut`, `--stop`, `--eteindre` avec **deux tirets**, sinon le mot est synthétisé.
13. Lancer a-ice à la main pour voir ses logs demande des **variables séparées** : écrire
    `QT_LOGGING_TO_CONSOLE=1QT_QPA_PLATFORM=wayland` (faute de frappe) désactive le logging
    → log de 0 octet. Utiles : `XDG_RUNTIME_DIR`, `DBUS_SESSION_BUS_ADDRESS`,
    `WAYLAND_DISPLAY`, `QT_QPA_PLATFORM=wayland`, `QT_LOGGING_RULES=aice.voice.debug=true`,
    stdout/err redirigés. `QT_LOGGING_TO_CONSOLE` est déprécié (→ `QT_FORCE_STDERR_LOGGING`).
14. `amixer` : carte et `numid` **changent au boot** → `amixer -c 0 controls | grep -i speaker`,
    jamais les numéros mémorisés. `asound.state` peut stocker 70,70 et WirePlumber remettre
    0 quand le jack est branché (normal) : le mixer n'était pas la cause du 21/10.
15. `ss -xpe | grep ddvoice` ne montre rien ici → préférer `/run/user/1000/ddvoice.log`.
16. Le 21/10 : `ddvoice --eteindre` n'a **pas** tué l'instance auto-spawnée par a-ice (il a
    fallu un `kill`) — anomalie non creusée. Au démarrage du serveur : `sox: command not
    found` (dépendance de secours absente), sans effet sur le pipeline utilisé.
17. **Chunk court ≠ voix rapide** : le `premier_son_s` du serveur est le même à 14 car et
    à 1022 car. Vouloir « gagner du TTFA » en coupant plus petit ne fait qu'ajouter des
    coutures de prosodie. La naturalité se juge à l'oreille, la latence se lit dans le log.

## Pistes restantes (non bloquantes)
- **Merger `voice` dans `main`** une fois la fonction validée à l'usage : le correctif
  `12c328c` est prouvé au test, il doit l'être en conversation réelle (entendre a-ice).
- Creuser `ddvoice --eteindre` : n'a pas arrêté l'instance auto-spawnée par a-ice.
- Eventuellement installer `sox` (annonce « command not found » au chargement ; le
  pipeline utilisé ne s'en sert pas).
- Reprendre l'incident ALSA du 20/10 **seulement** si le log serveur montre
  `enfilé #N` + `premier son` sans son audible : les deux incidents ont des signatures
  opposées, et le log serveur tranche entre eux.
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
