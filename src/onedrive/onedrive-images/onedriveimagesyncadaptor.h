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

#ifndef ONEDRIVEIMAGESYNCADAPTOR_H
#define ONEDRIVEIMAGESYNCADAPTOR_H

#include "onedrivedatatypesyncadaptor.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QDateTime>
#include <QtCore/QVariantMap>
#include <QtCore/QList>
#include <QtSql/QSqlDatabase>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>

#include <socialcache/onedriveimagesdatabase.h>
#include <socialcache/socialimagesdatabase.h>

class OneDriveImageSyncAdaptor : public OneDriveDataTypeSyncAdaptor
{
    Q_OBJECT

public:
    OneDriveImageSyncAdaptor(QObject *parent);
    ~OneDriveImageSyncAdaptor();

    QString syncServiceName() const;
    void sync(const QString &dataTypeString, int accountId);

protected: // implementing OneDriveDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode);
    void beginSync(int accountId, const QString &accessToken);
    void finalize(int accountId);

private:
    void requestResource(int accountId, const QString &accessToken, const QString &onedriveResource = QString());
    void requestNextLink(int accountId, const QString &accessToken, const QString &nextLink, bool isDefaultResource);

private Q_SLOTS:
    void resourceFinishedHandler();

private:
    struct AlbumData {
        AlbumData();
        AlbumData(const QString &albumId,
                  const QString &userId,
                  const QDateTime &createdTime,
                  const QDateTime &updatedTime,
                  const QString &albumName,
                  int imageCount);
        AlbumData(const AlbumData &other);

        QString albumId;
        QString userId;
        QDateTime createdTime;
        QDateTime updatedTime;
        QString albumName;
        int imageCount;
    };

    struct ImageData {
        ImageData();
        ImageData(const QString &photoId,
                  const QString &albumId,
                  const QString &userId,
                  const QDateTime &createdTime,
                  const QDateTime &updatedTime,
                  const QString &photoName,
                  int imageWidth,
                  int imageHeight,
                  const QString &thumbnailUrl,
                  const QString &imageSourceUrl,
                  const QString &description);
        ImageData(const ImageData &other);

        QString photoId;
        QString albumId;
        QString userId;
        QDateTime createdTime;
        QDateTime updatedTime;
        QString photoName;
        int imageWidth;
        int imageHeight;
        QString thumbnailUrl;
        QString imageSourceUrl;
        QString description;
    };

private:
    // for server-side removal detection.
    bool initRemovalDetectionLists(int accountId);
    void clearRemovalDetectionLists();
    void checkRemovedImages(const QString &fbAlbumId);
    QMap<QString, OneDriveAlbum::ConstPtr> m_cachedAlbums;
    QSet<QString> m_seenAlbums;
    QMap<QString, AlbumData> m_albumData;
    QMap<QString, ImageData> m_imageData;
    QMap<QString, QSet<QString> > m_serverAlbumImageIds;
    QStringList m_removedImages;

    QString m_userId;
    QString m_userDisplayName;

    OneDriveImagesDatabase m_db;
    SocialImagesDatabase m_imageCacheDb;
};

#endif // ONEDRIVEIMAGESYNCADAPTOR_H
