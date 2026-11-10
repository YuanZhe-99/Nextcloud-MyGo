#pragma once
#include <QByteArray>
#include <QJsonDocument>

/// decodeMessage rejects invalid UTF-8, duplicate decoded keys, nonfinite values and excessive depth.
/// Inputs: a bounded complete payload, excluding the four-byte frame header.
bool decodeMessage(const QByteArray &bytes, QJsonDocument &document);

/// frameMessage prefixes a compact JSON message with its little-endian byte length.
QByteArray frameMessage(const QJsonDocument &document);
