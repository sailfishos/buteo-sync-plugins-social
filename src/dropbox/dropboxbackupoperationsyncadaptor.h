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

#ifndef DROPBOXBACKUPOPERATIONSYNCADAPTOR_H
#define DROPBOXBACKUPOPERATIONSYNCADAPTOR_H

#include "dropboxdatatypesyncadaptor.h"

#include <QtCore/QObject>
#include <QtCore/QList>
#include <QtCore/QStringList>
#include <QtCore/QSet>
#include <QtCore/QFileInfo>

class QDBusInterface;

class DropboxBackupOperationSyncAdaptor : public DropboxDataTypeSyncAdaptor
{
    Q_OBJECT

public:
    enum Operation {
        Backup,
        BackupQuery,
        BackupRestore
    };

    DropboxBackupOperationSyncAdaptor(SocialNetworkSyncAdaptor::DataType dataType, QObject *parent);
    ~DropboxBackupOperationSyncAdaptor();

    QString syncServiceName() const override;
    void sync(const QString &dataTypeString, int accountId) override;
    virtual DropboxBackupOperationSyncAdaptor::Operation operation() const = 0;

protected: // implementing DropboxDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode);
    void beginSync(int accountId, const QString &accessToken);
    void finalize(int accountId);
    void finalCleanup();

private:
    void requestList(int accountId,
                     const QString &accessToken,
                     const QString &remotePath,
                     const QString &continuationCursor,
                     const QVariantMap &extraProperties);
    void requestData(int accountId,
                     const QString &accessToken,
                     const QString &localPath,
                     const QString &remotePath,
                     const QString &remoteFile);
    void uploadData(int accountId,
                    const QString &accessToken,
                    const QString &localPath,
                    const QString &remotePath,
                    const QString &localFile);
    void purgeAccount(int accountId);

    void beginSyncOperation(int accountId, const QString &accessToken);

private Q_SLOTS:
    void cloudBackupStatusChanged(int accountId, const QString &status);
    void cloudBackupError(int accountId, const QString &error, const QString &errorString);
    void cloudRestoreStatusChanged(int accountId, const QString &status);
    void cloudRestoreError(int accountId, const QString &error, const QString &errorString);

    void remotePathFinishedHandler();
    void remoteFileFinishedHandler();
    void createRemotePathFinishedHandler();
    void createRemoteFileFinishedHandler();
    void downloadProgressHandler(qint64 bytesReceived, qint64 bytesTotal);
    void uploadProgressHandler(qint64 bytesSent, qint64 bytesTotal);

private:
    QDBusInterface *m_sailfishBackup = nullptr;
    QFileInfo m_localFileInfo;
    QSet<QString> m_backupFiles;
    QString m_remoteDirPath;
    QString m_accessToken;
    int m_accountId = 0;
};

#endif // DropboxBackupOperationSyncAdaptor_H
