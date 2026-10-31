# Sécurité du terminal — Patterns, approval, protection

> Source : `~/.hermes/hermes-agent/tools/approval.py` (1812 lignes),
> `~/.hermes/hermes-agent/tools/terminal_tool.py`, `agent/file_safety.py`.
> Objectif : durcir `TerminalTool` d'A-Ice qui exécute actuellement `bash -c` dans
> `$HOME` **sans aucun garde-fou**.

## État actuel A-Ice

`src/tools/TerminalTool.cpp` :
- `bash -c <command>` dans `m_workdir` (défaut `$HOME`).
- Timeout 1s–120s (clamp).
- Output tronqué à 20000 chars.
- **Aucune détection de commande dangereuse.**
- **Aucun mécanisme d'approbation.**
- **Aucune protection des paths sensibles.**

Le modèle peut exécuter `rm -rf /`, `dd of=/dev/sda`, `curl … | sh`, `chmod -R 777 ~`,
écraser `~/.ssh/authorized_keys`, etc. **C'est le risque #1 du projet.**

---

## Le système Hermes (3 couches)

### Couche 1 — Hardline blocklist (non bypassable)

`HARDLINE_PATTERNS` (12 patterns) — bloqués **même en `--yolo`** :
```
rm -rf /, mkfs, dd if=… of=/dev/sd*, shutdown/reboot/halt/poweroff,
> /dev/sd*, fork bomb :(){ :|:& };:, chmod -R 777 /, curl|sh sur root…
```
Raisonnement : opter dans yolo = confiance sur tes fichiers/services, **pas** confiance
pour wiper le disque. Pas de recovery path → block inconditionnel.

### Couche 2 — Dangerous patterns (approve-once)

`DANGEROUS_PATTERNS` (47 patterns) — requirent approbation, bypassable en yolo :
- `rm -r`, `rm … /`, `chmod 777/666`, `chown -R root`, `mkfs`, `dd if=`, `> /dev/sd`
- SQL destructif : `DROP TABLE/DATABASE`, `DELETE FROM … sans WHERE`, `TRUNCATE`
- Services : `systemctl stop/restart/disable`, `kill -9 -1`, `pkill -9`, `killall -KILL`
- Shell exec : `bash -c`, `python -c`, `perl -e`, `curl|sh`, `bash <(curl …)`
- Écriture paths sensibles : `tee`/`>`/`cp`/`mv`/`install`/`sed -i` sur
  `~/.ssh/*`, `~/.netrc`, `~/.bashrc`, `~/.hermes/.env`, `/etc/*`
- Git destructif : `git reset --hard`, `git push --force`, `git clean -f`, `git branch -D`
- `xargs rm`, `find -exec rm`, `find -delete`
- Self-kill : `pkill hermes`, `kill $(pgrep …)`, `docker compose down`
- Sudo privilège : `sudo -S/--stdin/-s/-a/--askpass`

### Couche 3 — Smart approve (LLM auxiliaire)

`_smart_approve` : si un pattern dangerous matche, un **second LLM** (temperature 0)
juge si la commande est réellement dangereuse ou un faux positif (ex:
`python -c "print('hello')"` matche "script execution via -c flag" mais est inoffensif).
Retourne `approve` / `deny` / `escalate`.

### Couche 4 — Session allowlist

`is_approved(session_key, pattern_key)` : si l'utilisateur a déjà approuvé ce pattern
dans la session (ex: `rm -r` une fois), on ne re-demande pas. Persistance optionnelle
(`approve_permanent` + `save_permanent_allowlist`).

### Couche 5 — file_safety.py (paths protégés en écriture)

`build_write_denied_paths` : ensemble de paths **jamais** écrasables :
```
~/.ssh/authorized_keys, id_rsa, id_ed25519, config
~/.bashrc, ~/.zshrc, ~/.profile, ~/.bash_profile, ~/.zprofile
~/.netrc, ~/.pgpass, ~/.npmrc, ~/.pypirc, ~/.git-credentials
/etc/sudoers, /etc/passwd, /etc/shadow
~/.hermes/.env, ~/.hermes/.anthropic_oauth.json
```
`build_write_denied_prefixes` : dirs sensibles jamais écrasables :
```
~/.ssh/, ~/.aws/, ~/.gnupg/, ~/.kube/, /etc/sudoers.d/, /etc/systemd/,
~/.docker/, ~/.azure/, ~/.config/gh/, ~/.config/gcloud/
```

### Flow de décision (`check_dangerous_command`)

```
1. sandbox (docker/modal) ? → approve (le sandbox isole)
2. hardline match ? → BLOCK (non bypassable)
3. yolo mode ? → approve (bypass dangerous)
4. dangerous match ?
   5. déjà approuvé en session ? → approve
   6. contexte non-interactif (cron) ?
      - cron_mode=deny → BLOCK
      - sinon → auto-approve + log warning
   7. gateway/ask ? → submit_pending (UI demande)
   8. CLI interactif → prompt_dangerous_approval (y/N/session/always)
      - deny  → BLOCK + message "user denied, do NOT retry"
      - session → approve_session
      - always → approve_session + approve_permanent + save
```

