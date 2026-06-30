// Test unitaire de la hardline TerminalSafety.
//
// CE TEST NE LANCE JAMAIS LE TERMINAL. Il ne link pas TerminalTool.cpp, ne crée
// aucun QProcess : il appelle uniquement checkHardline() sur des chaînes de
// caractères. Physiquement incapable d'exécuter quoique ce soit sur le système.
//
// Build : l'executable `test_hardline` link uniquement TerminalSafety.cpp + Qt6::Core.
// Run   : ./test_hardline  →  exit 0 si tout passe, 1 si un cas échoue.

#include "../src/tools/TerminalSafety.h"

#include <QString>
#include <QStringList>
#include <cstdio>

struct Case {
    const char *command;
    bool shouldBlock;   // true = hardline doit bloquer, false = doit laisser passer
    const char *note;
};

static const Case kCases[] = {
    // --- Doivent être BLOQUÉS (hardline) ---
    {"rm -rf /",                          true,  "rm recursive on root"},
    {"rm -rf / home",                     true,  "rm recursive on root (suite après /)"},
    {"rm -r /",                           true,  "rm -r /"},
    {"sudo rm -rf /",                     true,  "sudo rm -rf /"},
    {"mkfs.ext4 /dev/sda",                true,  "mkfs"},
    {"mkfs",                              true,  "mkfs seul"},
    {"dd if=/dev/zero of=/dev/sda bs=1M", true,  "dd vers device brut"},
    {"dd of=/dev/nvme0n1",                true,  "dd vers nvme"},
    {"cat foo > /dev/sda",                true,  "redirect vers device brut"},
    {"echo x > /dev/sdb",                 true,  "redirect vers device brut"},
    {":(){ :|:& };:",                     true,  "fork bomb"},
    {"shutdown now",                      true,  "shutdown"},
    {"reboot",                            true,  "reboot"},
    {"halt",                              true,  "halt"},
    {"poweroff",                          true,  "poweroff"},
    {"kill -9 -1",                        true,  "kill -9 -1"},
    {"chmod -R 777 /etc",                 true,  "chmod system dir"},
    {"chmod --recursive 755 /usr/bin",    true,  "chmod system dir (--recursive)"},

    // --- Doivent PASSER (safe) ---
    {"ls -la ~",                          false, "listage"},
    {"find . -name foo",                  false, "find"},
    {"cat README.md",                     false, "cat"},
    {"git status",                        false, "git"},
    {"git log --oneline -5",              false, "git log"},
    {"echo hello",                        false, "echo inoffensif"},
    {"rm singlefile.txt",                 false, "rm simple (pas récursif, pas root)"},
    {"rm -rf /tmp/testdir",               false, "rm récursif mais pas root (dangerous, pas hardline)"},
    {"chmod 644 file.txt",                false, "chmod simple (pas -R)"},
    {"chmod -R 755 ./myproject",          false, "chmod -R mais pas system dir"},
    {"kill 1234",                         false, "kill simple (pas -9 -1)"},
    {"pkill -9 firefox",                  false, "pkill (pas kill -9 -1)"},
    {"make -j8",                          false, "make"},
    {"echo \"rm -rf /\"",                 false, "echo avec quotes : le / n'est pas suivi de \\s|$"},

    // --- Faux positif volontaire (conservateur) ---
    {"echo rm -rf /",                     true,  "echo SANS quotes : matche le pattern -> blocked (conservateur)"},
};

int main()
{
    int failures = 0;
    int passed = 0;
    const int total = int(sizeof(kCases) / sizeof(kCases[0]));

    std::printf("=== TerminalSafety hardline self-test (%d cas) ===\n", total);
    std::printf("Le terminal n'est JAMAIS appele dans ce test.\n\n");

    for (const Case &c : kCases) {
        const QString blocked = checkHardline(QString::fromLatin1(c.command));
        const bool isBlocked = !blocked.isEmpty();
        const bool ok = (isBlocked == c.shouldBlock);

        if (ok) {
            ++passed;
            std::printf("  [OK] %-40s %s\n", c.command,
                        c.shouldBlock ? "(blocked)" : "(allowed)");
        } else {
            ++failures;
            std::printf("  [FAIL] %-40s EXPECTED %s, GOT %s\n", c.command,
                        c.shouldBlock ? "blocked" : "allowed",
                        isBlocked ? "blocked" : "allowed");
            if (isBlocked) {
                std::printf("      -> %s\n", blocked.toUtf8().constData());
            }
            std::printf("      note: %s\n", c.note);
        }
    }

    std::printf("\n=== %d/%d passed, %d failed ===\n", passed, total, failures);
    return failures == 0 ? 0 : 1;
}