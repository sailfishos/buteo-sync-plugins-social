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

#ifndef DROPBOXIMAGESYNCADAPTOR_H
#define DROPBOXIMAGESYNCADAPTOR_H

#include "dropboxdatatypesyncadaptor.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QDateTime>
#include <QtCore/QVariantMap>
#include <QtCore/QList>
#include <QtCore/QJsonArray>
#include <QtSql/QSqlDatabase>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>

#include <socialcache/dropboximagesdatabase.h>
#include <socialcache/socialimagesdatabase.h>

class DropboxImageSyncAdaptor
        : public DropboxDataTypeSyncAdaptor
{
    Q_OBJECT

public:
    DropboxImageSyncAdaptor(QObject *parent);
    ~DropboxImageSyncAdaptor();

    QString syncServiceName() const;
    void sync(const QString &dataTypeString, int accountId);

protected: // implementing DropboxDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode);
    void beginSync(int accountId, const QString &accessToken);
    void finalize(int accountId);

private:
    void queryCameraRollCursor(int accountId, const QString &accessToken);
    void queryCameraRoll(int accountId, const QString &accessToken, const QString &albumId, const QString &cursor, const QString &continuationCursor);
    bool haveAlreadyCachedImage(const QString &fbImageId, const QString &imageUrl);
    void possiblyAddNewUser(const QString &fbUserId, int accountId, const QString &accessToken);

private Q_SLOTS:
    void cameraRollCursorFinishedHandler();
    void cameraRollFinishedHandler();
    void userFinishedHandler();

private:
    // for server-side removal detection.
    bool initRemovalDetectionLists(int accountId);
    void clearRemovalDetectionLists();
    void checkRemovedImages(const QString &fbAlbumId);
    QMap<QString, DropboxAlbum::ConstPtr> m_cachedAlbums;
    QMap<QString, QSet<QString> > m_serverImageIds;
    QStringList m_removedImages;

    DropboxImagesDatabase m_db;
    SocialImagesDatabase m_imageCacheDb;

    QJsonArray m_retrievedObjects;

    bool determineOptimalDimensions();
    int m_optimalThumbnailWidth;
    int m_optimalImageWidth;
};

#endif // DROPBOXIMAGESYNCADAPTOR_H
