#pragma once
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>

namespace kobalt::i18n {
inline QJsonObject resource(const QString &name) {
    QFile file(":/kobalt/translations/" + name + ".json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
inline const QJsonObject &languages() {
    static const auto result = resource("languages");
    return result;
}
inline QString normalize(QString code) {
    code = code.toLower().replace('_', '-').section('-', 0, 0);
    if (code == "nb" || code == "nn")
        code = "no";
    if (code == "kmr")
        code = "ku";
    return languages().contains(code) ? code : QStringLiteral("en");
}
struct State {
    QMutex mutex;
    QString code;
    QJsonObject catalog, english;
};
inline State &state() {
    static State value;
    return value;
}
inline void setLanguage(const QString &requested) {
    auto &s = state();
    QMutexLocker lock(&s.mutex);
    const auto code = normalize(requested.isEmpty() ? QLocale::system().name() : requested);
    if (s.code == code)
        return;
    s.code = code;
    s.catalog = resource(code);
    if (s.english.isEmpty())
        s.english = resource("en");
}
inline QString language() {
    auto &s = state();
    QMutexLocker lock(&s.mutex);
    return s.code;
}
inline QString lookup(const QString &group, const QString &key, const QString &fallback) {
    auto &s = state();
    QMutexLocker lock(&s.mutex);
    if (s.code.isEmpty()) {
        s.code = normalize(QLocale::system().name());
        s.catalog = resource(s.code);
        s.english = resource("en");
    }
    const auto translated = s.catalog.value(group).toObject().value(key).toString();
    if (!translated.isEmpty())
        return translated;
    return s.english.value(group).toObject().value(key).toString(fallback);
}
inline QString text(const QString &source) {
    return lookup("tools", source, source);
}
} // namespace kobalt::i18n
