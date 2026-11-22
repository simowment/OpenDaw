#pragma once

// Stable, typed identifiers for project items, exposed to agents.
//
// Format: "<kind>:<rawEditItemID>", e.g. "track:1043", "clip:2210",
// "plugin:877", and "pattern:3" for OpenDaw patterns. Tracktion EditItemIDs
// are saved in the .tracktionedit file, so an id stays valid across renames,
// reordering, other tracks being deleted, and save/load.

#include "engine/AudioEngine.h"
#include <QString>
#include <optional>

namespace OpenDaw::ids {

inline const QString kTrack   = QStringLiteral("track");
inline const QString kClip    = QStringLiteral("clip");
inline const QString kPlugin  = QStringLiteral("plugin");
inline const QString kPattern = QStringLiteral("pattern");

inline QString make(const QString& kind, te::EditItemID id) {
    return kind + QLatin1Char(':') + QString::number(static_cast<qulonglong>(id.getRawID()));
}

inline QString of(const te::Track* t)  { return t ? make(kTrack, t->itemID) : QString(); }
inline QString of(const te::Clip* c)   { return c ? make(kClip, c->itemID) : QString(); }
inline QString of(const te::Plugin* p) { return p ? make(kPlugin, p->itemID) : QString(); }

// True when the string looks like "<kind>:<digits>" for the given kind.
inline bool isIdOfKind(const QString& s, const QString& kind) {
    if (!s.startsWith(kind + QLatin1Char(':'))) return false;
    bool ok = false;
    s.mid(kind.size() + 1).toULongLong(&ok);
    return ok;
}

// Parses "<kind>:<digits>"; nullopt when the prefix or number is wrong.
inline std::optional<te::EditItemID> parse(const QString& s, const QString& kind) {
    if (!isIdOfKind(s, kind)) return std::nullopt;
    const auto raw = s.mid(kind.size() + 1).toULongLong();
    return te::EditItemID::fromRawID(static_cast<uint64_t>(raw));
}

inline QString makePattern(int patternId) { return kPattern + QLatin1Char(':') + QString::number(patternId); }

inline std::optional<int> parsePattern(const QString& s) {
    if (!isIdOfKind(s, kPattern)) return std::nullopt;
    return s.mid(kPattern.size() + 1).toInt();
}

} // namespace OpenDaw::ids
