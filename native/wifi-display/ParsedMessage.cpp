/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ParsedMessage.h"

#include <ctype.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>

namespace android {

// static
sp<ParsedMessage> ParsedMessage::Parse(
        const char *data, size_t size, bool noMoreData, size_t *length) {
    sp<ParsedMessage> msg = new ParsedMessage;
    ssize_t res = msg->parse(data, size, noMoreData);

    if (res < 0) {
        *length = 0;
        return NULL;
    }

    *length = res;
    return msg;
}

ParsedMessage::ParsedMessage() {
}

ParsedMessage::~ParsedMessage() {
}

bool ParsedMessage::findString(const char *name, AString *value) const {
    AString key = name;
    key.tolower();

    ssize_t index = mDict.indexOfKey(key);

    if (index < 0) {
        value->clear();

        return false;
    }

    *value = mDict.valueAt(index);
    return true;
}

bool ParsedMessage::findInt32(const char *name, int32_t *value) const {
    AString stringValue;

    if (!findString(name, &stringValue)) {
        return false;
    }

    char *end;
    *value = strtol(stringValue.c_str(), &end, 10);

    if (end == stringValue.c_str() || *end != '\0') {
        *value = 0;
        return false;
    }

    return true;
}

const char *ParsedMessage::getContent() const {
    return mContent.c_str();
}

ssize_t ParsedMessage::parse(const char *data, size_t size, bool noMoreData) {
    if (size == 0) {
        return -1;
    }

    ssize_t lastDictIndex = -1;

    size_t offset = 0;
    while (offset < size) {
        size_t lineEndOffset = offset;
        while (lineEndOffset + 1 < size
                && (data[lineEndOffset] != '\r'
                        || data[lineEndOffset + 1] != '\n')) {
            ++lineEndOffset;
        }

        if (lineEndOffset + 1 >= size) {
            return -1;
        }

        AString line(&data[offset], lineEndOffset - offset);

        if (offset == 0) {
            // Special handling for the request/status line.

            mDict.add(AString("_"), line);
            offset = lineEndOffset + 2;

            continue;
        }

        if (lineEndOffset == offset) {
            offset += 2;
            break;
        }

        if (line.c_str()[0] == ' ' || line.c_str()[0] == '\t') {
            // Support for folded header values.

            if (lastDictIndex >= 0) {
                // Otherwise it's malformed since the first header line
                // cannot continue anything...

                AString &value = mDict.editValueAt(lastDictIndex);
                value.append(line);
            }

            offset = lineEndOffset + 2;
            continue;
        }

        ssize_t colonPos = line.find(":");
        if (colonPos >= 0) {
            AString key(line, 0, colonPos);
            key.trim();
            key.tolower();

            line.erase(0, colonPos + 1);

            lastDictIndex = mDict.add(key, line);
        }

        offset = lineEndOffset + 2;
    }

    for (size_t i = 0; i < mDict.size(); ++i) {
        mDict.editValueAt(i).trim();
    }

    // Found the end of headers.

    int32_t contentLength;
    if (!findInt32("content-length", &contentLength) || contentLength < 0) {
        contentLength = 0;
    }

    size_t totalLength = offset + contentLength;

    if (size < totalLength) {
        return -1;
    }

    mContent.setTo(&data[offset], contentLength);

    return totalLength;
}

void ParsedMessage::getRequestField(size_t index, AString *field) const {
    AString line;
    CHECK(findString("_", &line));

    size_t prevOffset = 0;
    size_t offset = 0;
    for (size_t i = 0; i <= index; ++i) {
        ssize_t spacePos = line.find(" ", offset);

        if (spacePos < 0) {
            spacePos = line.size();
        }

        prevOffset = offset;
        offset = spacePos + 1;
    }

    field->setTo(line, prevOffset, offset - prevOffset - 1);
}

bool ParsedMessage::getStatusCode(int32_t *statusCode) const {
    AString statusCodeString;
    getRequestField(1, &statusCodeString);

    char *end;
    *statusCode = strtol(statusCodeString.c_str(), &end, 10);

    if (*end != '\0' || end == statusCodeString.c_str()
            || (*statusCode) < 100 || (*statusCode) > 999) {
        *statusCode = 0;
        return false;
    }

    return true;
}

AString ParsedMessage::debugString() const {
    AString line;
    CHECK(findString("_", &line));

    line.append("\n");

    for (size_t i = 0; i < mDict.size(); ++i) {
        const AString &key = mDict.keyAt(i);
        const AString &value = mDict.valueAt(i);

        if (key == AString("_")) {
            continue;
        }

        line.append(key);
        line.append(": ");
        line.append(value);
        line.append("\n");
    }

    line.append("\n");
    line.append(mContent);

    return line;
}

// static
bool ParsedMessage::GetAttribute(
        const char *s, const char *key, AString *value) {
    value->clear();

    size_t keyLen = strlen(key);

    for (;;) {
        while (isspace(*s)) {
            ++s;
        }

        const char *colonPos = strchr(s, ';');

        size_t len =
            (colonPos == NULL) ? strlen(s) : colonPos - s;

        if (len >= keyLen + 1 && s[keyLen] == '=' && !strncmp(s, key, keyLen)) {
            value->setTo(&s[keyLen + 1], len - keyLen - 1);
            return true;
        }

        if (colonPos == NULL) {
            return false;
        }

        s = colonPos + 1;
    }
}

// static
bool ParsedMessage::GetInt32Attribute(
        const char *s, const char *key, int32_t *value) {
    AString stringValue;
    if (!GetAttribute(s, key, &stringValue)) {
        *value = 0;
        return false;
    }

    char *end;
    *value = strtol(stringValue.c_str(), &end, 10);

    if (end == stringValue.c_str() || *end != '\0') {
        *value = 0;
        return false;
    }

    return true;
}

}  // namespace android

