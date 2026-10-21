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
| Module voix | `src/voice/VoiceStream.h` (108 l.) + `src/voice/VoiceStream.cpp` (422 l.) |
| Branchements UI | `src/ChatWidget.cpp` → `setupVoice()` (appelée l.110, définie l.113), `/voice` → `handleVoiceCommand()` (l.805) |
| Source du flux texte | `src/llama_client.cpp` → signal `contentChunk` (émit l.375/482/494/501) |
| Tests (in-process, faux serveur) | `tests/test_voicestream.cpp` (445 l., 15 tests) → `QT_QPA_PLATFORM=offscreen build/test_voicestream` |
| Test protocole client ↔ serveur | `faster-qwen3-tts/test_protocol.py` (166 l.) — parle au **vrai** socket ; textes `silencieux` → synthétisés mais **pas joués** |
| Test du lecteur audio | `faster-qwen3-tts/test_lecture.py` (123 l.) — invariant « un seul ffplay vivant », process oubliés, trous audio (`Popen` simulé : **aucun son**) |
| Serveur TTS (Python) | `~/projects/ai/projects/faster-qwen3-tts/voix_serveur.py` (700 l.) — **repo git** depuis le 20/10 (branche `main`, commit racine `de3411a`) ; client CLI : `voix_client.py` |
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

