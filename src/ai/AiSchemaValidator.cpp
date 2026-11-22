#include "AiSchemaValidator.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>
#include <cmath>

namespace OpenDaw {

namespace {

bool isWholeNumber(double d)
{
    return std::isfinite(d) && std::floor(d) == d;
}

QString formatNumber(double d)
{
    if (isWholeNumber(d) && std::fabs(d) < 1e15)
        return QString::number(static_cast<qint64>(d));
    return QString::number(d, 'g', 12);
}

// Short description of a value for error messages: scalars by value,
// containers by kind.
QString describe(const QJsonValue& v)
{
    switch (v.type()) {
    case QJsonValue::Null:      return QStringLiteral("null");
    case QJsonValue::Bool:      return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    case QJsonValue::Double:    return formatNumber(v.toDouble());
    case QJsonValue::String: {
        QString s = v.toString();
        if (s.size() > 40) s = s.left(37) + QStringLiteral("...");
        return QLatin1Char('"') + s + QLatin1Char('"');
    }
    case QJsonValue::Array:     return QStringLiteral("array");
    case QJsonValue::Object:    return QStringLiteral("object");
    case QJsonValue::Undefined: return QStringLiteral("nothing");
    }
    return QStringLiteral("unknown");
}

bool matchesType(const QJsonValue& v, const QString& type)
{
    if (type == QLatin1String("string"))  return v.isString();
    if (type == QLatin1String("number"))  return v.isDouble() && std::isfinite(v.toDouble());
    if (type == QLatin1String("integer")) return v.isDouble() && isWholeNumber(v.toDouble());
    if (type == QLatin1String("boolean")) return v.isBool();
    if (type == QLatin1String("array"))   return v.isArray();
    if (type == QLatin1String("object"))  return v.isObject();
    if (type == QLatin1String("null"))    return v.isNull();
    return false; // unknown type names never match
}

// JSON equality with numbers compared numerically (4 == 4.0).
bool jsonEquals(const QJsonValue& a, const QJsonValue& b)
{
    if (a.isDouble() && b.isDouble())
        return a.toDouble() == b.toDouble();
    if (a.type() != b.type())
        return false;
    if (a.isArray()) {
        const auto aa = a.toArray(), bb = b.toArray();
        if (aa.size() != bb.size()) return false;
        for (qsizetype i = 0; i < aa.size(); ++i)
            if (!jsonEquals(aa.at(i), bb.at(i))) return false;
        return true;
    }
    if (a.isObject()) {
        const auto ao = a.toObject(), bo = b.toObject();
        if (ao.size() != bo.size()) return false;
        for (auto it = ao.constBegin(); it != ao.constEnd(); ++it) {
            if (!bo.contains(it.key())) return false;
            if (!jsonEquals(it.value(), bo.value(it.key()))) return false;
        }
        return true;
    }
    return a == b;
}

bool isPlainKey(const QString& key)
{
    if (key.isEmpty()) return false;
    for (const QChar c : key)
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_')))
            return false;
    return !key.at(0).isDigit();
}

QString childPath(const QString& path, const QString& key)
{
    if (isPlainKey(key))
        return path + QLatin1Char('.') + key;
    QString escaped = key;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return path + QStringLiteral("[\"") + escaped + QStringLiteral("\"]");
}

AiValidationError makeError(const QString& path, const QString& what)
{
    return AiValidationError{path, path + QStringLiteral(": ") + what};
}

QStringList schemaTypes(const QJsonObject& schema)
{
    QStringList types;
    const auto t = schema.value(QLatin1String("type"));
    if (t.isString()) {
        types << t.toString();
    } else if (t.isArray()) {
        for (const auto& e : t.toArray())
            if (e.isString()) types << e.toString();
    }
    return types;
}

// A one-word summary of what a sub-schema accepts, for oneOf/anyOf messages.
QString schemaSummary(const QJsonObject& schema)
{
    const auto types = schemaTypes(schema);
    if (!types.isEmpty()) return types.join(QLatin1Char('|'));
    if (schema.contains(QLatin1String("enum"))) return QStringLiteral("enum value");
    return QStringLiteral("value");
}

std::optional<AiValidationError> validateImpl(const QJsonValue& value, const QJsonObject& schema,
                                              const QString& path);

std::optional<AiValidationError> validateAlternatives(const QJsonValue& value, const QJsonArray& alternatives,
                                                      const QString& path, bool exactlyOne)
{
    int matches = 0;
    std::optional<AiValidationError> firstTypedError; // error from a branch whose type matched
    QStringList summaries;
    for (const auto& alt : alternatives) {
        const auto sub = alt.toObject();
        summaries << schemaSummary(sub);
        auto e = validateImpl(value, sub, path);
        if (!e) {
            ++matches;
            continue;
        }
        const auto types = schemaTypes(sub);
        bool typeMatched = types.isEmpty();
        for (const auto& t : types)
            if (matchesType(value, t)) typeMatched = true;
        if (typeMatched && !firstTypedError)
            firstTypedError = e;
    }
    if (matches == 0) {
        // A branch of the right type failed on a detail (range, property...):
        // that message is far more useful than "expected one of".
        if (firstTypedError) return firstTypedError;
        summaries.removeDuplicates();
        return makeError(path, QStringLiteral("expected %1, got %2")
                                   .arg(summaries.join(QStringLiteral(" or ")), describe(value)));
    }
    if (exactlyOne && matches > 1)
        return makeError(path, QStringLiteral("value matches more than one allowed form"));
    return std::nullopt;
}

std::optional<AiValidationError> validateImpl(const QJsonValue& value, const QJsonObject& schema,
                                              const QString& path)
{
    // type
    const auto types = schemaTypes(schema);
    if (!types.isEmpty()) {
        bool any = false;
        for (const auto& t : types)
            if (matchesType(value, t)) { any = true; break; }
        if (!any)
            return makeError(path, QStringLiteral("expected %1, got %2")
                                       .arg(types.join(QStringLiteral(" or ")), describe(value)));
    }

    // enum
    if (schema.contains(QLatin1String("enum"))) {
        const auto allowed = schema.value(QLatin1String("enum")).toArray();
        bool found = false;
        for (const auto& a : allowed)
            if (jsonEquals(a, value)) { found = true; break; }
        if (!found) {
            QStringList names;
            for (const auto& a : allowed) names << describe(a);
            return makeError(path, QStringLiteral("must be one of %1, got %2")
                                       .arg(names.join(QStringLiteral(", ")), describe(value)));
        }
    }

    // oneOf / anyOf
    if (schema.contains(QLatin1String("oneOf"))) {
        if (auto e = validateAlternatives(value, schema.value(QLatin1String("oneOf")).toArray(), path, true))
            return e;
    }
    if (schema.contains(QLatin1String("anyOf"))) {
        if (auto e = validateAlternatives(value, schema.value(QLatin1String("anyOf")).toArray(), path, false))
            return e;
    }

    // numbers
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (!std::isfinite(d))
            return makeError(path, QStringLiteral("expected a finite number"));
        if (schema.contains(QLatin1String("minimum"))) {
            const double m = schema.value(QLatin1String("minimum")).toDouble();
            if (d < m)
                return makeError(path, QStringLiteral("must be >= %1, got %2").arg(formatNumber(m), formatNumber(d)));
        }
        if (schema.contains(QLatin1String("maximum"))) {
            const double m = schema.value(QLatin1String("maximum")).toDouble();
            if (d > m)
                return makeError(path, QStringLiteral("must be <= %1, got %2").arg(formatNumber(m), formatNumber(d)));
        }
        if (schema.value(QLatin1String("exclusiveMinimum")).isDouble()) {
            const double m = schema.value(QLatin1String("exclusiveMinimum")).toDouble();
            if (d <= m)
                return makeError(path, QStringLiteral("must be > %1, got %2").arg(formatNumber(m), formatNumber(d)));
        }
        if (schema.value(QLatin1String("exclusiveMaximum")).isDouble()) {
            const double m = schema.value(QLatin1String("exclusiveMaximum")).toDouble();
            if (d >= m)
                return makeError(path, QStringLiteral("must be < %1, got %2").arg(formatNumber(m), formatNumber(d)));
        }
    }

