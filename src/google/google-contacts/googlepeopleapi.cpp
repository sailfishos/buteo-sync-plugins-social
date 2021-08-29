/****************************************************************************
 **
 ** Copyright (c) 2021 Jolla Ltd.
 **
 ** This program/library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation.
 **
 ** This program/library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ** You should have received a copy of the GNU Lesser General Public
 ** License along with this program/library; if not, write to the Free
 ** Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 ** 02110-1301 USA
 **
 ****************************************************************************/

#include "googlepeopleapi.h"
#include "trace.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QBuffer>
#include <QFile>
#include <QTemporaryFile>
#include <QFileInfo>
#include <QImage>
#include <QDebug>

namespace {

const QString ContentIdCreateContact = QStringLiteral("CreateContact:");
const QString ContentIdUpdateContact = QStringLiteral("UpdateContact:");
const QString ContentIdDeleteContact = QStringLiteral("DeleteContact:");
const QString ContentIdAddContactPhoto = QStringLiteral("AddContactPhoto:");
const QString ContentIdUpdateContactPhoto = QStringLiteral("UpdateContactPhoto:");
const QString ContentIdDeleteContactPhoto = QStringLiteral("DeleteContactPhoto:");
const int MaximumAvatarWidth = 512;

template<typename T>
QList<T> jsonArrayToList(const QJsonArray &array)
{
    QList<T> values;
    for (auto it = array.constBegin(); it != array.constEnd(); ++it) {
        values.append(T::fromJsonObject(it->toObject()));
    }
    return values;
}

QJsonObject parseJsonObject(const QByteArray &data)
{
    QJsonParseError err;

    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(lcSocialPlugin) << "JSON parse error:" << err.errorString();
        return QJsonObject();
    }

    return doc.object();
}

QFile *newResizedImageFile(const QString &imagePath, int maxWidth)
{
    QImage image;
    if (!image.load(imagePath)) {
        qCWarning(lcSocialPlugin) << "Unable to load image file:" << imagePath;
        return nullptr;
    }

    const QByteArray fileSuffix = QFileInfo(imagePath).suffix().toUtf8();
    if (image.size().width() < maxWidth) {
        return nullptr;
    }

    QTemporaryFile *temp = new QTemporaryFile;
    image = image.scaledToWidth(maxWidth);
    temp->setFileTemplate(imagePath);
    if (temp->open() && image.save(temp->fileName(), fileSuffix.data())) {
        temp->seek(0);
        return temp;
    } else {
        qCWarning(lcSocialPlugin) << "Unable to save resized image to file:" << temp->fileName();
    }

    delete temp;
    return nullptr;
}

bool writePhotoUpdateBody(QJsonObject *jsonObject, const QContactAvatar &avatar)
{
    if (!avatar.imageUrl().isLocalFile()) {
        qCWarning(lcSocialPlugin) << "Cannot open non-local avatar file:" << avatar.imageUrl();
        return false;
    }

    // Reduce the avatar size to minimize the uploaded data.
    const QString avatarFileName = avatar.imageUrl().toLocalFile();
    QFile *imageFile = newResizedImageFile(avatarFileName, MaximumAvatarWidth);
    if (!imageFile) {
        imageFile = new QFile(avatarFileName);
        if (!imageFile->open(QFile::ReadOnly)) {
            qCWarning(lcSocialPlugin) << "Unable to open avatar file:" << imageFile->fileName();
            delete imageFile;
            return false;
        }
    }

    jsonObject->insert("photoBytes", QString::fromLatin1(imageFile->readAll().toBase64()));
    delete imageFile;

    return true;
}

QString contentIdForContactOperation(GooglePeopleApi::OperationType operationType, const QContact &contact)
{
    static const QMap<GooglePeopleApi::OperationType, QString> contentIdPrefixes = {
        { GooglePeopleApi::CreateContact, ContentIdCreateContact },
        { GooglePeopleApi::UpdateContact, ContentIdUpdateContact },
        { GooglePeopleApi::DeleteContact, ContentIdDeleteContact },
        { GooglePeopleApi::AddContactPhoto, ContentIdAddContactPhoto },
        { GooglePeopleApi::UpdateContactPhoto, ContentIdUpdateContactPhoto },
        { GooglePeopleApi::DeleteContactPhoto, ContentIdDeleteContactPhoto },
    };

    const QString idPrefix = contentIdPrefixes.value(operationType);
    if (idPrefix.isEmpty()) {
        qCWarning(lcSocialPlugin) << "contentIdForOperationType(): invalid operation type!";
        return QString();
    }

    return QString("Content-ID: %1%2\n").arg(idPrefix).arg(contact.id().toString());
}