---

## Recommandation pour A-Ice

### Niveau 1 — Hardline (non bypassable, ~15 lignes)

Liste de regex `QRegularExpression` compilées une fois. Avant `proc->start`, on scanne
la commande (lowercased). Match → on n'exécute pas, on retourne une erreur au modèle :

```cpp
static const QList<QPair<QRegularExpression, QString>> kHardline = {
    {QRegularExpression(R"(\brm\s+(-[^\s]*\s+)*-r?f?[^\s]*\s+/\b"), "rm recursive on root"},
    {QRegularExpression(R"(\bmkfs\b)"), "format filesystem"},
    {QRegularExpression(R"(\bdd\b.*\bof=/dev/(sd|nvme|hd|mmcblk|vd|xvd))"), "dd to raw device"},
    {QRegularExpression(R"(>\s*/dev/(sd|nvme|hd|mmcblk|vd|xvd))"), "redirect to raw device"},
    {QRegularExpression(R"(:\(\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;\s*:)"), "fork bomb"},
    {QRegularExpression(R"(\b(shutdown|reboot|halt|poweroff)\b)"), "system power off"},
    {QRegularExpression(R"(\bchmod\s+(-[^\s]*\s+)*-?R?\s*(777|666)\b)"), "world-writable perms"},
    // …
};
```

Retour au modèle (via `role="tool"`) :
```
BLOCKED (unrecoverable): <description>. This command is on the unconditional
blocklist. Do NOT retry or attempt variants. Find an alternative approach.
```

### Niveau 2 — Dangerous (approve manuel, ~30 lignes)

Patterns dangereux mais récupérables. À la première occurence dans la session →
**bulle d'approbation** dans l'UI A-Ice (un mini-dialog ou un bouton inline dans la
bulle "⚠️ `git reset --hard` — Allow? [y/N]"). Pas de smart-approve LLM (trop lourd).

Stockage : `QSet<QString> m_sessionApprovedPatterns` dans `ChatWidget`. Une fois
approuvé pour la session, on ne redemande pas.

Retour au modèle si refusé :
```
BLOCKED: User denied this potentially dangerous command (matched '<desc>').
Do NOT retry this command - the user has explicitly rejected it.
```

### Niveau 3 — Paths sensibles (write guard, ~20 lignes)

Avant d'exécuter, scanner la commande pour des targets sensibles. On peut se
contenter d'une liste de chemins absolus (résolu via `QStandardPaths` ou `QDir::homePath()`) :

```cpp
static const QStringList kDeniedTargets = {
    "~/.ssh/authorized_keys", "~/.ssh/id_rsa", "~/.ssh/config",
    "~/.bashrc", "~/.zshrc", "~/.profile",
    "~/.netrc", "~/.pgpass", "~/.npmrc",
    "/etc/sudoers", "/etc/passwd", "/etc/shadow",
};
```

Si la commande contient un de ces paths comme cible d'écriture (`>`, `>>`, `tee`,
`cp … <path>`, `mv … <path>`, `sed -i … <path>`) → bloquer.

C'est une approximation regex moins fine qu'Hermes (qui a des ancres `_COMMAND_TAIL`
pour ne matcher que la destination), mais suffisant pour A-Ice.

### Niveau 4 — Où placer la logique

Deux options :

**A. Dans `TerminalTool::execute`** — le tool se protège lui-même. Pro : c'est
localisé, le tool est responsable de sa sécurité. Con : l'UI ne sait pas qu'une
approbation est requise, donc pas de mini-dialog inline propre.

**B. Dans `ChatWidget::executeToolCalls` + un `ApprovalPolicy`** — avant d'appeler
`registry->execute`, on passe par le policy. Pro : l'UI peut afficher une bulle
d'approbation native. Con : sépare la sécurité de l'outil.

**Mon avis** : B pour l'approval (UI native), A pour le hardline (le tool ne doit
**jamais** exécuter un hardline, même appelé directement). Concrètement :
- `TerminalTool::execute` check hardline + paths sensibles → block direct.
- `ChatWidget::executeToolCalls` check dangerous → si match, demande approval UI,
  puis appelle le tool si approuvé.

### Niveau 5 — Interrupt propre (le bouton stop)

