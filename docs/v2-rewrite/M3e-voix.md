---
date: 2026-11-02
type: note de chantier
branch: v2
---

# M3e — la voix streaming de la v1, reprise telle quelle

Le mot d'ordre etait **porter, pas reinventer** : `src/voice/VoiceStream.{h,cpp}` est
entre dans le POC sans etre retape. Seul le pont cote corps est nouveau. Note du vault
associee : `memory/projects/voix-streaming-aice.md`.

## 1. Ce qui est porte tel quel

Le module v1 (chunkur adaptatif + filtre « ce qui peut etre dit ») est compile dans la
cible `a-ice-m1` via `poc/CMakeLists.txt`, sans modification. Il apporte :

| Ce que tient le module | Ou |
|---|---|
| Protocole du serveur : `$XDG_RUNTIME_DIR` ou `/tmp` → `ddvoice.sock`, 1 ligne = 1 requete, le serveur **clot apres chaque reponse** (d'ou `connexionFraiche()`) | `connexionFraiche()`, `statusLine()` |
| `{"action":"statut"}` → `{lecture, attente, avance_audio_s}` ; chunk = `{"texte": …}` ; coupe = `{"action":"stop"}` | `sendChunk()`, `stop()` |
| Decoupe adaptatif : 18,5 car/s, `MIN_CHUNK_CHARS=24`, `MAX_CHUNK_CHARS=500`, flush 120 ms, verrou anti-avalanche 1 spawn / 30 s, spawn auto du serveur | constantes + `m_flushTimer` |
| Filtre prononçable : markdown (titres, puces, citations, tableaux, liens, balises, fences), **emoji et pictogrammes** (derive TTS mesuree), tirets longs → virgule ; l'etat `Filtre` survit aux decoupages | `nettoyerPourLaVoix(chunk, Filtre&)` |
| Reglage persistant : la meme cle que la v1 | `QSettings("a-ice","a-ice")`, `voice/enabled` |

Rien de ces constantes n'est change : `test_voicestream` (25 verifs) passe sans qu'une
seule de ses assertions ait du etre retouchee — c'est la preuve du portage.

## 2. Le pont neuf : `poc/VoiceControl.{h,cpp}`

Une couche mince QML ↔ `VoiceStream`, pensee pour ne rien decisionner :

- lit et ecrit **la meme cle que la v1** (`voice/enabled`) : monter la voix d'un cote la
  fait monter de l'autre ;
- `AICE_VOICE=0` : override non persiste (comme `AICE_SHADOW`), pour mesurer sans voix ;
- `AICE_VOICE_SOCKET=<chemin>` : knob de test qui reprend `VoiceStream::setSocketPath()`,
  permet de brancher un faux serveur sans toucher au vrai ;
- timer de 700 ms (`rafraichi`) : la carte du hub ne se redessine pas a chaque chunk ;
- `turnOn()`, `turnOff()`, `toggle()`, `resetBuffer()`, proprietes `on` et `status`
  (`status` retire le prefix `voice ` de `statusLine()` de la v1).

## 3. Le seuil : on ne vocalise que le tour qu'on a lance

C'est le seul point ou la v2 se comporte vraiment autrement qu'en v1, parce que le
corps vit avec un
hub et pas avec un fil unique. Dans `poc/main.cpp` :

```cpp
// chunkReceived → voiceStream.feed() seulement si hub.isGenerating() && role == "agent"
```

Sans ce seuil, la **reprise de session** (`session/load`) vocaliserait tout
l'historique rejoue, et les tours lances depuis **l'application du telephone** ou
l'agent de voix seraient lus alors qu'ils ne sortent pas de notre barre. Mesure : un
tour sur une session reprise recoit 59 chunks / 11 027 car de chunks rejoues, et
**un seul** part a la voix. Le seuil est ce qui rend la voix correcte en multi-client.

A la fin du tour : `turnEnded` → `stop()` si la raison est `cancelled` ou `error`
(couper net), sinon `finish()` (le dernier morceau doit partir, meme si le serveur
le refuse une premiere fois). `failed` (jonction perdue) → `stop()`.

## 4. `/voice` dans la barre

Ajoutee aux commandes que le corps rend lui-meme (`slashOwn()`), traitee par
`reglerVoix()` — meme grammaire que la v1 :

- `/voice on` · `/voice 1` · `/voice true` : monte la voix ;
- `/voice off` · `/voice 0` · `/voice false` : la coupe ;
- `/voice` seul : **bascule** ;
- couper **pendant** un tour en cours annule le tour (`hub.cancelTurn()`) : on ne paie
  pas la suite d'un tour qu'on vient de taire.

La carte du hub affiche une ligne `voix <status>` avec un bouton couper / monter, et
`resetConversation()` (nouvelle conversation) appelle `voice.resetBuffer()` : une
conversation qu'on vide ne doit pas laisser de texte en file.

Harnais de test : une entree `AICE_SLASH` prefixed par `@` est dispatchee comme une
vraie saisie de la barre — c'est la seule facon de tester `/voice off` en plein tour.

## 5. Mesures

Faux serveur `ddvoice` (meme protocole, journalise au lieu de parler) +
`a-ice-m1 --prompt … AICE_VOICE=1 AICE_VOICE_SOCKET=/tmp/ddvoice-fake.sock` :

- un tour de 350 mots rend **12 chunks** vocalises, prose seule : le markdown, le gras
  et le code en ligne (`print`) ont ete retires par le filtre, les tirets longs sont
  devenus des virgules, les guillemets francais ont tenu ;
- `/voice off` lance en plein tour : le journal du faux serveur recoit **`STOP`** apres
  le 12e chunk, et le tour du hub est annule ;
- le vrai ddvoice n'est **jamais** monte pendant ces tests (aucun spawn, aucun
  `ddvoice.sock` cree) — c'est le point de vigilance : le serveur charge Qwen3-TTS sur
  le GPU.

Reste a l'œil de Dimitri : le rythme reel (la voix part-elle trop tot ou trop vite),
et le rendu de la ligne `voix` dans la carte du hub.
