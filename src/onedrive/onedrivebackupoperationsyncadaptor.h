/****************************************************************************
 **
 ** Copyright (c) 2020 Open Mobile Platform LLC.
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

#ifndef ONEDRIVEBACKUPOPERATIONSYNCADAPTOR_H
#define ONEDRIVEBACKUPOPERATIONSYNCADAPTOR_H

#include "onedrivedatatypesyncadaptor.h"

#include <QtCore/QObject>
#include <QtCore/QList>
#include <QtCore/QStringList>
#include <QtCore/QFileInfo>

class OneDriveBackupOperationSyncAdaptor : public OneDriveDataTypeSyncAdaptor
{
    Q_OBJECT

public:
    enum Operation {
        Backup,
        BackupQuery,
        BackupRestore
    };

    OneDriveBackupOperationSyncAdaptor(DataType dataType, QObject *parent);
    ~OneDriveBackupOperationSyncAdaptor();

    QString syncServiceName() const override;
    void sync(const QString &dataTypeString, int accountId) override;

    virtual OneDriveBackupOperationSyncAdaptor::Operation operation() const = 0;

protected: // implementing OneDriveDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode);
    void beginSync(int accountId, const QString &accessToken);
    void finalize(int accountId);
    void finalCleanup();

private:
    void initialiseAppFolderRequest(int accountId, const QString &accessToken,
                                    const QString &localPath, const QString &remotePath,
                                    const QString &remoteFile, const QString &syncDirection);
    void getRemoteFolderMetadata(int accountId, const QString &accessToken,
                                 const QString &localPath, const QString &remotePath,
                                 const QString &parentId, const QString &remoteDirName);

    void requestData(int accountId, const QString &accessToken,
                     const QString &localPath, const QString &remotePath,
                     const QString &remoteFile = QString(), const QString &redirectUrl = QString());
    void uploadData(int accountId, const QString &accessToken,
                    const QString &localPath, const QString &remotePath,
                    const QString &localFile = QString());
    void purgeAccount(int accountId);

private Q_SLOTS:
    void cloudBackupStatusChanged(int accountId, const QString &status);
    void cloudBackupError(int accountId, const QString &error, const QString &errorString);
    void cloudRestoreStatusChanged(int accountId, const QString &status);
    void cloudRestoreError(int accountId, const QString &error, const QString &errorString);

    void initialiseAppFolderFinishedHandler();
    void getRemoteFolderMetadataFinishedHandler();
    void remotePathFinishedHandler();
    void remoteFileFinishedHandler();
    void createRemotePathFinishedHandler();
    void createRemoteFileFinishedHandler();
    void downloadProgressHandler(qint64 bytesReceived, qint64 bytesTotal);
    void uploadProgressHandler(qint64 bytesSent, qint64 bytesTotal);

private:
    void beginListOperation(int accountId, const QString &accessToken, const QString &remoteDirPath);
    void beginSyncOperation(int accountId, const QString &accessToken);
    void listOperationFinished();

    QDBusInterface *m_sailfishBackup = nullptr;
    QString m_remoteAppDir;

    struct RemoteDirectory {
        QString dirName;
        QString remoteId;
        QString parentPath;
        QString parentId;
        bool created;
    };
    QList<RemoteDirectory> m_remoteDirectories;
    QString m_accessToken;
    QString m_remoteDirPath;
    QFileInfo m_localFileInfo;
    int m_accountId = 0;
};

#endif // ONEDRIVEBACKUPOPERATIONSYNCADAPTOR_H