    // strings
    if (value.isString()) {
        const auto len = value.toString().size();
        if (schema.contains(QLatin1String("minLength"))) {
            const int m = schema.value(QLatin1String("minLength")).toInt();
            if (len < m)
                return makeError(path, QStringLiteral("must be at least %1 characters long").arg(m));
        }
        if (schema.contains(QLatin1String("maxLength"))) {
            const int m = schema.value(QLatin1String("maxLength")).toInt();
            if (len > m)
                return makeError(path, QStringLiteral("must be at most %1 characters long").arg(m));
        }
    }

    // arrays
    if (value.isArray()) {
        const auto arr = value.toArray();
        if (schema.contains(QLatin1String("minItems"))) {
            const int m = schema.value(QLatin1String("minItems")).toInt();
            if (arr.size() < m)
                return makeError(path, QStringLiteral("must have at least %1 item(s), got %2").arg(m).arg(arr.size()));
        }
        if (schema.contains(QLatin1String("maxItems"))) {
            const int m = schema.value(QLatin1String("maxItems")).toInt();
            if (arr.size() > m)
                return makeError(path, QStringLiteral("must have at most %1 item(s), got %2").arg(m).arg(arr.size()));
        }
        const auto items = schema.value(QLatin1String("items"));
        if (items.isObject()) {
            const auto itemSchema = items.toObject();
            for (qsizetype i = 0; i < arr.size(); ++i) {
                if (auto e = validateImpl(arr.at(i), itemSchema, path + QStringLiteral("[%1]").arg(i)))
                    return e;
            }
        }
    }

    // objects
    if (value.isObject()) {
        const auto obj = value.toObject();

        const auto required = schema.value(QLatin1String("required")).toArray();
        for (const auto& r : required) {
            const QString key = r.toString();
            if (!obj.contains(key))
                return makeError(childPath(path, key), QStringLiteral("required property is missing"));
        }

        const bool hasProps = schema.contains(QLatin1String("properties"));
        const auto props = schema.value(QLatin1String("properties")).toObject();
        const auto additional = schema.value(QLatin1String("additionalProperties"));

        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            const QString key = it.key();
            const QString p = childPath(path, key);
            if (props.contains(key)) {
                if (auto e = validateImpl(it.value(), props.value(key).toObject(), p))
                    return e;
                continue;
            }
            if (additional.isObject()) {
                if (auto e = validateImpl(it.value(), additional.toObject(), p))
                    return e;
                continue;
            }
            // Strict by default where properties are declared.
            const bool allowExtra = additional.isBool() ? additional.toBool() : !hasProps;
            if (!allowExtra) {
                QStringList allowed = props.keys();
                const QString hint = allowed.isEmpty()
                    ? QStringLiteral("this object takes no properties")
                    : QStringLiteral("allowed: %1").arg(allowed.join(QStringLiteral(", ")));
                return makeError(p, QStringLiteral("unknown property (%1)").arg(hint));
            }
        }
    }

    return std::nullopt;
}

} // namespace

std::optional<AiValidationError> AiSchemaValidator::validate(const QJsonValue& value, const QJsonObject& schema,
                                                             const QString& path)
{
    return validateImpl(value, schema, path);
}

} // namespace OpenDaw
