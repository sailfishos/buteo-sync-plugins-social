/****************************************************************************
 **
 ** Copyright (C) 2013-2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
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

#include "facebookimagesyncadaptor.h"
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

#include <MDConfItem>

// Update the following version if database schema changes e.g. new
// fields are added to the existing tables.
// It will make old tables dropped and creates new ones.

// Currently, we integrate with the device image gallery via saving thumbnails to the
// ~/.config/sociald/images directory, and filling the ~/.config/sociald/images/facebook.db
// with appropriate data.

// TODO: there is still issues with multiaccount, if an user adds two times the same
// account, it might have some problems, like data being removed while it shouldn't.
FacebookImageSyncAdaptor::FacebookImageSyncAdaptor(QObject *parent)
    : FacebookDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Images, parent)
    , m_optimalThumbnailWidth(0)
    , m_optimalImageWidth(0)
{
    setInitialActive(m_db.isValid());
}


FacebookImageSyncAdaptor::~FacebookImageSyncAdaptor()
{
}

QString FacebookImageSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("facebook-images");
}

void FacebookImageSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    // get ready for sync
    if (!determineOptimalDimensions()) {
        qCWarning(lcSocialPlugin) << "unable to determine optimal image dimensions, aborting";
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }
    if (!initRemovalDetectionLists(accountId)) {
        qCWarning(lcSocialPlugin) << "unable to initialized cached account list for account" << accountId;
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // call superclass impl.
    FacebookDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void FacebookImageSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    m_db.purgeAccount(oldId);
    m_db.commit();
    m_db.wait();
}

void FacebookImageSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    // XXX TODO: use a sync queue.  One for accounts + one for albums.
    // Finish all images from a single album, etc on down.
    // That way we don't request anything "out of order" which can screw up Facebook's paging etc stuff.
    // Downside: much slower, since more (network) IO bound than previously.
    requestData(accountId, accessToken, QString(), QString(), QString());
}

void FacebookImageSyncAdaptor::finalize(int accountId)
{
    Q_UNUSED(accountId)

    if (syncAborted()) {
        qCInfo(lcSocialPlugin) << "sync aborted, won't commit database changes";
    } else {
        // Remove albums
        m_db.removeAlbums(m_cachedAlbums.keys());

        // Remove images
        m_db.removeImages(m_removedImages);

        m_db.commit();
        m_db.wait();
    }
}

void FacebookImageSyncAdaptor::requestData(int accountId,
                                           const QString &accessToken,
                                           const QString &continuationUrl,
                                           const QString &fbUserId,
                                           const QString &fbAlbumId)
{
    if (syncAborted()) {
        qCDebug(lcSocialPlugin) << "skipping data request due to sync abort";
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        return;
    }

    QUrl url;
    if (!continuationUrl.isEmpty()) {
        // fetch the next page.
        url = QUrl(continuationUrl);
    } else {
        // build the request, depending on whether we're fetching albums or images.
        if (fbAlbumId.isEmpty()) {
            // fetching all albums from the me user.
            url = QUrl(graphAPI(QLatin1String("/me/albums")));
        } else {
            // fetching images from a particular album.
            url = QUrl(graphAPI(QString(QLatin1String("/%1/photos")).arg(fbAlbumId)));
        }
    }

    // if the url already contains query part (in which case it is continuationUrl), don't overwrite it.
    if (!url.hasQuery()) {
        QList<QPair<QString, QString> > queryItems;
        QUrlQuery query(url);
        queryItems.append(QPair<QString, QString>(QString(QLatin1String("access_token")), accessToken));
        queryItems.append(QPair<QString, QString>(QString(QLatin1String("limit")), QString(QLatin1String("2000"))));
        if (fbAlbumId.isEmpty()) {
            queryItems.append(QPair<QString, QString>(QString(QLatin1String("fields")),
                                                      QString(QLatin1String("id,from,name,created_time,updated_time,count"))));
        } else {
            queryItems.append(QPair<QString, QString>(QString(QLatin1String("fields")),
                                                      QString(QLatin1String("id,picture,source,images,width,height,created_time,updated_time,name"))));
        }
        query.setQueryItems(queryItems);
        url.setQuery(query);
    }

    QNetworkReply *reply = m_networkAccessManager->get(QNetworkRequest(url));
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("fbUserId", fbUserId);
        reply->setProperty("fbAlbumId", fbAlbumId);
        reply->setProperty("continuationUrl", continuationUrl);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        if (fbAlbumId.isEmpty()) {
            connect(reply, SIGNAL(finished()), this, SLOT(albumsFinishedHandler()));
        } else {
            connect(reply, SIGNAL(finished()), this, SLOT(imagesFinishedHandler()));
        }

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        qCWarning(lcSocialPlugin) << "unable to request data from Facebook account with id" << accountId;
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
    }
}

void FacebookImageSyncAdaptor::albumsFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString fbUserId = reply->property("fbUserId").toString();
    QString fbAlbumId = reply->property("fbAlbumId").toString();
    QString continuationUrl = reply->property("continuationUrl").toString();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (isError || !ok || !parsed.contains(QLatin1String("data"))) {
        qCWarning(lcSocialPlugin) << "unable to read albums response for Facebook account with id" << accountId;
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    QJsonArray data = parsed.value(QLatin1String("data")).toArray();
    if (data.size() == 0) {
        qCDebug(lcSocialPlugin) << "Facebook account with id" << accountId << "has no albums";
        decrementSemaphore(accountId);
        return;
    }

    // read the albums information
    for (int i = 0; i < data.size(); ++i) {
        QJsonObject albumObject = data.at(i).toObject();
        if (albumObject.isEmpty()) {
            continue;
        }

        QString albumId = albumObject.value(QLatin1String("id")).toString();
        QString userId = albumObject.value(QLatin1String("from")).toObject().value(QLatin1String("id")).toString();
        if (!userId.isEmpty() && userId != fbUserId) {
            // probably because the fbUserId hasn't been filled yet.
            fbUserId = userId;
            m_db.syncAccount(accountId, fbUserId);
        }
        if (!albumId.isEmpty() && albumId != fbAlbumId) {
            // probably because the fbAlbumId hasn't been filled yet.
            fbAlbumId = albumId;
        }

        QString albumName = albumObject.value(QLatin1String("name")).toString();
        QString createdTimeStr = albumObject.value(QLatin1String("created_time")).toString();
        QString updatedTimeStr = albumObject.value(QLatin1String("updated_time")).toString();
        int imageCount = static_cast<int>(albumObject.value(QLatin1String("count")).toDouble());
        qCDebug(lcSocialPlugin) << "Got album information:" << userId << albumId << albumName
                                << createdTimeStr << updatedTimeStr << imageCount;

        // check to see whether we need to sync (any changes since last sync)
        // Note that we also check if the image count is the same, since, when
        // removing an image, the updatedTime is not changed
        QDateTime createdTime = QDateTime::fromString(createdTimeStr, Qt::ISODate);
        QDateTime updatedTime = QDateTime::fromString(updatedTimeStr, Qt::ISODate);

        const FacebookAlbum::ConstPtr &dbAlbum = m_cachedAlbums.value(fbAlbumId);
        m_cachedAlbums.remove(fbAlbumId);  // Removal detection
        if (!dbAlbum.isNull() && (dbAlbum->updatedTime() >= updatedTime
                                  && dbAlbum->imageCount() == imageCount)) {
            qCDebug(lcSocialPlugin) << "album with id" << albumId << "by user" << userId
                                    << "from Facebook account with id" << accountId << "doesn't need sync";
            continue;
        }

        // We need to sync. We save the album entry, and request the images for the album.
        // When saving the album, we might need to add a new user
        possiblyAddNewUser(userId, accountId, accessToken);

        // We then save the album
        m_db.addAlbum(albumId, userId, createdTime, updatedTime, albumName, imageCount);
        // TODO: After successfully added an album, we should begin a new query to get the image
        // information (based on cover image id).
        requestData(accountId, accessToken, QString(), fbUserId, fbAlbumId);
    }

    // Perform a continuation request if required.
    QJsonObject paging = parsed.value(QLatin1String("paging")).toObject();
    QString nextUrl = paging.value(QLatin1String("next")).toString();
    if (!nextUrl.isEmpty() && nextUrl != continuationUrl) {
        // note: we check equality because fb can return spurious paging data...
        qCDebug(lcSocialPlugin) << "performing continuation request for more albums for Facebook account with id"
                                << accountId << ":" << nextUrl;
        requestData(accountId, accessToken, nextUrl, fbUserId, QString());
    }

    // Finally, reduce our semaphore.
    decrementSemaphore(accountId);
}

void FacebookImageSyncAdaptor::imagesFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString fbUserId = reply->property("fbUserId").toString();
    QString fbAlbumId = reply->property("fbAlbumId").toString();
    QString continuationUrl = reply->property("continuationUrl").toString();
    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (isError || !ok || !parsed.contains(QLatin1String("data"))) {
        qCWarning(lcSocialPlugin) << "unable to read photos response for Facebook account with id" << accountId;
        qCDebug(lcSocialPlugin) << replyData;
        clearRemovalDetectionLists(); // don't perform server-side removal detection during this sync run.
        decrementSemaphore(accountId);
        return;
    }

    QJsonArray data = parsed.value(QLatin1String("data")).toArray();
    if (data.size() == 0) {
        qCDebug(lcSocialPlugin) << "album with id" << fbAlbumId << "from Facebook account with id" << accountId << "has no photos";
        checkRemovedImages(fbAlbumId);
        decrementSemaphore(accountId);
        return;
    }

    // read the photos information
    foreach (const QJsonValue &photoValue, data) {
        QJsonObject imageObject = photoValue.toObject();
        if (imageObject.isEmpty()) {
            continue;
        }

        QString photoId = imageObject.value(QLatin1String("id")).toString();
        QString thumbnailUrl = imageObject.value(QLatin1String("picture")).toString();
        QString imageSrcUrl = imageObject.value(QLatin1String("source")).toString();
        QString createdTimeStr = imageObject.value(QLatin1String("created_time")).toString();
        QString updatedTimeStr = imageObject.value(QLatin1String("updated_time")).toString();
        QString photoName = imageObject.value(QLatin1String("name")).toString();
        int imageWidth = 0;
        int imageHeight = 0;
        if (photoId.isEmpty()) {
            qCWarning(lcSocialPlugin) << "Unable to parse photo id from data:" << photoValue.toObject().toVariantMap();
            continue;
        }

        // Find optimal thumbnail and image source urls based on dimensions.
        QList<ImageSource> imageSources;
        QJsonArray images = imageObject.value(QLatin1String("images")).toArray();
        foreach (const QJsonValue &imageValue, images) {
            QJsonObject image = imageValue.toObject();
            imageSources << ImageSource(static_cast<int>(image.value(QLatin1String("width")).toDouble()),
                                        static_cast<int>(image.value(QLatin1String("height")).toDouble()),
                                        image.value(QLatin1String("source")).toString());
        }

        bool foundOptimalThumbnail = false, foundOptimalImage = false;
        std::sort(imageSources.begin(), imageSources.end());
        Q_FOREACH (const ImageSource &img, imageSources) {
            if (!foundOptimalThumbnail && qMin(img.width, img.height) >= m_optimalThumbnailWidth) {
                foundOptimalThumbnail = true;
                thumbnailUrl = img.sourceUrl;
            }
            if (!foundOptimalImage && qMin(img.width, img.height) >= m_optimalImageWidth) {
                foundOptimalImage = true;
                imageWidth = img.width;
                imageHeight = img.height;
                imageSrcUrl = img.sourceUrl;
            }
        }
        if (!foundOptimalThumbnail && imageSources.size()) {
            // just choose the largest one.
            thumbnailUrl = imageSources.last().sourceUrl;
        }
        if (!foundOptimalImage && imageSources.size()) {
            // just choose the largest one.
            imageSrcUrl = imageSources.last().sourceUrl;
            imageWidth = imageSources.last().width;
            imageHeight = imageSources.last().height;
        }

        QDateTime createdTime = QDateTime::fromString(createdTimeStr, Qt::ISODate);
        QDateTime updatedTime = QDateTime::fromString(updatedTimeStr, Qt::ISODate);
        if (!m_serverImageIds[fbAlbumId].contains(photoId)) {
            m_serverImageIds[fbAlbumId].insert(photoId);
        }

        // check if we need to sync, and write to the database.
        if (!imageSrcUrl.isEmpty()) {
            if (haveAlreadyCachedImage(photoId, imageSrcUrl)) {
                qCDebug(lcSocialPlugin) << "have previously cached photo" << photoId << ":" << imageSrcUrl;
            } else {
                qCDebug(lcSocialPlugin) << "caching new photo" << photoId << ":" << imageSrcUrl << "->" << imageWidth << "x" << imageHeight;
                m_db.addImage(photoId, fbAlbumId, fbUserId, createdTime, updatedTime,
                              photoName, imageWidth, imageHeight, thumbnailUrl, imageSrcUrl);
            }
        } else {
            qCWarning(lcSocialPlugin) << "Cannot add photo to database:" << photoId << "- empty image source url!";
        }
    }
    // perform a continuation request if required.
    QJsonObject paging = parsed.value(QLatin1String("paging")).toObject();
    QString nextUrl = paging.value(QLatin1String("next")).toString();
    if (!nextUrl.isEmpty() && nextUrl != continuationUrl) {
        qCDebug(lcSocialPlugin) << "performing continuation request for more photos for Facebook account with id" << accountId << ":" << nextUrl;
        requestData(accountId, accessToken, nextUrl, fbUserId, fbAlbumId);
    } else {
        // this was the laste page, check removed images
        checkRemovedImages(fbAlbumId);
    }

    // we're finished this request.  Decrement our busy semaphore.
    decrementSemaphore(accountId);
}

bool FacebookImageSyncAdaptor::haveAlreadyCachedImage(const QString &fbImageId, const QString &imageUrl)
{
    FacebookImage::ConstPtr dbImage = m_db.image(fbImageId);
    bool imagedbSynced = !dbImage.isNull();

    if (!imagedbSynced) {
        return false;
    }

    QString dbImageUrl = dbImage->imageUrl();
    if (dbImageUrl != imageUrl) {
        qCWarning(lcSocialPlugin) << "Image/facebook.db has outdated data!\n"
                          "   fbPhotoId:" << fbImageId << "\n"
                          "   cached image url:" << dbImageUrl << "\n"
                          "   new image url:" << imageUrl;
        return false;
    }

    return true;
}

void FacebookImageSyncAdaptor::possiblyAddNewUser(const QString &fbUserId, int accountId,
                                                  const QString &accessToken)
{
    if (!m_db.user(fbUserId).isNull()) {
        return;
    }

    // We need to add the user. We call Facebook to get the informations that we
    // need and then add it to the database
    // me?fields=updated_time,name,picture
    QUrl url(graphAPI(QLatin1String("/me")));
    QList<QPair<QString, QString> > queryItems;
    queryItems.append(QPair<QString, QString>(QString(QLatin1String("access_token")), accessToken));
    queryItems.append(QPair<QString, QString>(QString(QLatin1String("fields")),
                                              QLatin1String("id,updated_time,name,picture")));
    QUrlQuery query(url);
    query.setQueryItems(queryItems);
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccessManager->get(QNetworkRequest(url));
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

void FacebookImageSyncAdaptor::userFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    QByteArray replyData = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    disconnect(reply);
    reply->deleteLater();

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);
    if (!ok || !parsed.contains(QLatin1String("id"))) {
        qCWarning(lcSocialPlugin) << "unable to read user response for Facebook account with id" << accountId;
        return;
    }

    QString fbUserId = parsed.value(QLatin1String("id")).toString();
    QString fbName = parsed.value(QLatin1String("name")).toString();
    QString updatedStr = parsed.value(QLatin1String("updated_time")).toString();

    m_db.addUser(fbUserId, QDateTime::fromString(updatedStr, Qt::ISODate), fbName);
    decrementSemaphore(accountId);
}

bool FacebookImageSyncAdaptor::initRemovalDetectionLists(int accountId)
{
    // This function should be called as part of the ::sync() preamble.
    // Clear our internal state variables which we use to track server-side deletions.
    // We have to do it this way, as results can be spread across multiple requests
    // if Facebook returns results in paginated form.
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
            FacebookAlbum::ConstPtr album = m_db.album(albumId);
            if (album->fbUserId() == userId) {
                m_cachedAlbums.insert(albumId, album);
            }
        }
    }

    return true;
}

void FacebookImageSyncAdaptor::clearRemovalDetectionLists()
{
    m_cachedAlbums.clear();
    m_serverImageIds.clear();
    m_removedImages.clear();
}

void FacebookImageSyncAdaptor::checkRemovedImages(const QString &fbAlbumId)
{
    const QSet<QString> &serverImageIds = m_serverImageIds.value(fbAlbumId);
    QSet<QString> cachedImageIds = m_db.imageIds(fbAlbumId).toSet();

    foreach (const QString &fbImageId, serverImageIds) {
        cachedImageIds.remove(fbImageId);
    }

    m_removedImages.append(cachedImageIds.toList());
}

bool FacebookImageSyncAdaptor::determineOptimalDimensions()
{
    int width = 0, height = 0;
    const int defaultValue = 0;
    MDConfItem widthConf("/lipstick/screen/primary/width");
    if (widthConf.value(defaultValue).toInt() != defaultValue) {
        width = widthConf.value(defaultValue).toInt();
    }
    MDConfItem heightConf("/lipstick/screen/primary/height");
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
    qCDebug(lcSocialPlugin) << "Determined optimal image dimension:" << m_optimalImageWidth << ", thumbnail:" << m_optimalThumbnailWidth;
    return true;
}
