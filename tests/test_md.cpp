// Test unitaire du decoupage markdown (M3a) — poc/Markdown.cpp, fonction libre
// markdownBlocks().
//
// Ce qu'on verifie : le decoupage a la source (fence, titre, liste, cases,
// citation, tableau, filet, paragraphe), et le fait qu'un bloc ne mange jamais
// son voisin. Le HTML produit par Qt n'est pas verifie ligne a ligne : le test
// le regarde (`--dump` sert a le lire) et n'en attend qu'un signature minimale.
//
// Build : link Markdown.cpp + Qt6::Gui (QTextDocument). Aucun hub, aucun corps.
// Run : ./test_md → exit 0 si tout passe.

#include "../poc/Markdown.h"

#include <QVariantList>
#include <QVariantMap>
#include <QString>
#include <QStringList>
#include <cstdio>

static int failures = 0;
static int passed = 0;

static void check(bool cond, const char *label)
{
    if (cond) {
        ++passed;
        std::printf("  [OK]   %s\n", label);
    } else {
        ++failures;
        std::printf("  [FAIL] %s\n", label);
    }
}

static QString field(const QVariantList &blocks, int index, const QString &key)
{
    if (index >= blocks.size())
        return QString();
    const QVariantMap block = blocks.at(index).toMap();
    return block.value(key).toString();
}

static QString kind(const QVariantList &blocks, int index)
{
    return field(blocks, index, QStringLiteral("kind"));
}

static bool mono(const QVariantList &blocks, int index)
{
    if (index >= blocks.size())
        return false;
    return blocks.at(index).toMap().value(QStringLiteral("mono")).toBool();
}

static int countOf(const QVariantList &blocks, const QString &wanted)
{
    int n = 0;
    for (const QVariant &value : blocks)
        if (value.toMap().value(QStringLiteral("kind")).toString() == wanted)
            ++n;
    return n;
}