void addPartHeaderForContactOperation(QByteArray *bytes, GooglePeopleApi::OperationType operationType, const QContact &contact)
{
    bytes->append("\n"
                  "--batch_people\n"
                  "Content-Type: application/http\n"
                  "Content-Transfer-Encoding: binary\n");
    bytes->append(contentIdForContactOperation(operationType, contact).toUtf8());
    bytes->append("\n");
}

}

GooglePeopleApiRequest::GooglePeopleApiRequest(const QString &accessToken)
    : m_accessToken(accessToken)
{
}

GooglePeopleApiRequest::~GooglePeopleApiRequest()
{
}

QByteArray GooglePeopleApiRequest::writeMultiPartRequest(const QMap<GooglePeopleApi::OperationType, QList<QContact> > &batch)
{
    QByteArray bytes;
    bool hasContent = false;
    static const QString supportedPersonFieldList = GooglePeople::Person::supportedPersonFields().join(',');

    // Encode each multi-part request into the overall request.
    // Each part contains a Content-ID that indicates the request type, to assist in parsing the
    // response when it is received.

    for (auto it = batch.constBegin(); it != batch.constEnd(); ++it) {
        switch (it.key()) {
        case GooglePeopleApi::UnsupportedOperation:
            qCWarning(lcSocialPlugin) << "Invalid operation type in multi-part batch";
            break;
        case GooglePeopleApi::CreateContact:
        {
            for (const QContact &contact : it.value()) {
                const QJsonObject jsonObject = GooglePeople::Person::contactToJsonObject(contact);
                if (jsonObject.isEmpty()) {
                    qCWarning(lcSocialPlugin) << "No contact data found for contact:" << contact.id();
                } else {
                    addPartHeaderForContactOperation(&bytes, it.key(), contact);

                    const QByteArray body = "\n" + QJsonDocument(jsonObject).toJson();
                    bytes += QString("POST /v1/people:createContact?personFields=%1 HTTP/1.1\n")
                            .arg(supportedPersonFieldList);
                    bytes += "Content-Type: application/json\n";
                    bytes += QString("Content-Length: %1\n").arg(body.size()).toLatin1();
                    bytes += "Accept: application/json\n";
                    bytes += body;
                    bytes += "\n";
                    hasContent = true;
                }
            }
            break;
        }
        case GooglePeopleApi::UpdateContact:
        {
            for (const QContact &contact : it.value()) {
                QStringList updatedPersonFieldList;
                const QJsonObject jsonObject = GooglePeople::Person::contactToJsonObject(
                            contact, &updatedPersonFieldList);
                if (updatedPersonFieldList.isEmpty()) {
                    qCInfo(lcSocialPlugin) << "No non-avatar fields have changed in contact:" << contact.id();
                } else if (jsonObject.isEmpty()) {
                    qCWarning(lcSocialPlugin) << "No contact data found for contact:" << contact.id();
                } else {
                    addPartHeaderForContactOperation(&bytes, it.key(), contact);

                    const QByteArray body = "\n" + QJsonDocument(jsonObject).toJson();
                    bytes += QString("PATCH /v1/%1:updateContact?updatePersonFields=%2&personFields=%3 HTTP/1.1\n")
                            .arg(GooglePeople::Person::personResourceName(contact))
                            .arg(updatedPersonFieldList.join(','))
                            .arg(supportedPersonFieldList);
                    bytes += "Content-Type: application/json\n";
                    bytes += QString("Content-Length: %1\n").arg(body.size()).toLatin1();
                    bytes += "Accept: application/json\n";
                    bytes += body;
                    bytes += "\n";
                    hasContent = true;
                }
            }
            break;
        }
        case GooglePeopleApi::DeleteContact:
        {
            for (const QContact &contact : it.value()) {
                addPartHeaderForContactOperation(&bytes, it.key(), contact);

                bytes += QString("DELETE /v1/%1:deleteContact HTTP/1.1\n")
                        .arg(GooglePeople::Person::personResourceName(contact));
                bytes += "Content-Type: application/json\n";
                bytes += "Accept: application/json\n";
                bytes += "\n";
                hasContent = true;
            }
            break;
        }
        case GooglePeopleApi::AddContactPhoto:
        case GooglePeopleApi::UpdateContactPhoto:
        {
            for (const QContact &contact : it.value()) {
                const QContactAvatar avatar = GooglePeople::Photo::getPrimaryPhoto(contact);
                if (avatar.imageUrl().isEmpty()) {
                    qCWarning(lcSocialPlugin) << "No avatar found in contact:" << contact;
                    continue;
                }
                QJsonObject jsonObject;
                if (!writePhotoUpdateBody(&jsonObject, avatar)) {
                    qCWarning(lcSocialPlugin) << "Failed to write avatar update details:" << avatar.imageUrl();
                    continue;
                }
                jsonObject.insert("personFields", supportedPersonFieldList);

                addPartHeaderForContactOperation(&bytes, it.key(), contact);

                const QByteArray body = "\n" + QJsonDocument(jsonObject).toJson();
                bytes += QString("PATCH /v1/%1:updateContactPhoto HTTP/1.1\n")
                        .arg(GooglePeople::Person::personResourceName(contact));
                bytes += "Content-Type: application/json\n";
                bytes += QString("Content-Length: %1\n").arg(body.size()).toLatin1();
                bytes += "Accept: application/json\n";
                bytes += body;
                bytes += "\n";
                hasContent = true;
            }
            break;
        }
        case GooglePeopleApi::DeleteContactPhoto:
        {
            for (const QContact &contact : it.value()) {
                addPartHeaderForContactOperation(&bytes, it.key(), contact);

                bytes += QString("DELETE /v1/%1:deleteContactPhoto?personFields=%2 HTTP/1.1\n")
                        .arg(GooglePeople::Person::personResourceName(contact))
                        .arg(supportedPersonFieldList);
                bytes += QString("Content-ID: %1%2\n")
                        .arg(ContentIdDeleteContactPhoto)
                        .arg(contact.id().toString()).toUtf8();
                bytes += "Accept: application/json\n";
                bytes += "\n";
                hasContent = true;
            }
            break;
        }
        }
    }

    if (!hasContent) {
        return QByteArray();
    }

    bytes += "--batch_people--\n\n";

    return bytes;
}

