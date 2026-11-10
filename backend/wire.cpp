#include "wire.h"
#include <QJsonArray>
#include <QSet>
#include <QStringDecoder>
#include <QtEndian>
#include <cmath>

namespace {
/// Scanner checks information QJsonDocument would discard, especially duplicate object keys.
class Scanner {
public:
    /// Scanner starts at the first byte of a complete UTF-8 document.
    explicit Scanner(const QByteArray &bytes) : bytes_(bytes) {}
    /// complete consumes exactly one JSON value with no trailing non-whitespace data.
    bool complete() { return value(0) && (space(), pos_ == bytes_.size()); }
private:
    /// space skips only the four JSON whitespace characters.
    void space() { while (pos_ < bytes_.size() && QByteArray(" \t\r\n").contains(bytes_[pos_])) ++pos_; }
    /// string decodes a quoted string with Qt's JSON escape validation.
    bool string(QString &out) {
        if (pos_ >= bytes_.size() || bytes_[pos_] != '"') return false;
        const auto start = pos_++;
        bool escape = false;
        while (pos_ < bytes_.size()) {
            const char c = bytes_[pos_++];
            if (!escape && c == '"') {
                QJsonParseError error;
                const auto d = QJsonDocument::fromJson('[' + bytes_.mid(start, pos_ - start) + ']', &error);
                if (error.error != QJsonParseError::NoError) return false;
                out = d.array().at(0).toString(); return true;
            }
            if (escape) escape = false; else if (c == '\\') escape = true;
        }
        return false;
    }
    /// value recursively validates containers and finite primitive values within the depth budget.
    bool value(int depth) {
        space(); if (pos_ >= bytes_.size()) return false;
        char c = bytes_[pos_];
        if (c == '"') { QString ignored; return string(ignored); }
        if (c == '{' || c == '[') {
            if (depth >= 64) return false;
            ++pos_; const char end = c == '{' ? '}' : ']'; QSet<QString> keys;
            space(); if (pos_ < bytes_.size() && bytes_[pos_] == end) { ++pos_; return true; }
            while (pos_ < bytes_.size()) {
                if (c == '{') {
                    QString key; space(); if (!string(key) || keys.contains(key)) return false;
                    keys.insert(key); space(); if (pos_ >= bytes_.size() || bytes_[pos_++] != ':') return false;
                }
                if (!value(depth + 1)) return false;
                space(); if (pos_ >= bytes_.size()) return false;
                const char next = bytes_[pos_++]; if (next == end) return true; if (next != ',') return false;
            }
            return false;
        }
        const auto start = pos_;
        while (pos_ < bytes_.size() && !QByteArray(" \t\r\n,]}").contains(bytes_[pos_])) ++pos_;
        QJsonParseError error;
        const auto d = QJsonDocument::fromJson('[' + bytes_.mid(start, pos_ - start) + ']', &error);
        return error.error == QJsonParseError::NoError && d.array().size() == 1 &&
            (!d.array().at(0).isDouble() || std::isfinite(d.array().at(0).toDouble()));
    }
    const QByteArray &bytes_;
    qsizetype pos_ = 0;
};
}

bool decodeMessage(const QByteArray &bytes, QJsonDocument &document) {
    if (bytes.isEmpty() || bytes.size() > 1048576) return false;
    QStringDecoder utf8(QStringDecoder::Utf8); const auto text = utf8.decode(bytes); Q_UNUSED(text);
    if (utf8.hasError() || !Scanner(bytes).complete()) return false;
    QJsonParseError error; document = QJsonDocument::fromJson(bytes, &error);
    return error.error == QJsonParseError::NoError;
}

QByteArray frameMessage(const QJsonDocument &document) {
    const auto payload = document.toJson(QJsonDocument::Compact);
    QByteArray frame(4, '\0'); qToLittleEndian<quint32>(quint32(payload.size()), frame.data()); frame.append(payload); return frame;
}
