/****************************************************************************
 **
 ** Copyright (C) 2015 Jolla Ltd.
 ** Contact: Antti Seppälä <antti.seppala@jollamobile.com>
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

#include "onedriveimagesyncadaptor.h"
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

OneDriveImageSyncAdaptor::AlbumData::AlbumData()
{
}

OneDriveImageSyncAdaptor::AlbumData::AlbumData(
        const QString &albumId, const QString &userId,
        const QDateTime &createdTime, const QDateTime &updatedTime,
        const QString &albumName, int imageCount)
    : albumId(albumId), userId(userId), createdTime(createdTime)
    , updatedTime(updatedTime), albumName(albumName), imageCount(imageCount)
{
}

OneDriveImageSyncAdaptor::AlbumData::AlbumData(const AlbumData &other)
{
    albumId = other.albumId;
    userId = other.userId;
    createdTime = other.createdTime;
    updatedTime = other.updatedTime;
    albumName = other.albumName;
    imageCount = other.imageCount;
}

OneDriveImageSyncAdaptor::ImageData::ImageData()
    : imageWidth(0), imageHeight(0)
{
}

OneDriveImageSyncAdaptor::ImageData::ImageData(
        const QString &photoId, const QString &albumId, const QString &userId,
        const QDateTime &createdTime, const QDateTime &updatedTime,
        const QString &photoName, int imageWidth, int imageHeight,
        const QString &thumbnailUrl, const QString &imageSourceUrl,
        const QString &description)
    : photoId(photoId), albumId(albumId), userId(userId)
    , createdTime(createdTime), updatedTime(updatedTime), photoName(photoName)
    , imageWidth(imageWidth), imageHeight(imageHeight)
    , thumbnailUrl(thumbnailUrl), imageSourceUrl(imageSourceUrl)
    , description(description)
{
}

OneDriveImageSyncAdaptor::ImageData::ImageData(const ImageData &other)
{
    photoId = other.photoId;
    albumId = other.albumId;
    userId = other.userId;
    createdTime = other.createdTime;
    updatedTime = other.updatedTime;
    photoName = other.photoName;
    imageWidth = other.imageWidth;
    imageHeight = other.imageHeight;
    thumbnailUrl = other.thumbnailUrl;
    imageSourceUrl = other.imageSourceUrl;
    description = other.description;
}

// Update the following version if database schema changes e.g. new
// fields are added to the existing tables.
// It will make old tables dropped and creates new ones.

// Currently, we integrate with the device image gallery via saving thumbnails to the
// ~/.config/sociald/images directory, and filling the ~/.config/sociald/images/onedrive.db
// with appropriate data.

OneDriveImageSyncAdaptor::OneDriveImageSyncAdaptor(QObject *parent)
    : OneDriveDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Images, parent)
{
    setInitialActive(m_db.isValid());
}

OneDriveImageSyncAdaptor::~OneDriveImageSyncAdaptor()
{
}

QString OneDriveImageSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("onedrive-images");
}

void OneDriveImageSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    if (!initRemovalDetectionLists(accountId)) {
        qCWarning(lcSocialPlugin) << "unable to initialized cached account list for account" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // call superclass impl.
    OneDriveDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void OneDriveImageSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    m_db.purgeAccount(oldId);
    m_db.commit();
    m_db.wait();

    // manage image cache. Gallery UI caches full size images
    // and maintains bindings between source and cached image in SocialImageDatabase.
    // purge cached images belonging to this account.
    purgeCachedImages(&m_imageCacheDb, oldId);
}

void OneDriveImageSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    requestResource(accountId, accessToken);
}

void OneDriveImageSyncAdaptor::finalize(int accountId)
{
    if (syncAborted()) {
        qCInfo(lcSocialPlugin) << "sync aborted, won't commit database changes";
    } else if (m_userId.isEmpty()) {
        qCWarning(lcSocialPlugin) << "no user id determined during sync, aborting";
    } else {
        // Add user
        if (m_db.user(m_userId).isNull()) {
            m_db.addUser(m_userId, QDateTime::currentDateTime(), m_userDisplayName, accountId);
        }

        // Add/update albums
        QMap<QString, AlbumData>::const_iterator i;
        for (i = m_albumData.constBegin(); i != m_albumData.constEnd(); ++i) {
            const AlbumData &data = i.value();
            m_db.addAlbum(data.albumId, data.userId, data.createdTime,
                          data.updatedTime, data.albumName, data.imageCount);
        }

        // Remove albums
        m_db.removeAlbums(m_cachedAlbums.keys());

        // determine whether any images have been removed server-side.
        Q_FOREACH (const QString &albumId, m_seenAlbums) {
            checkRemovedImages(albumId);
        }

        // Remove images
        m_db.removeImages(m_removedImages);

        // Add/update images
        QMap<QString, ImageData>::const_iterator j;
        for (j = m_imageData.constBegin(); j != m_imageData.constEnd(); ++j) {
            const ImageData &data = j.value();
            m_db.addImage(data.photoId, data.albumId, data.userId,
                          data.createdTime, data.updatedTime,
                          data.photoName, data.imageWidth, data.imageHeight,
                          data.thumbnailUrl, data.imageSourceUrl, data.description,
                          accountId);
        }

        m_db.commit();
        m_db.wait();

        // manage image cache. Gallery UI caches full size images
        // and maintains bindings between source and cached image in SocialImageDatabase.
        // purge cached images older than two weeks.
        purgeExpiredImages(&m_imageCacheDb, accountId);
    }
}

void OneDriveImageSyncAdaptor::requestResource(int accountId, const QString &accessToken, const QString &resourceTarget)
{
    // TODO: in future, do a "first pass" WITHOUT child expansion to detect changed ctag/etag.
    //       then, do a "second pass" only for the albums which have changed ctag.
    const QString defaultResourceTarget = QStringLiteral("/drive/special/photos");
    const QUrl url(QStringLiteral("%1%2%3?expand=children(expand=thumbnails)")
                             .arg(api()).arg("/me").arg(resourceTarget.isEmpty()
                                                        ? defaultResourceTarget
                                                        : resourceTarget));
    qCDebug(lcSocialPlugin) << "OneDrive image sync requesting resource:" << url.toString();
    QNetworkRequest req(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());
    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("defaultResource", resourceTarget.isEmpty());
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(resourceFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        qCWarning(lcSocialPlugin) << "unable to request data from OneDrive account with id" << accountId;
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void OneDriveImageSyncAdaptor::requestNextLink(int accountId, const QString &accessToken, const QString &nextLink, bool isDefaultResource)
{
    qCDebug(lcSocialPlugin) << "OneDrive image sync requesting nextlink resources:" << nextLink;
    QUrl nextLinkUrl(nextLink);
    QNetworkRequest req(nextLinkUrl);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());
    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("defaultResource", isDefaultResource);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(resourceFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        qCWarning(lcSocialPlugin) << "unable to request data from OneDrive account with id" << accountId;
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void OneDriveImageSyncAdaptor::resourceFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    const bool isError = reply->property("isError").toBool();
    const int accountId = reply->property("accountId").toInt();
    const QString accessToken = reply->property("accessToken").toString();
    const bool defaultResource = reply->property("defaultResource").toBool();
    const QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    const QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (isError || !ok || !parsed.contains(QLatin1String("id"))) {
        qCWarning(lcSocialPlugin) << "Unable to parse query response for OneDrive account with id" << accountId;
        qCDebug(lcSocialPlugin) << "Received response data:" << replyData;
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    const QJsonObject userObj = parsed.value("createdBy").toObject().value("user").toObject();
    if (defaultResource) {
        m_userDisplayName = userObj.value("displayName").toString();
        m_userId = userObj.value("id").toString();
        if (m_userId.isEmpty()) {
            qCDebug(lcSocialPlugin) << "Unable to determine user id for default resource, aborting";
            decrementSemaphore(accountId);
            return;
        }
        m_db.syncAccount(accountId, m_userId);
    } else if (m_userId != userObj.value("id").toString()) {
        // ignore this album, not created by the current user.
        qCDebug(lcSocialPlugin) << "Ignoring album" << parsed.value("name").toString() << " - different user.";
        decrementSemaphore(accountId);
        return;
    }

    const QJsonObject fileSystemInfo = parsed.value("fileSystemInfo").toObject();

    const QString albumId = parsed.value("id").toString();
    const QDateTime createdTime = QDateTime::fromString(fileSystemInfo.value("createdDateTime").toString(), Qt::ISODate);
    const QDateTime updatedTime = QDateTime::fromString(fileSystemInfo.value("lastModifiedDateTime").toString(), Qt::ISODate);
    const QString albumName = parsed.value("name").toString();
    int photoCount = 0;

    const QJsonArray children = parsed.value("children").toArray();
    for (int i = 0; i < children.size(); ++i) {
        const QJsonObject child = children.at(i).toObject();
        if (child.contains("folder")) {
            const QJsonObject parentReference = child.value("parentReference").toObject();
            const QString onedriveResourceTarget = QStringLiteral("%1/%2").arg(parentReference.value("path").toString(), child.value("name").toString());
            qCDebug(lcSocialPlugin) << "Found subfolder:" << child.value("name").toString() << "of folder:" << albumName;
            requestResource(accountId, accessToken, onedriveResourceTarget);
        } else if (child.contains("image") && child.contains("@microsoft.graph.downloadUrl")) {
            photoCount++;
            const QJsonArray thumbnails = child.value("thumbnails").toArray();
            const QJsonObject thumbnail = thumbnails.size() ? thumbnails.at(0).toObject() : QJsonObject();
            const QString photoId = child.value("id").toString();
            const QDateTime photoCreatedTime = QDateTime::fromString(child.value("createdDateTime").toString(), Qt::ISODate);
            const QDateTime photoUpdatedTime = QDateTime::fromString(child.value("lastModifiedDateTime").toString(), Qt::ISODate);
            const QString photoName = child.value("name").toString();
            const int imageWidth = child.value("image").toObject().value("width").toInt();
            const int imageHeight = child.value("image").toObject().value("height").toInt();
            const QString photoThumbnailUrl = thumbnail.value("medium").toObject().value("url").toString();
            const QString photoImageSrcUrl = QStringLiteral("%1%2%3%4%5")
                    .arg(api()).arg("/me").arg("/drive/items/").arg(photoId).arg("/content");
            const QString photoDescription = child.value("description").toString();
            const ImageData image(photoId, albumId, m_userId, photoCreatedTime, photoUpdatedTime, photoName,
                                  imageWidth, imageHeight, photoThumbnailUrl, photoImageSrcUrl, photoDescription);

            // record the fact that we've seen this photo in this album
            m_serverAlbumImageIds[albumId].insert(photoId);

            // now check to see if this image has changed server-side
            const OneDriveImage::ConstPtr &dbImage = m_db.image(photoId);
            if (dbImage.isNull()
                    || dbImage->imageId() != photoId
                    || dbImage->createdTime().toTime_t() < photoCreatedTime.toTime_t()
                    || dbImage->updatedTime().toTime_t() < photoUpdatedTime.toTime_t()) {
                // changed, need to update in our local db.
                qCDebug(lcSocialPlugin) << "Image:" << photoName << "in folder:" << albumName << "added or changed on server";
                m_imageData.insert(photoId, image);
            }
        }
    }

    const OneDriveAlbum::ConstPtr &dbAlbum = m_cachedAlbums.value(albumId);
    m_cachedAlbums.remove(albumId); // Removal detection
    m_seenAlbums.insert(albumId);
    if (!dbAlbum.isNull() && (dbAlbum->updatedTime().toTime_t() >= updatedTime.toTime_t())) {
        qCDebug(lcSocialPlugin) << "album with id" << albumId << "by user" << m_userId <<
                          "from OneDrive account with id" << accountId << "doesn't need update";
    } else {
        qCDebug(lcSocialPlugin) << "Album:" << albumName << "added or changed on server";
        const AlbumData album(albumId, m_userId, createdTime, updatedTime, albumName, photoCount);
        if (m_albumData.contains(albumId)) {
            // updating due to nextlink / continuation request
            m_albumData[albumId].imageCount += photoCount;
        } else {
            // new unseen album.
            m_albumData.insert(albumId, album);
        }
    }

    // if more than 200 children exist, the result will include a
    // continuation request / pagination request next-link URL.
    const QString nextLink = parsed.value("@odata.nextLink").toString();
    if (!nextLink.isEmpty()) {
        requestNextLink(accountId, accessToken, nextLink, defaultResource);
    }

    decrementSemaphore(accountId);
}

bool OneDriveImageSyncAdaptor::initRemovalDetectionLists(int accountId)
{
    // This function should be called as part of the ::sync() preamble.
    // Clear our internal state variables which we use to track server-side deletions.
    // We have to do it this way, as results can be spread across multiple requests
    // if OneDrive returns results in paginated form.
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
            OneDriveAlbum::ConstPtr album = m_db.album(albumId);
            if (album->userId() == userId) {
                m_cachedAlbums.insert(albumId, album);
            }
        }
    }

    return true;
}

void OneDriveImageSyncAdaptor::clearRemovalDetectionLists()
{
    m_cachedAlbums.clear();
    m_serverAlbumImageIds.clear();
    m_removedImages.clear();
}

void OneDriveImageSyncAdaptor::checkRemovedImages(const QString &albumId)
{
    const QSet<QString> &serverImageIds = m_serverAlbumImageIds.value(albumId);
    QSet<QString> dbImageIds = m_db.imageIds(albumId).toSet();

    foreach (const QString &imageId, serverImageIds) {
        dbImageIds.remove(imageId);
    }

    m_removedImages.append(dbImageIds.toList());
}
