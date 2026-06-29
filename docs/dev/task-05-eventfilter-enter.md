# Tâche 05 — Implémenter `eventFilter` pour Enter / Shift+Enter

**Statut :** ⬜ A faire
**Priorité :** 🟠 Moyenne (fonctionnel)
**Temps estimé :** ~20 min

## Description

Le code appelle `m_messageTextEdit->installEventFilter(this)` dans `setupUI()`, mais aucune méthode `eventFilter()` n'est définie dans la classe. Le comportement attendu :
- **Enter** → envoyer le message
- **Shift+Enter / Ctrl+Enter** → saut de ligne (déjà géré par défaut avec `TabPerIndent`)

## Contexte technique

- **Fichiers concernés :** `src/ChatWidget.h` et `src/ChatWidget.cpp`

## Critères d'acceptation

- [ ] Ajouter la déclaration `bool eventFilter(QObject *watched, QEvent *event)` dans ChatWidget
- [ ] Détecter le QTextEdit + touche Enter → appeler `onSendClicked()` et retourner `true` (consommer l'événement)
- [ ] Shift/Return sur le QTextEdit → saut de ligne non intercepté (retour par défaut)
- [ ] Tester : frapper Enter dans le champ texte → message envoyé
- [ ] Tester : Shift+Enter → nouvelle ligne insérée

## Notes / Implémentation hint

Exemple minimal d'eventFilter Qt6 compatible :

```cpp
bool ChatWidget::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_messageTextEdit && event->type() == QEvent::KeyPress) {
        auto ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
            if (!(ke->modifiers() & Qt::ShiftModifier)) {
                onSendClicked();
                return true;  // consomme l'événement → pas de saut de ligne
            }
        }
    }
    return QWidget::eventFilter(watched, event);  // laisser le default faire son boulot
}
```

Ne pas oublier d'inclure `<QKeyEvent>`.
