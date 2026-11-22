#pragma once

// Strict validator for the JSON Schema subset used by tool input_schemas.
//
// Supported keywords: type (string|number|integer|boolean|array|object|null,
// or an array of those), enum, oneOf, anyOf, minimum, maximum,
// exclusiveMinimum, exclusiveMaximum, minLength, maxLength, minItems,
// maxItems, items, properties, required, additionalProperties.
//
// Unknown properties are rejected at every object level that declares
// "properties", unless that level sets "additionalProperties": true (or a
// schema, which extra properties are then validated against).
// "integer" rejects numbers with a fractional part.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <optional>

namespace OpenDaw {

struct AiValidationError {
    QString path;     // e.g. "$.notes[2].velocity"
    QString message;  // e.g. "$.notes[2].velocity: expected integer, got 1.5"
};

class AiSchemaValidator {
public:
    static std::optional<AiValidationError> validate(const QJsonValue& value, const QJsonObject& schema,
                                                     const QString& path = QStringLiteral("$"));
};

} // namespace OpenDaw
