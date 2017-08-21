/****************************************************************************
 **
 ** Copyright (C) 2015 Jolla Ltd.
 ** Contact: Jonni Rainisto <jonni.rainisto@jollamobile.com>
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

#include "dropboximagesyncadaptor.h"
#include "trace.h"

#include <QtCore/QPair>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QVariantMap>
#include <QtCore/QByteArray>
#include <QtCore/QUrlQuery>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlQuery>
#include <QtSql/QSqlError>
#include <QtSql/QSqlRecord>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <MGConfItem>

// Update the following version if database schema changes e.g. new
// fields are added to the existing tables.
// It will make old tables dropped and creates new ones.

// Currently, we integrate with the device image gallery via saving thumbnails to the
// ~/.config/sociald/images directory, and filling the ~/.config/sociald/images/dropbox.db
// with appropriate data.

namespace {
    bool filenameHasImageExtension(const QString &filename) {
        if (filename.endsWith(".jpg", Qt::CaseInsensitive)  ||
            filename.endsWith(".jpeg", Qt::CaseInsensitive) ||
            filename.endsWith(".png", Qt::CaseInsensitive)  ||
            filename.endsWith(".tiff", Qt::CaseInsensitive) ||
            filename.endsWith(".tif", Qt::CaseInsensitive)  ||
            filename.endsWith(".gif", Qt::CaseInsensitive)  ||
            filename.endsWith(".bmp", Qt::CaseInsensitive)) {
            return true;
        }
        return false;
    }
}

DropboxImageSyncAdaptor::DropboxImageSyncAdaptor(QObject *parent)
    : DropboxDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Images, parent)
    , m_optimalThumbnailWidth(0)
    , m_optimalImageWidth(0)
{
    setInitialActive(m_db.isValid());
}

DropboxImageSyncAdaptor::~DropboxImageSyncAdaptor()
{
}

QString DropboxImageSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("dropbox-images");
}

void DropboxImageSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    // get ready for sync
    if (!determineOptimalDimensions()) {
        SOCIALD_LOG_ERROR("unable to determine optimal image dimensions, aborting");
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }
    if (!initRemovalDetectionLists(accountId)) {
        SOCIALD_LOG_ERROR("unable to initialized cached account list for account" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // call superclass impl.
    DropboxDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void DropboxImageSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    m_db.purgeAccount(oldId);
    m_db.commit();
    m_db.wait();

    // manage image cache. Gallery UI caches full size images
    // and maintains bindings between source and cached image in SocialImageDatabase.
    // purge cached images belonging to this account.
    purgeCachedImages(&m_imageCacheDb, oldId);
}

void DropboxImageSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    possiblyAddNewUser(QString::number(accountId), accountId, accessToken);
    queryCameraRollCursor(accountId, accessToken);
}

void DropboxImageSyncAdaptor::finalize(int accountId)
{
    Q_UNUSED(accountId)

    if (syncAborted()) {
        SOCIALD_LOG_INFO("sync aborted, won't commit database changes");
    } else {
        // Remove albums
        m_db.removeAlbums(m_cachedAlbums.keys());

        // Remove images
        m_db.removeImages(m_removedImages);

        m_db.commit();
        m_db.wait();

        // manage image cache. Gallery UI caches full size images
        // and maintains bindings between source and cached image in SocialImageDatabase.
        // purge cached images older than two weeks.
        purgeExpiredImages(&m_imageCacheDb, accountId);
    }
}

void DropboxImageSyncAdaptor::queryCameraRollCursor(int accountId, const QString &accessToken)
{
    QJsonObject requestParameters;
    requestParameters.insert("path", "/Pictures");
    requestParameters.insert("recursive", false);
    requestParameters.insert("include_media_info", true);
    requestParameters.insert("include_deleted", false);
    requestParameters.insert("include_has_explicit_shared_members", false);
    QJsonDocument doc;
    doc.setObject(requestParameters);
    QByteArray postData = doc.toJson(QJsonDocument::Compact);

    QUrl url(QStringLiteral("%1/2/files/list_folder/get_latest_cursor").arg(api()));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::ContentLengthHeader, postData.size());
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    SOCIALD_LOG_DEBUG("querying camera roll cursor:" << url.toString());

    QNetworkReply *reply = m_networkAccessManager->post(req, postData);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(cameraRollCursorFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("unable to request data from Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void DropboxImageSyncAdaptor::cameraRollCursorFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString continuationUrl = reply->property("continuationUrl").toString();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);

    if (isError || !ok || parsed.contains("error")) {
        SOCIALD_LOG_ERROR("unable to read Pictures cursor response for Dropbox account with id" << accountId);
        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            SOCIALD_LOG_DEBUG("Possibly" << reply->request().url().toString()
                              << "is not available on server because no photos have been uploaded yet");
        }
        QString errorResponse = QString::fromUtf8(replyData);
        Q_FOREACH (const QString &line, errorResponse.split('\n')) {
            SOCIALD_LOG_DEBUG(line);
        }
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    QString cursor = parsed.value(QLatin1String("cursor")).toString();
    QString userId = QString::number(accountId);
    QString albumId = "DropboxPictures-" + userId; // in future we might have multiple dropbox accounts
    const DropboxAlbum::ConstPtr &dbAlbum = m_cachedAlbums.value(albumId);
    m_cachedAlbums.remove(albumId); // this album exists, so remove it from the removal detection delta.
    if (!dbAlbum.isNull() && dbAlbum->hash() == cursor) {
        SOCIALD_LOG_DEBUG("album with id" << albumId << "by user" << userId <<
                          "from Dropbox account with id" << accountId << "doesn't need sync");
        decrementSemaphore(accountId);
        return;
    }

    // some changes have occurred, we need to sync.
    queryCameraRoll(accountId, accessToken, albumId, cursor, QString());
    decrementSemaphore(accountId);
}

void DropboxImageSyncAdaptor::queryCameraRoll(int accountId, const QString &accessToken, const QString &albumId, const QString &cursor, const QString &continuationCursor)
{
    QJsonObject requestParameters;
    if (continuationCursor.isEmpty()) {
        requestParameters.insert("path", "/Pictures");
        requestParameters.insert("include_media_info", true);
        requestParameters.insert("include_deleted", false);
        requestParameters.insert("include_has_explicit_shared_members", false);
    } else {
        requestParameters.insert("cursor", continuationCursor);
    }
    QJsonDocument doc;
    doc.setObject(requestParameters);
    QByteArray postData = doc.toJson(QJsonDocument::Compact);

    QUrl url;
    if (continuationCursor.isEmpty()) {
        url = QUrl(QStringLiteral("%1/2/files/list_folder").arg(api()));
    } else {
        url = QUrl(QStringLiteral("%1/2/files/list_folder_continue").arg(api()));
    }
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setHeader(QNetworkRequest::ContentLengthHeader, postData.size());
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    SOCIALD_LOG_DEBUG("querying camera roll:" << url.toString());

    QNetworkReply *reply = m_networkAccessManager->post(req, postData);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("albumId", albumId);
        reply->setProperty("cursor", cursor);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(cameraRollFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("unable to request data from Dropbox account with id" << accountId);
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void DropboxImageSyncAdaptor::cameraRollFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString albumId = reply->property("albumId").toString();
    QString cursor = reply->property("cursor").toString();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

qWarning() << "Got replyData:" << replyData;

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);

    if (isError || !ok || parsed.contains("error")) {
        SOCIALD_LOG_ERROR("unable to read albums response for Dropbox account with id" << accountId);
        if (reply->error() == QNetworkReply::ContentNotFoundError) {
            SOCIALD_LOG_DEBUG("Possibly" << reply->request().url().toString()
                              << "is not available on server because no photos have been uploaded yet");
        }
        QString errorResponse = QString::fromUtf8(replyData);
        Q_FOREACH (const QString &line, errorResponse.split('\n')) {
            SOCIALD_LOG_DEBUG(line);
        }
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    // read the pictures information
    QJsonArray data = parsed.value(QLatin1String("entries")).toArray();
    for (int i = 0; i < data.size(); ++i) {
        QJsonObject fileObject = data.at(i).toObject();
        const QString &remoteFilePath = fileObject.value("path_display").toString();
        if (!fileObject.isEmpty() && filenameHasImageExtension(remoteFilePath)) {
            m_retrievedObjects.append(fileObject);
        }
    }

    QString continuationCursor = parsed.value(QLatin1String("cursor")).toString();
    bool hasMore = parsed.value(QLatin1String("has_more")).toBool();

    if (hasMore) {
        queryCameraRoll(accountId, accessToken, albumId, cursor, continuationCursor);
    } else {
        // we have retrieved all of the image file objects data.
        QString userId = QString::number(accountId);
        QString albumName = "Pictures"; // TODO: do we need to translate?
        m_db.syncAccount(accountId, userId);
        m_db.addAlbum(albumId, userId, QDateTime(), QDateTime(), albumName, m_retrievedObjects.size(), cursor);

        // process the objects and update the database.
        for (int i = 0; i < m_retrievedObjects.size(); ++i) {
            QJsonObject fileObject = m_retrievedObjects.at(i).toObject();
            const QString &remoteFilePath = fileObject.value("path_display").toString();
            QString photoId = fileObject.value(QLatin1String("rev")).toString();
            QString photoName = remoteFilePath.split("/").last();
            int imageWidth = fileObject.value(QLatin1String("media_info")).toObject().value(QLatin1String("dimensions")).toObject().value(QLatin1String("width")).toInt();
            int imageHeight = fileObject.value(QLatin1String("media_info")).toObject().value(QLatin1String("dimensions")).toObject().value(QLatin1String("height")).toInt();
            if (imageWidth == 0 || imageHeight == 0) {
                imageWidth = 768;
                imageHeight = 1024;
            }

            QString createdTimeStr = fileObject.value(QLatin1String("client_modified")).toString();
            QDateTime createdTime = QDateTime::fromString(createdTimeStr, Qt::ISODate);
            QString updatedTimeStr = fileObject.value(QLatin1String("server_modified")).toString();
            QDateTime updatedTime = QDateTime::fromString(updatedTimeStr, Qt::ISODate);
            if (!m_serverImageIds[albumId].contains(photoId)) {
                m_serverImageIds[albumId].insert(photoId);
            }

            QString thumbnailAPIUrl = content() + "/2/files/get_thumbnail";
            QString fileAPIUrl = content() + "/2/files/download";

            QString thumbnailSizeStr;
            if (m_optimalThumbnailWidth <= 32) {
                thumbnailSizeStr = "w32h32";
            } else if (m_optimalThumbnailWidth <= 64) {
                thumbnailSizeStr = "w64h64";
            } else if (m_optimalThumbnailWidth <= 128) {
                thumbnailSizeStr = "w128h128";
            } else if (m_optimalThumbnailWidth <= 480) {
                thumbnailSizeStr = "w640h480";
            } else {
                thumbnailSizeStr = "w1024h768";
            }

            QJsonObject thumbnailQueryObject;
            thumbnailQueryObject.insert("path", remoteFilePath);
            thumbnailQueryObject.insert("format", "jpeg");
            thumbnailQueryObject.insert("size", thumbnailSizeStr);
            QByteArray thumbnailQueryArg = QJsonDocument(thumbnailQueryObject).toJson(QJsonDocument::Compact);
            QString thumbnailUrl = thumbnailAPIUrl + "?arg=" + QString::fromUtf8(thumbnailQueryArg.toPercentEncoding());

            QJsonObject fileQueryObject;
            fileQueryObject.insert("path", remoteFilePath);
            QByteArray fileQueryArg = QJsonDocument(fileQueryObject).toJson(QJsonDocument::Compact);
            QString imageSrcUrl = fileAPIUrl + "?arg=" + QString::fromUtf8(fileQueryArg.toPercentEncoding());

            // check if we need to sync, and write to the database.
            if (haveAlreadyCachedImage(photoId, imageSrcUrl)) {
                SOCIALD_LOG_DEBUG("have previously cached photo" << photoId << ":" << imageSrcUrl);
            } else {
                SOCIALD_LOG_DEBUG("caching new photo" << photoId << ":" << imageSrcUrl << "->" << imageWidth << "x" << imageHeight);
                m_db.addImage(photoId, albumId, userId, createdTime, updatedTime,
                              photoName, imageWidth, imageHeight, thumbnailUrl, imageSrcUrl, accessToken);
            }
        }
        checkRemovedImages(albumId);
    }

    decrementSemaphore(accountId);
}

bool DropboxImageSyncAdaptor::haveAlreadyCachedImage(const QString &imageId, const QString &imageUrl)
{
    DropboxImage::ConstPtr dbImage = m_db.image(imageId);
    bool imagedbSynced = !dbImage.isNull();

    if (!imagedbSynced) {
        return false;
    }

    QString dbImageUrl = dbImage->imageUrl();
    if (dbImageUrl != imageUrl) {
        SOCIALD_LOG_ERROR("Image/dropbox.db has outdated data!\n"
                          "   photoId:" << imageId << "\n"
                          "   cached image url:" << dbImageUrl << "\n"
                          "   new image url:" << imageUrl);
        return false;
    }

    return true;
}

void DropboxImageSyncAdaptor::possiblyAddNewUser(const QString &userId, int accountId,
                                                  const QString &accessToken)
{
    if (!m_db.user(userId).isNull()) {
        return;
    }

    // We need to add the user. We call Dropbox to get the informations that we
    // need and then add it to the database https://api.dropboxapi.com/2/users/get_current_account

    QUrl url(QStringLiteral("%1/2/users/get_current_account").arg(api()));
    QNetworkRequest req;
    req.setUrl(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    SOCIALD_LOG_DEBUG("querying Dropbox account info:" << url.toString());

    QNetworkReply *reply = m_networkAccessManager->post(req, QByteArray());
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)),
                this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)),
                this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(userFinishedHandler()));

        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    }
}

void DropboxImageSyncAdaptor::userFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    QByteArray replyData = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    disconnect(reply);
    reply->deleteLater();

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (!ok || !parsed.contains(QLatin1String("name"))) {
        SOCIALD_LOG_ERROR("unable to read user response for Dropbox account with id" << accountId);
        return;
    }

    // QString userId = parsed.value(QLatin1String("id")).toString();
    QJsonObject name = parsed.value(QLatin1String("name")).toObject();
    QString display_name = name.value(QLatin1String("display_name")).toString();
    if (display_name.isEmpty()) {
        SOCIALD_LOG_ERROR("unable to read user display name for Dropbox account with id" << accountId);
        return;
    }

    m_db.addUser(QString::number(accountId), QDateTime::currentDateTime(), display_name);
    decrementSemaphore(accountId);
}

bool DropboxImageSyncAdaptor::initRemovalDetectionLists(int accountId)
{
    // This function should be called as part of the ::sync() preamble.
    // Clear our internal state variables which we use to track server-side deletions.
    // We have to do it this way, as results can be spread across multiple requests
    // if Dropbox returns results in paginated form.
    clearRemovalDetectionLists();

    bool ok = false;
    QMap<int,QString> accounts = m_db.accounts(&ok);
    if (!ok) {
        return false;
    }
    if (accounts.contains(accountId)) {
        QString userId = accounts.value(accountId);

        QStringList allAlbumIds = m_db.allAlbumIds();
        foreach (const QString& albumId, allAlbumIds) {
            DropboxAlbum::ConstPtr album = m_db.album(albumId);
            if (album->userId() == userId) {
                m_cachedAlbums.insert(albumId, album);
            }
        }
    }

    return true;
}

void DropboxImageSyncAdaptor::clearRemovalDetectionLists()
{
    m_cachedAlbums.clear();
    m_serverImageIds.clear();
    m_removedImages.clear();
}

void DropboxImageSyncAdaptor::checkRemovedImages(const QString &albumId)
{
    const QSet<QString> &serverImageIds = m_serverImageIds.value(albumId);
    QSet<QString> cachedImageIds = m_db.imageIds(albumId).toSet();

    foreach (const QString &imageId, serverImageIds) {
        cachedImageIds.remove(imageId);
    }

    m_removedImages.append(cachedImageIds.toList());
}

bool DropboxImageSyncAdaptor::determineOptimalDimensions()
{
    int width = 0, height = 0;
    const int defaultValue = 0;
    MGConfItem widthConf("/lipstick/screen/primary/width");
    if (widthConf.value(defaultValue).toInt() != defaultValue) {
        width = widthConf.value(defaultValue).toInt();
    }
    MGConfItem heightConf("/lipstick/screen/primary/height");
    if (heightConf.value(defaultValue).toInt() != defaultValue) {
        height = heightConf.value(defaultValue).toInt();
    }

    // we want to use the largest of these dimensions as the "optimal"
    int maxDimension = qMax(width, height);
    if (maxDimension % 3 == 0) {
        m_optimalThumbnailWidth = maxDimension / 3;
    } else {
        m_optimalThumbnailWidth = (maxDimension / 2);
    }
    m_optimalImageWidth = maxDimension;
    SOCIALD_LOG_DEBUG("Determined optimal image dimension:" << m_optimalImageWidth << ", thumbnail:" << m_optimalThumbnailWidth);
    return true;
}
