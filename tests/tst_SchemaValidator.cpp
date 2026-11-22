// Unit tests for the JSON-schema subset used by tool definitions
// (AiSchemaValidator::validate). Pure JSON: needs no engine.

#include "TestHarness.h"
#include "ai/AiSchemaValidator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace OpenDaw;

namespace {

QJsonObject parseObject(const char* json)
{
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(QByteArray(json), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        qFatal("bad JSON in test: %s (%s)", json, qPrintable(err.errorString()));
    return doc.object();
}

QJsonValue parseValue(const char* json)
{
    // Wrap so scalars parse too.
    const QByteArray wrapped = QByteArray("{\"v\":") + json + "}";
    return parseObject(wrapped.constData()).value("v");
}

// The kind of input_schema tools use: a track ref (oneOf), numbers with
// bounds, an enum, an array of note objects.
QJsonObject toolSchema()
{
    return parseObject(R"({
        "type": "object",
        "properties": {
            "track":  {"oneOf": [{"type": "string"}, {"type": "integer"}]},
            "volume_db": {"type": "number", "minimum": -60, "maximum": 6},
            "count":  {"type": "integer", "minimum": 1, "maximum": 16},
            "mode":   {"type": "string", "enum": ["mix", "stems"]},
            "muted":  {"type": "boolean"},
            "notes": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "note": {"type": "integer", "minimum": 0, "maximum": 127},
                        "start_beat": {"type": "number"},
                        "length_beats": {"type": "number"}
                    },
                    "required": ["note", "start_beat", "length_beats"]
                }
            },
            "options": {
                "type": "object",
                "properties": {
                    "tags": {"type": "array", "items": {"type": "string"}}
                }
            }
        },
        "required": ["track"]
    })");
}

std::optional<AiValidationError> check(const char* json)
{
    return AiSchemaValidator::validate(parseValue(json), toolSchema());
}

QString describe(const std::optional<AiValidationError>& e)
{
    return e ? QString("%1: %2").arg(e->path, e->message) : QString("<valid>");
}

} // namespace

class TestSchemaValidator : public QObject {
    Q_OBJECT

private slots:
    void validMinimal()
    {
        const auto e = check(R"({"track": "Drums"})");
        QVERIFY2(!e, qPrintable(describe(e)));
    }