int main(int argc, char **argv)
{
    std::printf("=== markdownBlocks self-test ===\n");

    // --- une reponse d'agent a plusieurs blocs ---
    {
        const QString md = QString::fromUtf8(
            "# Rapport\n"
            "\n"
            "Voici ce que j'ai trouve, en **deux** points.\n"
            "\n"
            "- le premier point\n"
            "- le second point\n"
            "\n"
            "```cpp\n"
            "int main() {\n"
            "    return 0;\n"
            "}\n"
            "```\n"
            "\n"
            "> une citation utile\n"
            "\n"
            "| cle | valeur |\n"
            "| --- | --- |\n"
            "| a | 1 |\n"
            "\n"
            "---\n"
            "\n"
            "- [ ] a faire\n"
            "- [x] fait\n"
            "\n"
            "La conclusion.\n");

        const QVariantList blocks = markdownBlocks(md);

        check(blocks.size() == 9, "9 blocs dans une reponse complete");
        check(kind(blocks, 0) == "heading", "titre en premier");
        check(field(blocks, 0, "level").toInt() == 1, "niveau du titre = 1");
        check(field(blocks, 0, "text") == "Rapport", "le titre perd ses #");
        check(kind(blocks, 1) == "para", "paragraphe");
        check(!field(blocks, 1, "html").contains(QStringLiteral("**"))
              && field(blocks, 1, "html").contains(QStringLiteral("font-weight")),
              "le gras est rendu, pas laisse en marques");
        check(kind(blocks, 2) == "list", "liste");
        check(kind(blocks, 3) == "code", "bloc de code");
        check(mono(blocks, 3), "le code est mono");
        check(field(blocks, 3, "text").split(QLatin1Char('\n')).size() == 3
              && field(blocks, 3, "text").contains(QStringLiteral("    return 0;")),
              "le code garde ses lignes et son indentation");
        check(!field(blocks, 3, "text").contains("```"), "les fences ne sont pas dans le code");
        check(kind(blocks, 4) == "quote", "citation");
        check(field(blocks, 4, "text") == "une citation utile", "la citation perd son >");
        check(kind(blocks, 5) == "table", "tableau");
        check(mono(blocks, 5), "le tableau est mono");
        check(field(blocks, 5, "text").split(QLatin1Char('\n')).size() == 3,
              "le tableau garde ses lignes alignees");
        check(kind(blocks, 6) == "rule", "filet");
        check(kind(blocks, 7) == "task", "liste de cases");
        check(field(blocks, 7, "text").contains(QStringLiteral("\u2610"))
              && field(blocks, 7, "text").contains(QStringLiteral("\u2611")),
              "les cases deviennent des glyphes");
        check(countOf(blocks, QStringLiteral("para")) == 2, "la fin revient en paragraphe");
    }

    // --- un paragraphe continue sur plusieurs lignes ne fait qu'un bloc ---
    {
        const QVariantList blocks = markdownBlocks(
            "une premiere ligne\nune deuxieme ligne\n\nune autre pensee");
        check(blocks.size() == 2, "deux pensees, deux blocs");
        check(field(blocks, 0, "text").contains(QStringLiteral("une deuxieme ligne")),
              "la continuation reste dans le meme paragraphe");
    }

    // --- un bloc de code peut contenir du markdown sans le declarer ---
    {
        const QVariantList blocks = markdownBlocks(
            "```md\n# pas un titre\n- pas une liste\n```\n\nvrai titre\n");
        check(blocks.size() == 2, "le fenced echappe au decoupage");
        check(mono(blocks, 0), "le fenced est rendu en mono");
        check(field(blocks, 0, "text").contains(QStringLiteral("# pas un titre")),
              "le contenu du fenced est intact");
        check(kind(blocks, 1) == "para", "le texte apres le fenced est libre");
    }

    // --- fenced jamais ferme ---
    {
        const QVariantList blocks = markdownBlocks("avant\n\n```\nl1\nl2");
        check(blocks.size() == 2, "un fenced ouvert va jusqu'au bout");
        check(field(blocks, 1, "text") == "l1\nl2", "le fenced ouvert rend ses lignes");
    }

    // --- les titres de tous niveaux, et un titre avec des marques ---
    {
        const QVariantList blocks = markdownBlocks(
            "#### Sous-titre **important**\n\ntexte");
        check(field(blocks, 0, "kind") == "heading"
              && field(blocks, 0, "level").toInt() == 4, "niveau 4 reconnu");
        check(field(blocks, 0, "text") == "Sous-titre important", "les marques du titre sautent");
    }

    // --- une liste alouee par une indentation reste une seule liste ---
    {
        const QVariantList blocks = markdownBlocks(
            "- un item\n   sa prolonge\n- un autre\n\nfin");
        check(countOf(blocks, QStringLiteral("list")) == 1, "la liste alouee ne se dedouble pas");
        check(field(blocks, 0, "text").contains(QStringLiteral("sa prolonge")),
              "la prolongation est gardee");
    }

    // --- filet : les trois ecritures ---
    {
        check(markdownBlocks("---").size() == 1 && kind(markdownBlocks("---"), 0) == "rule",
              "filet avec des tirets");
        check(kind(markdownBlocks("***"), 0) == "rule", "filet avec des asteriques");
        check(kind(markdownBlocks("_ _ _"), 0) == "rule", "filet espacement");
        check(kind(markdownBlocks("- un vrai item"), 0) == "list",
              "un item n'est pas un filet");
    }

    // --- CRLF, blanc, vide ---
    {
        check(markdownBlocks(QString::fromUtf8("a\r\n\r\nb\r\n")).size() == 2,
              "les fins de ligne Windows sont repliées");
        check(markdownBlocks("   \n\t\n  ").isEmpty(), "le blanc seul ne fait aucune bulle");
        check(markdownBlocks(QString()).isEmpty(), "rien ne fait rien");
    }

    // --- le code en ligne survit au parse (Qt le rendait en texte nu) ---
    {
        const QVariantList blocks = markdownBlocks("un `appel()` et du **gras**");
        check(field(blocks, 0, "html").contains(QStringLiteral("Ubuntu Mono")),
              "le code en ligne a son span");
        check(field(blocks, 0, "html").contains(QStringLiteral("appel()"))
              && !field(blocks, 0, "html").contains(QChar('`')),
              "le code en ligne garde son contenu, sans backticks");
    }

    // --- le html ne rend pas un document complet ---
    {
        const QVariantList blocks = markdownBlocks("un paragraphe");
        check(!field(blocks, 0, "html").contains(QStringLiteral("<!DOCTYPE"))
              && !field(blocks, 0, "html").contains(QStringLiteral("<body>")),
              "le html du bloc est un fragment, pas une page");
    }

    // --- une citation imbriquee et la citation de liste ---
    {
        const QVariantList blocks = markdownBlocks("> - un\n> - deux\n\napres");
        check(blocks.size() == 2, "la citation de liste est un seul bloc");
        check(field(blocks, 0, "kind") == "quote" && field(blocks, 1, "kind") == "para",
              "la citation ne s'etend pas au-dela du bloc");
    }

    if (argc > 1 && QString::fromLatin1(argv[1]) == QLatin1String("--dump")) {
        const QVariantList blocks = markdownBlocks(
            "du `code en ligne`, du **gras**, un [lien](https://exemple.fr)\n\n"
            "- item **un**\n- item `two`\n\n> citation **forte**");
        for (const QVariant &value : blocks) {
            const QVariantMap block = value.toMap();
            std::printf("--- bloc %s\n", qPrintable(block.value("kind").toString()));
            std::printf("html: %s\n", qPrintable(block.value("html").toString()));
        }
    }

    std::printf("=== %d passe(s), %d echec(s) ===\n", passed, failures);
    return failures ? 1 : 0;
}