Hermes check `_interrupt_requested` **avant** chaque tool et injecte des `role="tool"`
"cancelled" pour les skipped. A-Ice : le bouton stop interrompt le stream réseau mais
**pas** un `QProcess` en cours. Il faut :
- Exposer les `QProcess` actifs (registry ou TerminalTool garde une liste).
- Au stop, `terminate()` tous les process en cours + append un `role="tool"` cancelled
  pour le tool_call_id en cours (sinon l'API rejette).

---

## Patterns à implémenter (version A-Ice minimale)

### Hardline (block inconditionnel)

| Pattern | Description |
|---|---|
| `\brm\s+(-[^\s]*\s+)*-?r?f?\s+/(\s|$)` | rm recursive on root path |
| `\bmkfs\b` | format filesystem |
| `\bdd\b.*\bof=/dev/(sd\|nvme\|hd\|mmcblk\|vd\|xvd)` | dd to raw block device |
| `>\s*/dev/(sd\|nvme\|hd\|mmcblk\|vd\|xvd)` | redirect to block device |
| `:\(\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;\s*:` | fork bomb |
| `\b(shutdown\|reboot\|halt\|poweroff)\b` | system power control |
| `\bkill\s+-9\s+-1\b` | kill all processes |
| `\bchmod\s+(-R\|--recursive\b).*\s+/(s?bin\|etc\|usr\|boot\|lib)` | chmod system dirs |

### Dangerous (approve-once par session)

| Pattern | Description |
|---|---|
| `\brm\s+-[^\s]*r` | recursive delete |
| `\brm\s+(-[^\s]*\s+)*/` | delete in root path |
| `\bchmod\s+(777\|666\|o\+w\|a\+w)` | world-writable perms |
| `\bchown\s+(-R\|--recursive).*\sroot\b` | recursive chown to root |
| `\b(bash\|sh\|zsh)\s+-[^\s]*c\b` | shell exec via -c |
| `\b(python[23]?\|perl\|ruby\|node)\s+-[ec]\s+` | script exec via -e/-c |
| `\b(curl\|wget)\b.*\|\s*(\|/\w+/)*/?(ba)?sh\b` | pipe to shell |
| `\bgit\s+reset\s+--hard\b` | git reset --hard |
| `\bgit\s+push\b.*(-f\|--force)\b` | git force push |
| `\bgit\s+clean\s+-[^\s]*f` | git clean force |
| `\bgit\s+branch\s+-D\b` | git branch force delete |
| `\bsystemctl\s+(stop\|restart\|disable\|mask)\b` | system service control |
| `\bpkill\s+-9\b` | force kill processes |
| `\bxargs\s+.*\brm\b` | xargs with rm |
| `\bfind\b.*-exec(?:dir)?\s+(/\S*/)?rm\b` | find -exec rm |
| `\bfind\b.*-delete\b` | find -delete |
| `\bsudo\b[^;&\|]*?\s+(-S\|--stdin\|-s\|-a\|--askpass)` | sudo privilege flag |
| `>>?\s*["']?~/.ssh/` | write to ssh dir |
| `>>?\s*["']?~/.bashrc["']?` | overwrite shell rc |
| `\bsed\s+-[^\s]*i` | in-place sed edit |
| `\b(cp\|mv\|install)\b.*\s~/.ssh/` | copy/move into ssh dir |
| `\b(cp\|mv\|install)\b.*\s~/.bashrc` | overwrite shell rc |
| `\bDROP\s+(TABLE\|DATABASE)\b` | SQL DROP |
| `\bDELETE\s+FROM\b(?![^\n]*\bWHERE\b)` | SQL DELETE without WHERE |
| `\bTRUNCATE\s+` | SQL TRUNCATE |
| `\bhermes\s+(gateway\|update)` | (n/a A-Ice — skip) |

### Paths protégés (write deny)

```
~/.ssh/authorized_keys, ~/.ssh/id_rsa, ~/.ssh/id_ed25519, ~/.ssh/config
~/.bashrc, ~/.zshrc, ~/.profile, ~/.bash_profile, ~/.zprofile
~/.netrc, ~/.pgpass, ~/.npmrc, ~/.pypirc, ~/.git-credentials
~/.config/a-ice/config.json, ~/.config/a-ice/SOUL.md   ← A-Ice local !
/etc/sudoers, /etc/passwd, /etc/shadow
```

On ajoute les fichiers de config d'A-Ice lui-même (sinon le modèle peut se
reconfigurer pour désactiver la sécurité — boucle classique d'agents).

---

## UX d'approbation dans A-Ice

Pas de modal QDialog (ça casserait le glassmorphisme). Option : une mini-bulle
inline dans la bulle en cours :

```
┌─────────────────────────────────────┐
│ 🔧 terminal: git reset --hard       │
│                                     │
│  ⚠️ git reset --hard                │
│  (destroys uncommitted changes)     │
│                                     │
│  [Allow once] [Allow session] [Deny]│
└─────────────────────────────────────┘
```

Tant que pas de réponse, l'exécution est en attente (le tool result n'est pas
appendu). Sur Deny → append `role="tool"` d'erreur. Sur Allow → exécuter + ajouter
le pattern à `m_sessionApproved`.

C'est du QML/Widget à coder dans `Bubble` (un état "pending approval" avec
boutons). ~50 lignes de UI.

---

## Ce qu'on n'emprunte PAS

- **Smart approve LLM** : trop lourd, latence, dépend d'un second endpoint. Les
  patterns + approval manuel suffisent pour un usage perso.
- **Sandbox docker/modal** : hors scope desktop overlay.
- **Tirith** (un autre système de règles d'Hermes, apparemment un policy engine
  séparé) : overkill.
- **Plugin hooks pre/post tool_call** : on n'a pas de système de plugins.
- **Cron mode / gateway mode / yolo mode** : contextes qui n'existent pas chez nous.