//-----------

void GooglePeopleApiResponse::BatchResponsePart::reset()
{
    contentType.clear();
    contentId.clear();
    bodyStatusLine.clear();
    bodyContentType.clear();
    body.clear();
}

void GooglePeopleApiResponse::BatchResponsePart::parse(
        GooglePeopleApi::OperationType *operationType,
        QString *contactId,
        GooglePeople::Person *person,
        Error *error) const
{
    static const QString responseToken = QStringLiteral("response-");
    if (!responseToken.startsWith(responseToken)) {
        qCWarning(lcSocialPlugin) << "Unexpected content ID in response:" << contentId;
        return;
    }
    const QString operationInfo = contentId.mid(responseToken.length());
    static const QMap<QString, GooglePeopleApi::OperationType> operationTypes = {
        { ContentIdCreateContact, GooglePeopleApi::CreateContact },
        { ContentIdUpdateContact, GooglePeopleApi::UpdateContact },
        { ContentIdDeleteContact, GooglePeopleApi::DeleteContact },
        { ContentIdAddContactPhoto, GooglePeopleApi::AddContactPhoto },
        { ContentIdUpdateContactPhoto, GooglePeopleApi::UpdateContactPhoto },
        { ContentIdDeleteContactPhoto, GooglePeopleApi::DeleteContactPhoto },
    };

    *operationType = GooglePeopleApi::UnsupportedOperation;
    for (auto it = operationTypes.constBegin(); it != operationTypes.constEnd(); ++it) {
        if (operationInfo.startsWith(it.key())) {
            *operationType = it.value();
            *contactId = operationInfo.mid(it.key().length());
            break;
        }
    }

    const QJsonObject jsonBody = parseJsonObject(body);
    const QJsonObject errorObject = jsonBody.value("error").toObject();
    if (!errorObject.isEmpty()) {
        error->code = errorObject.value("code").toInt();
        error->message = errorObject.value("message").toString();
        error->status = errorObject.value("status").toString();
    } else {
        switch (*operationType) {
        case GooglePeopleApi::CreateContact:
        case GooglePeopleApi::UpdateContact:
            // The JSON response is a Person object
            *person = GooglePeople::Person::fromJsonObject(jsonBody);
            break;
        case GooglePeopleApi::AddContactPhoto:
        case GooglePeopleApi::UpdateContactPhoto:
        case GooglePeopleApi::DeleteContactPhoto:
        {
            // The JSON response contains a "person" value that is a Person object
            *person = GooglePeople::Person::fromJsonObject(jsonBody.value("person").toObject());
            break;
        }
        case GooglePeopleApi::DeleteContact:
            // JSON response is empty.
            break;
        case GooglePeopleApi::UnsupportedOperation:
            break;
        }
    }
}

//-----------

void GooglePeopleApiResponse::PeopleConnectionsListResponse::getContacts(
        int accountId,
        const QList<QContactCollection> &candidateCollections,
        QList<QContact> *addedOrModified,
        QList<QContact> *deleted) const
{
    for (const GooglePeople::Person &person : connections) {
        if (person.metadata.deleted) {
            if (deleted) {
                deleted->append(person.toContact(accountId, candidateCollections));
            }
        } else if (addedOrModified) {
            addedOrModified->append(person.toContact(accountId, candidateCollections));
        }
    }
}