**Chaque connexion a son thread** (`servir_connexion`, depuis `6363b9f`) : un client
connecté qui ne dit rien (a-ice ouvre puis remplace sa connexion avant d'écrire) ne
doit immobiliser personne. Séquentiel, il bloquait `recv` jusqu'au timeout de 30 s et
`ddvoice --statut` répondait « aucun serveur en mémoire » alors que le serveur tournait.

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
- Anti-dérive : avance de lecture mesurée (`_avance()` = `ecrit_s − écoulé + en_file_s`) ;
  `EN_AVANCE_MAX_S = 6.0` (marge max prise sur la lecture), `SEUIL_D_UN_COUP = 1200`
  (redécoupage côté serveur), `FILE_MAX_TEXTES = 8` (refus → `{"refus":"file pleine"}`).
- **Lecteur audio = un seul joueur à la fois** : `LECTEURS` (tous les ffplay non récoltés),
  `_recolter(lecteur, delai)` (ferme stdin, attend la durée **réellement** en instance,
  tue sinon), `_aucun_lecteur_vivant()` avant toute ouverture, `couper_lecture()` qui
  nettoie tout. Voir l'incident « voix superposées » ci-dessous.
- **Trous audio mesurés** (`SEUIL_TROU_S = 0.4`) : `TROU AUDIO x s` est journalisé quand
  l'avance est retombée à 0 (ffplay à sec) alors qu'un texte est en synthèse ET que le
  son du tour avait déjà commencé (`CADENCE["ecrit_s"] > 0`, sinon le TTFA normal du
  premier son serait compté comme un défaut). Compteurs exposés dans le statut :
  `trous`, `trous_audio_s`, `lecteurs_vivants`.
- Veille : extinction après 300 s d'inactivité (`--inactivite` ; env `DDVOICE_VEILLE`).
- Diagnostic live : `/run/user/1000/ddvoice.log` (toute requête entrante, bilans, bridages,
  trous) + `ddvoice.diag` + statut (`lecteurs_vivants`, `trous`, `avance_audio_s`).

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
  n'existe pas. 10 commits voix : `63fa4e4` (VoiceStream) → `99a65d2` (débit 18.5 + filet)
  → `421d7ed` (backpressure sur l'avance serveur) → `f9ad8e1` (faux serveur de test,
  `serverLost`, persistance `/voice`, `/voice off`) → `11e7df8` + `22a198e` (docs)
  → `fb4ab81` (`m_voice->reset()` à chaque tour) → `12c328c` (connexion neuve par requête)
  → `b42115f` (brief) → `a66f599` (découpe naturelle : paragraphe → phrase → virgule en secours)
  → `a785c8c` (brief). L'avance (`audioLead()`) ne pilote plus la taille des chunks.
- Serveur (`main`) : `de3411a` (racine) → `c599783` (multi-requêtes par connexion + journal
  fondé + `test_protocol.py`) → `4a01aff` (un seul lecteur ffplay + trous audio +
  `test_lecture.py`) → `8787fcd` (trou = après le début du tour) → `6363b9f` (un thread
  par connexion) → `7ecd6c3` (**verrou d'instance `flock`** + `test_instance.py` +
  `probe_ffplay.py` + trou audio journalisé avec son état complet).
- **Aucun `.bak` ne subsiste** dans les deux repos (vérifié le 21/10, `find`) : la question
  des blobs intermédiaires est close, `git diff` suffit désormais.
- Côté mémoire de l'assistant : la version canonique vit dans
  `~/.config/a-ice/memory/projects/voix-streaming-aice.md` (`master`), **resynchronisée juste
  après chaque engagement de ce miroir** avec `sync-canonique.py` (tête de la mémoire +
  corps de ce fichier) → corps identiques. ⚠️ Ne jamais `git commit -a` dans
  `~/.config/a-ice` : d'autres fichiers modifiés y sont les mises à jour propres de
  l'assistant, pas les nôtres.
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

## Incident du 21/10 (2) — « charabia, des milliers de personnes qui veulent parler »
Symptôme : voix **superposées**/cacophonie pendant une longue réponse.

**Cause : un ffplay orphelin, puis un deuxième par-dessus.** À la fin d'un texte,
`boucle_audio` refermait le lecteur avec `lecteur.wait(timeout=5)` alors que jusqu'à
`EN_AVANCE_MAX_S = 6.0` s d'audio étaient **déjà écrites** dans le buffer interne de
ffplay : le délai expirait, `LECTEUR = None` alors que le process jouait encore (et
s'orphelinait en zombie — `[ffplay] <defunct>` observé). Le texte suivant ouvrait un
**deuxième** ffplay : les deux voix se marchaient dessus. Chaque trou de file ajoutait
son orphelin → plus la réponse était longue, plus il y avait de voix simultanées.

- Corrigé par `4a01aff` (`_recolter` + `LECTEURS` + `_aucun_lecteur_vivant`) et affiné par
  `8787fcd` (le trou exige que le son du tour ait déjà commencé).
- Prouvé par `test_lecture.py` : **avec l'ancien code → 1 process oublié vivant +
  2 lecteurs simultanés** ; avec le correctif → pic à 1, aucun oubli, trou de 1,6 s mesuré.
- Vérifié en réel : 4 phrases espacées, `ffplay` échantillonné chaque seconde → **1 partout**.

## Incident du 21/10 (3) — `ddvoice --statut` affirmait « aucun serveur » (mensonge)
Le serveur traitait les connexions **une par une** dans sa boucle principale. Un client
resté connecté sans rien dire (a-ice : `ensureConnected()` ouvre, puis `connexionFraiche()`
remplace la connexion avant d'écrire) bloquait `recv` jusqu'au timeout de 30 s : plus
aucune autre connexion n'était servie — `--statut`, `--stop`, un autre client.

- Corrigé par `6363b9f` : `servir_connexion()` dans un **thread daemon par connexion**,
  extinction via `ARRET = threading.Event()`.
- Prouvé par la nouvelle vérification de `test_protocol.py` : client muet ouvert + statut
  demandé 2 s après → **aucune réponse avant le correctif**, réponse service après.
- Leçon de diagnostic : **un CLI qui dit « aucun serveur » ne prouve pas l'absence de
  serveur**. `cat /run/user/1000/ddvoice.pid` + `tail /run/user/1000/ddvoice.log` tranchent.

## Incident du 21/10 (4) — DEUX SERVEURS en parallèle : la vraie cause du « charabia »
Dans le journal de la session suivante (01:37 → 01:52), **après** les correctifs
`4a01aff`/`6363b9f` : « modèle 0.6 B chargé en 8.7 s » apparaît **deux fois à la même
seconde** (01:37:35), idem « inactif depuis 300 s → veille, extinction » (01:42:37), alors
qu'aux autres démarrages ces lignes sont uniques. Un processus n'écrit pas une ligne deux
fois : **deux instances de `voix_serveur.py` tournaient ensemble**, chacune avec son modèle,
son thread audio et son ffplay → deux voix qui se marchent dessus, exactement « des milliers
de personnes qui veulent parler ».

**Cause : la garde anti-doublon laissait une fenêtre de ~9 s.** `principal()` demandait
« qui répond sur `ddvoice.sock` ? » (timeout 2 s), **puis** chargeait le modèle (~9 s),
**puis** `os.unlink(socket)` + `bind()`. Deux instances lancées ensemble (auto-spawn d'a-ice
+ un `ddvoice` qui croit le serveur absent) passent la garde toutes les deux, chargent en
parallèle, et la dernière venue vole le socket en le `unlink`ant : les deux restent vivantes.

Corrigé par `7ecd6c3` : `prendre_instance()` pose un **`flock` exclusif sur
`$XDG_RUNTIME_DIR/ddvoice.lock` AVANT toute charge lourde** (et y écrit le PID), relâché à
l'extinction. Le verrou tombe tout seul si le porteur est tué de force → pas de blocage
éternel après un crash. `test_instance.py` teste l'invariant et non l'écriture : place prise
→ seconde instance refusée → place reprise après libération → place d'un serveur vivant
d'un autre PID non volable, puis libérée par sa mort brutale. Vérifié en réel contre le
serveur en place : refus en **moins d'une seconde**, avant tout chargement —
`serveur déjà vivant (pid 254959) → abandon : jamais deux serveurs`.

Mesure faite au passage (`probe_ffplay.py` : 20 s de **silence numérique** — échantillons à
zéro — envoyés avec les flags exacts du serveur, donc pipeline traversé sans rien faire
sortir) : écriture bornée à **14,45 s** par la backpressure et ffplay terminant après
**20,07 s** → **ffplay lit à 1× et n'avale rien** ; avec `-infbuf` l'écriture passe à 0,10 s
sans changer la durée (20,06 s). L'hypothèse « le lecteur consommerait plus vite que la
lecture » est mesurée fausse.

Ce qui reste ouvert : sur le run **unique** de 02:10, 49 s d'audio ont été produites en 30 s
de wall **sans qu'aucune `bride` ne se déclenche**, alors qu'un trou de 18,6 s était compté
dans la même fenêtre. Ces deux faits ne sont pas réconciliables avec une avance honnête.
Depuis `7ecd6c3`, chaque `TROU AUDIO` est journalisé avec son état complet (tailles des deux
files, `écrit s / écoulé s / en file s`, synthèse en cours, lecteur vivant ou fermé) : le
prochain tour de Dimitri tranchera avec des chiffres au lieu d'une hypothèse.

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
3. Lecteur audio : `.venv/bin/python test_lecture.py` → « Lecteur OK : un seul joueur à la
   fois, aucun process oublié, trous mesurés ».
4. Instance unique : `.venv/bin/python test_instance.py` → « Verrou d'instance OK : jamais
   deux serveurs ddVoice à la fois ». Le test réel, lui, est de relancer
   `.venv/bin/python voix_serveur.py` pendant qu'un serveur tient la place : il doit répondre
   « serveur déjà vivant (pid …) → abandon » en moins d'une seconde.
5. Le journal ne doit **jamais** contenir deux fois la même ligne de démarrage à la même
   seconde : c'est le révélateur de deux serveurs (§ incident 4).
6. `ddvoice --statut` → JSON avec `modele`, `attente`, `lecture`, `avance_audio_s`,
   `vram_go`, `lecteurs_vivants`, `trous`, `trous_audio_s`. ⚠️ **deux tirets** : `ddvoice
   statut` enfile le mot comme texte à dire. ⚠️ Si le CLI dit « aucun serveur », **vérifier
   dans le serveur** : `cat /run/user/1000/ddvoice.pid` puis `tail /run/user/1000/ddvoice.log`
   (ce CLI a menti le 21/10 : il n'était pas capable de servir deux clients).
7. Le log qui tranche : `tail -f /run/user/1000/ddvoice.log` doit montrer
   `requête recue : texte (N car)` → `enfilé #N` → `premier son en … ms`.
   **Si on n'y voit que des `statut`, le chunk n'a jamais atteint le serveur** : problème
   côté client. S'il y a `TROU AUDIO` et/ou plusieurs `lecteurs_vivants`, problème côté
   serveur (lecture/starvation), pas côté mixage.
8. Dans a-ice : `/voice on`, une question → du son en ~250-450 ms, `ddvoice.diag` dont le
   `id` avance, et `coupe paragraphe · N car` / `coupe phrase · N car` dans
   `QT_LOGGING_RULES=aice.voice.debug=true` (`coupe virgule (secours)` doit rester rare).
9. Rien ne sort **alors que** le log serveur montre `enfilé #N` + `premier son` : là
   seulement `ffplay qq.wav` hors pipeline puis mixer ALSA (§ incident 20/10).
10. Après toute modif de `VoiceStream.*` : **rebuild dans distrobox**, puis relancer 1.

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
18. **Un seul joueur audio, toujours** : toute fermeture de ffplay doit être suivie d'une
    récolte (attendre l'audio réellement en instance, sinon tuer). Un lecteur oublié vivant
    = une deuxième voix qui démarre par-dessus la première. Les `wait(timeout=…)` courts
    sont le piège : ils expirent pile quand le buffer du lecteur est plein.
19. **Un client peut bloquer un serveur séquentiel sans rien dire** : ne jamais servir les
    connexions une par une dans la boucle principale. Et un CLI qui prétend « aucun serveur
    en mémoire » n'est pas une mesure, c'est une interprétation.
20. **Ne pas confondre TTFA initial et trou audio** : avant le premier son d'un tour le
    lecteur attend ~0,3-0,5 s (le modèle calcule) — ce n'est pas un défaut. Le marqueur est
    `CADENCE["ecrit_s"] > 0` : le son de ce tour a déjà commencé.
21. **`wall_s` d'un bilan n'est PAS la vitesse du modèle** : il inclut les attentes de la
    bride (`_attendre_la_place`) et l'attente de la file. Pour mesurer le modèle, envoyer un
    texte `silencieux` (jamais bridé) : 300 car → ~6 s de wall pour ~15 s d'audio. Les
    « RTF ≈ 1 » lus dans les bilans de ce fichier sont de fausses lectures, corrigées le 21/10.
22. **Une ligne dupliquée dans `ddvoice.log` = deux processus**, pas un bug de journal :
    `journal()` n'est appelé qu'une fois par événement. Deux « modèle chargé » à la même
    seconde = deux serveurs (couper le doublon avant de chercher une cause audio).
23. **ffplay n'avale pas l'audio plus vite qu'il ne joue** : mesuré (20 s de silence → fin à
    20,07 s, écriture bridée à 14,45 s). Ne pas « corriger » la cadence en accusant le
    lecteur ; la backpressure du pipe est réelle et saine.

## Pistes restantes (non bloquantes)
- **Merger `voice` dans `main`** une fois validé à l'usage : les correctifs sont prouvés aux
  tests et aux instruments ; il reste l'approbation à l'oreille de Dimitri.
- Surveiller `trous` / `trous_audio_s` (statut) et `coupe virgule (secours)` (client). À ne
  pas confondre avec un problème de débit : mesuré le 21/10 sur des textes `silencieux`
  (donc sans bridage), le modèle sort **49 car/s** pour 19 car/s parlés → **~2,5 × le temps
  réel**. Il y a de la marge ; un trou vient de la cadence de la file, pas du GPU.
- Le compteur d'avance est grossier : `_avance() = ecrit_s − écoulé + en_file_s`, et quand
  le lecteur transfère un chunk de `en_file_s` vers `ecrit_s` la somme **ne change pas**.
  Ce n'est pas un bug (c'est un garde-fou conservateur), mais ça explique des `bride` qui
  traînent : les attendre n'est pas un défaut audible tant qu'il reste de l'audio en sortie.
- **Premier à relire après un trou** : la nouvelle ligne `TROU AUDIO … · textes en file : N ·
  audio en file : N · écrit X s en Y s · en file Z s · synthèse en cours : … · lecteur : …`.
  Les deux gros trous du run unique de 02:10 (18,6 s et 12,0 s) ont été comptés pendant que
  le modèle produisait 49 s d'audio en 30 s de wall **sans aucune `bride`** : ces chiffres
  diront enfin si c'est la file, le lecteur, ou la mesure qui est fautive.
- Creuser `ddvoice --eteindre` : n'a pas arrêté l'instance auto-spawnée par a-ice —
  **plausible maintenant que l'incident 4 est connu** : le socket avait été volé par une
  seconde instance, donc `--eteindre` parlait à un serveur qui n'était plus le propriétaire
  de tout. À revérifier avec le verrou d'instance en place.
- Eventuellement installer `sox` (annoncé « command not found » au chargement ; le pipeline
  utilisé ne s'en sert pas).
- Reprendre l'incident ALSA du 20/10 **seulement** si le log serveur montre `enfilé #N` +
  `premier son` sans son audible : les incidents (mixer / connexion perdue / voix
  superposées / client muet / deux serveurs) ont des signatures opposées et le log serveur
  tranche entre eux.
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