    void validFull()
    {
        const auto e = check(R"({
            "track": 2, "volume_db": -3.5, "count": 4, "mode": "stems", "muted": true,
            "notes": [{"note": 60, "start_beat": 0, "length_beats": 0.5},
                      {"note": 64, "start_beat": 1.5, "length_beats": 1}],
            "options": {"tags": ["a", "b"]}
        })");
        QVERIFY2(!e, qPrintable(describe(e)));
    }

    void rootMustBeObject()
    {
        QVERIFY(check(R"([1, 2])"));
        QVERIFY(check(R"("track")"));
        QVERIFY(check(R"(null)"));
    }

    void wrongTypes_data()
    {
        QTest::addColumn<QString>("json");
        QTest::addColumn<QString>("key");
        QTest::newRow("number as string") << R"({"track": 0, "volume_db": "-3"})" << "volume_db";
        QTest::newRow("boolean as number") << R"({"track": 0, "muted": 1})" << "muted";
        QTest::newRow("string as bool") << R"({"track": 0, "mode": true})" << "mode";
        QTest::newRow("array as object") << R"({"track": 0, "notes": {"note": 1}})" << "notes";
        QTest::newRow("object as array") << R"({"track": 0, "options": []})" << "options";
        QTest::newRow("null value") << R"({"track": 0, "count": null})" << "count";
    }

    void wrongTypes()
    {
        QFETCH(QString, json);
        QFETCH(QString, key);
        const auto e = check(json.toUtf8().constData());
        QVERIFY2(e, qPrintable("accepted: " + json));
        QVERIFY2(e->path.startsWith("$"), qPrintable(e->path));
        QVERIFY2(e->path.contains(key), qPrintable(describe(e)));
        QVERIFY(!e->message.isEmpty());
    }

    void integerVsFractional()
    {
        QVERIFY2(!check(R"({"track": 0, "count": 3})"), "integer rejected");
        // 3.0 is the same JSON number as 3.
        QVERIFY2(!check(R"({"track": 0, "count": 3.0})"), "3.0 rejected as integer");
        const auto e = check(R"({"track": 0, "count": 2.5})");
        QVERIFY2(e, "2.5 accepted as integer");
        QVERIFY(e->path.contains("count"));
        // An integer is a valid "number".
        QVERIFY(!check(R"({"track": 0, "volume_db": -6})"));
        // A fractional track index matches neither oneOf branch.
        QVERIFY(check(R"({"track": 1.5})"));
    }

    void required()
    {
        const auto e = check(R"({"volume_db": 0})");
        QVERIFY2(e, "missing required 'track' accepted");
        QVERIFY2(e->message.contains("track") || e->path.contains("track"), qPrintable(describe(e)));

        const auto n = check(R"({"track": 0, "notes": [{"note": 60, "start_beat": 0}]})");
        QVERIFY2(n, "missing nested required 'length_beats' accepted");
        QVERIFY2(n->path.contains("notes"), qPrintable(describe(n)));
        QVERIFY2(n->message.contains("length_beats") || n->path.contains("length_beats"),
                 qPrintable(describe(n)));
    }

    void unknownPropertyRejected()
    {
        const auto e = check(R"({"track": 0, "volumedb": -3})");
        QVERIFY2(e, "unknown property accepted");
        QVERIFY2(e->path.contains("volumedb") || e->message.contains("volumedb"), qPrintable(describe(e)));

        const auto n = check(R"({"track": 0, "notes": [{"note": 1, "start_beat": 0, "length_beats": 1, "vel": 3}]})");
        QVERIFY2(n, "unknown nested property accepted");
        QVERIFY2(n->path.contains("notes"), qPrintable(describe(n)));
    }

    void enumValues()
    {
        QVERIFY(!check(R"({"track": 0, "mode": "mix"})"));
        const auto e = check(R"({"track": 0, "mode": "MIX"})");
        QVERIFY2(e, "value outside enum accepted");
        QVERIFY(e->path.contains("mode"));
    }

    void minMax_data()
    {
        QTest::addColumn<QString>("json");
        QTest::addColumn<bool>("valid");
        QTest::newRow("min edge") << R"({"track": 0, "volume_db": -60})" << true;
        QTest::newRow("max edge") << R"({"track": 0, "volume_db": 6})" << true;
        QTest::newRow("below min") << R"({"track": 0, "volume_db": -60.01})" << false;
        QTest::newRow("above max") << R"({"track": 0, "volume_db": 6.5})" << false;
        QTest::newRow("int below") << R"({"track": 0, "count": 0})" << false;
        QTest::newRow("int above") << R"({"track": 0, "count": 17})" << false;
        QTest::newRow("nested above") << R"({"track": 0, "notes": [{"note": 128, "start_beat": 0, "length_beats": 1}]})" << false;
    }

    void minMax()
    {
        QFETCH(QString, json);
        QFETCH(bool, valid);
        const auto e = check(json.toUtf8().constData());
        QCOMPARE(!e.has_value(), valid);
    }

    void nestedArrayPath()
    {
        const auto e = check(R"({"track": 0, "notes": [
            {"note": 60, "start_beat": 0, "length_beats": 1},
            {"note": "C4", "start_beat": 0, "length_beats": 1}]})");
        QVERIFY2(e, "string note accepted");
        // The path names the array, the failing element and the field,
        // e.g. "$.notes[1].note".
        QVERIFY2(e->path.contains("notes"), qPrintable(describe(e)));
        QVERIFY2(e->path.contains("1"), qPrintable(describe(e)));
        QVERIFY2(e->path.contains("note"), qPrintable(describe(e)));
        QVERIFY2(!e->path.contains("0]"), qPrintable(describe(e)));

        const auto t = check(R"({"track": 0, "options": {"tags": ["ok", 5]}})");
        QVERIFY2(t, "number inside string array accepted");
        QVERIFY2(t->path.contains("options") && t->path.contains("tags"), qPrintable(describe(t)));
    }

    void oneOf()
    {
        QVERIFY(!check(R"({"track": "Bass"})"));
        QVERIFY(!check(R"({"track": 3})"));
        QVERIFY(check(R"({"track": true})"));
        QVERIFY(check(R"({"track": {"name": "Bass"}})"));
        QVERIFY(check(R"({"track": null})"));

        // oneOf means exactly one: overlapping branches both matching fail.
        const QJsonObject overlapping = parseObject(R"({
            "oneOf": [{"type": "number"}, {"type": "integer"}]
        })");
        QVERIFY(!AiSchemaValidator::validate(QJsonValue(1.5), overlapping));
        QVERIFY(AiSchemaValidator::validate(QJsonValue(2), overlapping));
    }

    void customRootPath()
    {
        const auto e = AiSchemaValidator::validate(QJsonValue("x"),
                                                   parseObject(R"({"type": "integer"})"),
                                                   QStringLiteral("$.steps[2].input"));
        QVERIFY(e);
        QVERIFY2(e->path.startsWith("$.steps[2].input"), qPrintable(e->path));
    }

    void emptySchemaAcceptsAnything()
    {
        QVERIFY(!AiSchemaValidator::validate(QJsonValue(42), QJsonObject{}));
        QVERIFY(!AiSchemaValidator::validate(QJsonValue("s"), QJsonObject{}));
        // A tool with no parameters: {"type":"object","properties":{}}.
        const auto noParams = parseObject(R"({"type": "object", "properties": {}})");
        QVERIFY(!AiSchemaValidator::validate(QJsonObject{}, noParams));
        QVERIFY(AiSchemaValidator::validate(QJsonObject{{"x", 1}}, noParams));
    }
};

OD_REGISTER_TEST(TestSchemaValidator)
#include "tst_SchemaValidator.moc"