bool GooglePeopleApiResponse::readResponse(
        const QByteArray &data, GooglePeopleApiResponse::ContactGroupsResponse *response)
{
    if (!response) {
        return false;
    }

    const QJsonObject object = parseJsonObject(data);
    response->contactGroups = jsonArrayToList<GooglePeople::ContactGroup>(object.value("contactGroups").toArray());
    response->totalItems = object.value("totalItems").toString().toInt();
    response->nextPageToken = object.value("nextPageToken").toString();
    response->nextSyncToken = object.value("nextSyncToken").toString();

    return true;
}

bool GooglePeopleApiResponse::readResponse(
        const QByteArray &data, GooglePeopleApiResponse::PeopleConnectionsListResponse *response)
{
    if (!response) {
        return false;
    }

    const QJsonObject object = parseJsonObject(data);
    response->connections = jsonArrayToList<GooglePeople::Person>(object.value("connections").toArray());
    response->nextPageToken = object.value("nextPageToken").toString();
    response->nextSyncToken = object.value("nextSyncToken").toString();
    response->totalPeople = object.value("totalPeople").toString().toInt();
    response->totalItems = object.value("totalItems").toString().toInt();

    return true;
}

bool GooglePeopleApiResponse::readMultiPartResponse(
        const QByteArray &data, QList<BatchResponsePart> *responseParts)
{
    if (!responseParts) {
        return false;
    }

    QBuffer buffer;
    buffer.setData(data);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return false;
    }

    /*
    Example multi-part response body:

    --batch_izedEXuDWnLH5_41NeoKptxfL5sqA2K6
    Content-Type: application/http
    Content-ID: response-

    HTTP/1.1 200 OK
    Content-Type: application/json; charset=UTF-8
    Vary: Origin
    Vary: X-Origin
    Vary: Referer

    {
        // json body of created contact
    }

    --batch_izedEXuDWnLH5_41NeoKptxfL5sqA2K6
    Content-Type: application/http
    Content-ID: response-

    HTTP/1.1 400 Bad Request
    Vary: Origin
    Vary: X-Origin
    Vary: Referer
    Content-Type: application/json; charset=UTF-8

    {
      "error": {
        "code": 400,
        "message": "Request person.etag is different than the current person.etag. Clear local cache and get the latest person.",
        "status": "FAILED_PRECONDITION"
      }
    }

    --batch_izedEXuDWnLH5_41NeoKptxfL5sqA2K6--
    */

    enum PartParseStatus {
        ParseHeaders,
        ParseBodyHeaders,
        ParseBody
    };

    BatchResponsePart currentPart;
    PartParseStatus parseStatus = ParseHeaders;

    static const QByteArray contentTypeToken = "Content-Type:";
    static const QByteArray contentIdToken = "Content-ID:";

    while (!buffer.atEnd()) {
        const QByteArray line = buffer.readLine();
        const bool isSeparator = line.startsWith("--batch_");

        if (parseStatus == ParseHeaders) {
            // Parse the headers for this part.
            if (isSeparator) {
                continue;
            } else if (line.startsWith(contentTypeToken)) {
                currentPart.contentType = QString::fromUtf8(line.mid(contentTypeToken.length() + 1).trimmed());
            } else if (line.startsWith(contentIdToken)) {
                currentPart.contentId = QString::fromUtf8(line.mid(contentIdToken.length() + 1).trimmed());
            } else if (line.trimmed().isEmpty() && !currentPart.contentType.isEmpty()) {
                parseStatus = ParseBodyHeaders;
            }
        } else if (parseStatus == ParseBodyHeaders) {
            // Parse the body of this part, which itself contains a separate HTTP response with
            // headers and body.
            if (line.startsWith("HTTP/")) {
                currentPart.bodyStatusLine = line.trimmed();
            } else if (line.startsWith(contentTypeToken)) {
                currentPart.bodyContentType = QString::fromUtf8(line.mid(contentTypeToken.length() + 1).trimmed());
            } else if (line.trimmed().isEmpty() && !currentPart.bodyContentType.isEmpty()) {
                parseStatus = ParseBody;
            }
        } else if (parseStatus == ParseBody) {
            if (isSeparator) {
                // This is the start of another part, or the end of the batch.
                responseParts->append(currentPart);

                currentPart.reset();
                parseStatus = ParseHeaders;

                if (line.endsWith("--")) {
                    break;
                }
            } else {
                currentPart.body += line;
            }
        }
    }

    return true;
}
