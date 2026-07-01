// Test unitaire de ChatMessage::toJson — repair historique (item 7).
//
// Vérifie que toJson() sérialise correctement les tool_calls et, surtout, qu'il
// répare silencieusement les `arguments` invalides (JSON tronqué/cassé) en les
// remplaçant par "{}" — sinon l'API OpenAI-compatible rejette tout le payload au
// prochain sendMessages.
//
// Build : link uniquement ChatMessage.cpp + Qt6::Core. Aucun terminal, aucun
// réseau. Run : ./test_chatmessage → exit 0 si tout passe.

#include "../src/ChatMessage.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
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

int main()
{
    std::printf("=== ChatMessage::toJson self-test ===\n");

    // --- Message user simple ---
    {
        ChatMessage m;
        m.role = "user";
        m.content = "hello";
        const QJsonObject o = m.toJson();
        check(o.value("role").toString() == "user", "user: role");
        check(o.value("content").toString() == "hello", "user: content");
        check(!o.contains("tool_calls"), "user: pas de tool_calls");
    }

    // --- Message tool (résultat) ---
    {
        ChatMessage m;
        m.role = "tool";
        m.toolCallId = "call_abc";
        m.toolName = "terminal";
        m.content = "[TOOL] ok";
        const QJsonObject o = m.toJson();
        check(o.value("role").toString() == "tool", "tool: role");
        check(o.value("tool_call_id").toString() == "call_abc", "tool: tool_call_id");
        check(o.value("name").toString() == "terminal", "tool: name");
        check(o.value("content").toString() == "[TOOL] ok", "tool: content");
    }

    // --- Assistant avec tool_calls valides ---
    {
        ChatMessage m;
        m.role = "assistant";
        m.content = "";
        ToolCall tc;
        tc.id = "call_1";
        tc.name = "terminal";
        tc.arguments = QStringLiteral("{\"command\":\"ls\"}");
        m.toolCalls.append(tc);

        const QJsonObject o = m.toJson();
        check(o.value("role").toString() == "assistant", "asst-valid: role");
        const QJsonArray calls = o.value("tool_calls").toArray();
        check(calls.size() == 1, "asst-valid: 1 tool_call");
        const QJsonObject fn = calls.at(0).toObject()
            .value("function").toObject();
        check(fn.value("name").toString() == "terminal", "asst-valid: name");
        check(fn.value("arguments").toString() == "{\"command\":\"ls\"}",
              "asst-valid: arguments intacts");
    }

    // --- REPAIR : arguments JSON invalide (tronqué) -> "{}" ---
    {
        ChatMessage m;
        m.role = "assistant";
        ToolCall tc;
        tc.id = "call_2";
        tc.name = "terminal";
        tc.arguments = QStringLiteral("{\"command\":\"ls"); // pas fermé
        m.toolCalls.append(tc);

        const QJsonObject o = m.toJson();
        const QJsonArray calls = o.value("tool_calls").toArray();
        const QString args = calls.at(0).toObject()
            .value("function").toObject()
            .value("arguments").toString();
        check(args == "{}", "repair: arguments tronques -> {}");
    }

    // --- REPAIR : arguments complètement cassés (pas du JSON) ---
    {
        ChatMessage m;
        m.role = "assistant";
        ToolCall tc;
        tc.id = "call_3";
        tc.name = "brave_search";
        tc.arguments = QStringLiteral("not json at all {");
        m.toolCalls.append(tc);

        const QJsonObject o = m.toJson();
        const QJsonArray calls = o.value("tool_calls").toArray();
        const QString args = calls.at(0).toObject()
            .value("function").toObject()
            .value("arguments").toString();
        check(args == "{}", "repair: arguments non-JSON -> {}");
    }

    // --- REPAIR : arguments vides -> "{}" (fromJson de "" est invalide) ---
    {
        ChatMessage m;
        m.role = "assistant";
        ToolCall tc;
        tc.id = "call_4";
        tc.name = "terminal";
        tc.arguments = QString(); // vide
        m.toolCalls.append(tc);

        const QJsonObject o = m.toJson();
        const QJsonArray calls = o.value("tool_calls").toArray();
        const QString args = calls.at(0).toObject()
            .value("function").toObject()
            .value("arguments").toString();
        check(args == "{}", "repair: arguments vides -> {}");
    }

    // --- REPAIR : mix valide + invalide dans le même message ---
    {
        ChatMessage m;
        m.role = "assistant";
        ToolCall a, b;
        a.id = "c1"; a.name = "terminal"; a.arguments = "{\"x\":1}";
        b.id = "c2"; b.name = "terminal"; b.arguments = "broken{";
        m.toolCalls.append(a);
        m.toolCalls.append(b);

        const QJsonObject o = m.toJson();
        const QJsonArray calls = o.value("tool_calls").toArray();
        check(calls.size() == 2, "repair-mix: 2 tool_calls");
        const QString a0 = calls.at(0).toObject().value("function").toObject()
            .value("arguments").toString();
        const QString a1 = calls.at(1).toObject().value("function").toObject()
            .value("arguments").toString();
        check(a0 == "{\"x\":1}", "repair-mix: args[0] intact");
        check(a1 == "{}", "repair-mix: args[1] réparé");
    }

    std::printf("    -> %d/%d passed\n", passed, passed + failures);
    std::printf("=== TOTAL: %d failure(s) ===\n", failures);
    return failures == 0 ? 0 : 1;
}