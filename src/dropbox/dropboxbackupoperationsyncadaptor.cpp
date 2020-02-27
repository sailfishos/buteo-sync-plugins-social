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

#include "dropboxbackupoperationsyncadaptor.h"
#include "trace.h"

#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtCore/QVariantMap>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusInterface>

namespace {

void debugDumpResponse(const QByteArray &data)
{
    QStringList lines = QString::fromUtf8(data).split('\n');
    Q_FOREACH (const QString &line, lines) {
        SOCIALD_LOG_DEBUG(line);
    }
}

}


DropboxBackupOperationSyncAdaptor::DropboxBackupOperationSyncAdaptor(SocialNetworkSyncAdaptor::DataType dataType, QObject *parent)
    : DropboxDataTypeSyncAdaptor(dataType, parent)
    , m_sailfishBackup(new QDBusInterface("org.sailfishos.backup", "/sailfishbackup", "org.sailfishos.backup", QDBusConnection::sessionBus(), this))
{
    m_sailfishBackup->connection().connect(
                m_sailfishBackup->service(), m_sailfishBackup->path(), m_sailfishBackup->interface(),
                "cloudBackupStatusChanged", this, SLOT(cloudBackupStatusChanged(int,QString)));
    m_sailfishBackup->connection().connect(
                m_sailfishBackup->service(), m_sailfishBackup->path(), m_sailfishBackup->interface(),
                "cloudBackupError", this, SLOT(cloudBackupError(int,QString,QString)));
    m_sailfishBackup->connection().connect(
                m_sailfishBackup->service(), m_sailfishBackup->path(), m_sailfishBackup->interface(),
                "cloudRestoreStatusChanged", this, SLOT(cloudRestoreStatusChanged(int,QString)));
    m_sailfishBackup->connection().connect(
                m_sailfishBackup->service(), m_sailfishBackup->path(), m_sailfishBackup->interface(),
                "cloudRestoreError", this, SLOT(cloudRestoreError(int,QString,QString)));
}

DropboxBackupOperationSyncAdaptor::~DropboxBackupOperationSyncAdaptor()
{
}

QString DropboxBackupOperationSyncAdaptor::syncServiceName() const
{
    // this service covers all of these sync profiles: backup, backup query and restore.
    return QStringLiteral("dropbox-backup");
}

void DropboxBackupOperationSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    DropboxDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void DropboxBackupOperationSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    purgeAccount(oldId);
}

void DropboxBackupOperationSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    QDBusReply<QString> backupDeviceIdReply = m_sailfishBackup->call("backupFileDeviceId");
    if (backupDeviceIdReply.value().isEmpty()) {
        SOCIALD_LOG_ERROR("Backup device ID is invalid!");
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    m_remoteDirPath = QString::fromLatin1("/Backups/%1").arg(backupDeviceIdReply.value());
    m_accountId = accountId;
    m_accessToken = accessToken;

    switch (operation()) {
    case Backup:
    {
        QDBusReply<QString> createBackupReply =
                m_sailfishBackup->call("createBackupForSyncProfile", m_accountSyncProfile->name());
        if (!createBackupReply.isValid() || createBackupReply.value().isEmpty()) {
            SOCIALD_LOG_ERROR("Call to createBackupForSyncProfile() failed:" << createBackupReply.error().name()
                              << createBackupReply.error().message());
            setStatus(SocialNetworkSyncAdaptor::Error);
            return;
        }

        // Save the file path, then wait for org.sailfish.backup service to finish creating the
        // backup before continuing in cloudBackupStatusChanged().
        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        m_localFileInfo = QFileInfo(createBackupReply.value());
        break;
    }
    case BackupQuery:
    {
        requestList(accountId, accessToken, m_remoteDirPath, QString(), QVariantMap());
        break;
    }
    case BackupRestore:
    {
        const QString filePath = m_accountSyncProfile->key(QStringLiteral("sfos-backuprestore-file"));
        if (filePath.isEmpty()) {
            SOCIALD_LOG_ERROR("No remote file has been set!");
            setStatus(SocialNetworkSyncAdaptor::Error);
            return;
        }

        m_localFileInfo = QFileInfo(filePath);

        QDir localDir;
        if (!localDir.mkpath(m_localFileInfo.absolutePath())) {
            SOCIALD_LOG_ERROR("Could not create local backup directory:" << m_localFileInfo.absolutePath()
                              << "for Dropbox account:" << accountId);
            setStatus(SocialNetworkSyncAdaptor::Error);
            return;
        }

        beginSyncOperation(accountId, accessToken);
        break;
    }
    default:
        SOCIALD_LOG_ERROR("Unrecognized sync operation: " + operation());
        setStatus(SocialNetworkSyncAdaptor::Error);
        break;
    }
}

void DropboxBackupOperationSyncAdaptor::beginSyncOperation(int accountId, const QString &accessToken)
{
    // dropbox requestData() function takes remoteFile param which has a fully specified path.
    QString remoteFile = QStringLiteral("%1/%2").arg(m_remoteDirPath).arg(m_localFileInfo.fileName());

    // either upsync or downsync as required.
    if (operation() == Backup) {
        uploadData(accountId, accessToken, m_localFileInfo.absolutePath(), m_remoteDirPath, m_localFileInfo.fileName());
    } else if (operation() == BackupRestore) {
        // step one: get the remote path and its children metadata.
        // step two: for each (non-folder) child in metadata, download it.
        QVariantMap properties = {
            { QStringLiteral("localPath"), m_localFileInfo.absolutePath() },
            { QStringLiteral("remoteFile"), remoteFile },
        };
        requestList(accountId, accessToken, m_remoteDirPath, QString(), properties);
    } else {
        SOCIALD_LOG_ERROR("No direction set for Dropbox Backup sync with account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }
}

void DropboxBackupOperationSyncAdaptor::cloudBackupStatusChanged(int accountId, const QString &status)
{
    if (accountId != m_accountId) {
        return;
    }

    SOCIALD_LOG_DEBUG("Backup status changed:" << status << "for file:" << m_localFileInfo.absoluteFilePath());

    if (status == QLatin1String("UploadingBackup")) {

        if (!m_localFileInfo.exists()) {
            SOCIALD_LOG_ERROR("Backup finished, but cannot find the backup file:" << m_localFileInfo.absoluteFilePath());
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(m_accountId);
            return;
        }

        beginSyncOperation(m_accountId, m_accessToken);
        decrementSemaphore(m_accountId);

    } else if (status == QLatin1String("Canceled")) {
        SOCIALD_LOG_ERROR("Cloud backup was canceled");
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);

    } else if (status == QLatin1String("Error")) {
        SOCIALD_LOG_ERROR("Failed to create backup file:" << m_localFileInfo.absoluteFilePath());
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
    }
}

void DropboxBackupOperationSyncAdaptor::cloudBackupError(int accountId, const QString &error, const QString &errorString)
{
    if (accountId != m_accountId) {
        return;
    }

    SOCIALD_LOG_ERROR("Cloud backup error was:" << error << errorString);
    setStatus(SocialNetworkSyncAdaptor::Error);
    decrementSemaphore(m_accountId);
}

void DropboxBackupOperationSyncAdaptor::cloudRestoreStatusChanged(int accountId, const QString &status)
{
    if (accountId != m_accountId) {
        return;
    }

    SOCIALD_LOG_DEBUG("Backup restore status changed:" << status << "for file:" << m_localFileInfo.absoluteFilePath());

    if (status == QLatin1String("Canceled")) {
        SOCIALD_LOG_ERROR("Cloud backup restore was canceled");
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);

    } else if (status == QLatin1String("Error")) {
        SOCIALD_LOG_ERROR("Cloud backup restore failed");
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(m_accountId);
    }
}

void DropboxBackupOperationSyncAdaptor::cloudRestoreError(int accountId, const QString &error, const QString &errorString)
{
    if (accountId != m_accountId) {
        return;
    }

    SOCIALD_LOG_ERROR("Cloud backup restore error was:" << error << errorString);
}

void DropboxBackupOperationSyncAdaptor::requestList(int accountId,
                                           const QString &accessToken,
                                           const QString &remotePath,
                                           const QString &continuationCursor,
                                           const QVariantMap &extraProperties)
{
    QJsonObject requestParameters;
    if (continuationCursor.isEmpty()) {
        requestParameters.insert("path", remotePath);
        requestParameters.insert("recursive", false);
        requestParameters.insert("include_deleted", false);
        requestParameters.insert("include_has_explicit_shared_members", false);
    } else {
        if (!continuationCursor.isEmpty()) {
            requestParameters.insert("cursor", continuationCursor);
        }
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

    SOCIALD_LOG_DEBUG("performing directory request:" << url.toString() << ":" << remotePath << continuationCursor);

    QNetworkReply *reply = m_networkAccessManager->post(req, postData);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("remotePath", remotePath);
        for (QVariantMap::const_iterator it = extraProperties.constBegin();
             it != extraProperties.constEnd(); ++it) {
            reply->setProperty(it.key().toUtf8().constData(), it.value());
        }
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(remotePathFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply, 10 * 60 * 1000); // 10 minutes
    } else {
        SOCIALD_LOG_ERROR("unable to request data from Dropbox account with id" << accountId);
    }
}

void DropboxBackupOperationSyncAdaptor::remotePathFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString remotePath = reply->property("remotePath").toString();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    if (isError) {
        // Show error but don't set error status until error code is checked more thoroughly.
        SOCIALD_LOG_ERROR("error occurred when performing Backup remote path request for Dropbox account" << accountId);
        debugDumpResponse(data);
    }

    bool ok = false;
    const QJsonObject parsed = parseJsonObjectReplyData(data, &ok);
    const QJsonArray entries = parsed.value("entries").toArray();

    if (!ok || entries.isEmpty()) {
        QString errorMessage = parsed.value("error_summary").toString();
        if (!errorMessage.isEmpty()) {
            SOCIALD_LOG_ERROR("Dropbox returned error message:" << errorMessage);
            errorMessage.clear();
        }

        // Directory may be not found or be empty if user has deleted backups. Only set the error
        // status if parsing failed or if there was an unexpected error code.
        if (!ok) {
            errorMessage = QStringLiteral("Failed to parse directory listing at %1 for account %2").arg(remotePath).arg(accountId);
        } else if (httpCode != 200
                   && httpCode != 404
                   && httpCode != 409   // Dropbox error when requested path is not found
                   && httpCode != 410) {
            errorMessage = QStringLiteral("Directory listing request at %1 for account %2 failed").arg(remotePath).arg(accountId);
        }

        if (errorMessage.isEmpty()) {
            SOCIALD_LOG_DEBUG("Completed directory listing for account:" << accountId);
        } else {
            SOCIALD_LOG_ERROR(errorMessage);
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
            return;
        }
    }

    QString continuationCursor = parsed.value("cursor").toString();
    bool hasMore = parsed.value("has_more").toBool();

    for (const QJsonValue &child : entries) {
        const QString tag = child.toObject().value(".tag").toString();
        const QString childPath = child.toObject().value("path_display").toString();
        if (tag.compare("folder", Qt::CaseInsensitive) == 0) {
            SOCIALD_LOG_DEBUG("ignoring folder:" << childPath << "under remote backup path:" << remotePath << "for Dropbox account:" << accountId);
        } else if (tag.compare("file", Qt::CaseInsensitive) == 0){
            SOCIALD_LOG_DEBUG("found remote backup object:" << childPath << "for Dropbox account:" << accountId);
            m_backupFiles.insert(childPath);
        }
    }

    if (entries.isEmpty()) {
        SOCIALD_LOG_DEBUG("No entries found in dir listing, but not an error (e.g. maybe file was deleted on server)");
        debugDumpResponse(data);
    } else {
        SOCIALD_LOG_DEBUG("Parsed dir listing entries:" << entries);
    }

    switch (operation()) {
    case BackupQuery:
    {
        if (hasMore) {
            requestList(accountId, accessToken, remotePath, continuationCursor, QVariantMap());
        } else {
            QDBusReply<void> setCloudBackupsReply =
                    m_sailfishBackup->call("setCloudBackups", m_accountSyncProfile->name(),
                                                QVariant(m_backupFiles.toList()));
            if (!setCloudBackupsReply.isValid()) {
                SOCIALD_LOG_DEBUG("Call to setCloudBackups() failed:" << setCloudBackupsReply.error().name()
                                  << setCloudBackupsReply.error().message());
            } else {
                SOCIALD_LOG_DEBUG("Wrote directory listing for" << m_accountSyncProfile->name());
            }
        }
        break;
    }
    case Backup:
    case BackupRestore:
    {
        QString localPath = reply->property("localPath").toString();
        QString remoteFile = reply->property("remoteFile").toString();
        if (hasMore) {
            QVariantMap properties = {
                { QStringLiteral("localPath"), localPath },
                { QStringLiteral("remoteFile"), remoteFile },
            };
            requestList(accountId, accessToken, remotePath, continuationCursor, properties);
        } else {
            bool fileFound = false;
            for (QSet<QString>::const_iterator it = m_backupFiles.constBegin(); it != m_backupFiles.constEnd(); it++) {
                if ((*it).endsWith(remoteFile)) {
                    requestData(accountId, accessToken, localPath, remotePath, *it);
                    fileFound = true;
                    break;
                }
            }
            if (!fileFound) {
                SOCIALD_LOG_ERROR("Cannot find requested file on remote server:" << remoteFile);
                setStatus(SocialNetworkSyncAdaptor::Error);
                decrementSemaphore(accountId);
                return;
            }
        }
        break;
    }
    default:
        SOCIALD_LOG_ERROR("Unrecognized sync operation: " << operation());
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    decrementSemaphore(accountId);
}

void DropboxBackupOperationSyncAdaptor::requestData(int accountId,
                                           const QString &accessToken,
                                           const QString &localPath,
                                           const QString &remotePath,
                                           const QString &remoteFile)
{
    // file download request
    QJsonObject fileQueryObject;
    fileQueryObject.insert("path", remoteFile);
    QByteArray fileQueryArg = QJsonDocument(fileQueryObject).toJson(QJsonDocument::Compact);

    QUrl url(QStringLiteral("%1/2/files/download?arg=%2").arg(content(), QString::fromUtf8(fileQueryArg.toPercentEncoding())));
    QNetworkRequest req;
    req.setUrl(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    SOCIALD_LOG_DEBUG("performing file download request:" << url.toString() << ":" << remoteFile);

    QNetworkReply *reply = m_networkAccessManager->post(req, QByteArray());
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("localPath", localPath);
        reply->setProperty("remotePath", remotePath);
        reply->setProperty("remoteFile", remoteFile);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(downloadProgressHandler(qint64,qint64)));
        connect(reply, SIGNAL(finished()), this, SLOT(remoteFileFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply, 10 * 60 * 1000); // 10 minutes
    } else {
        SOCIALD_LOG_ERROR("unable to create download request:" << remotePath << remoteFile <<
                          "for Dropbox account with id" << accountId);
    }
}

void DropboxBackupOperationSyncAdaptor::remoteFileFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString remoteFile = reply->property("remoteFile").toString();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();

    removeReplyTimeout(accountId, reply);
    if (isError) {
        SOCIALD_LOG_ERROR("error occurred when performing Backup remote file request for Dropbox account" << accountId);
        debugDumpResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    if (data.isEmpty()) {
        SOCIALD_LOG_INFO("remote file:" << remoteFile << "from" << remotePath << "is empty; ignoring");
    } else {
        // create local directory if it doesn't exist
        QFileInfo fileInfo(QStringLiteral("%1/%2").arg(localPath).arg(QFileInfo(remoteFile).fileName()));
        QDir localDir;
        if (!localDir.mkpath(fileInfo.absolutePath())) {
            SOCIALD_LOG_ERROR("Could not create local backup directory:" << fileInfo.absolutePath()
                              << "for Dropbox account:" << accountId);
            setStatus(SocialNetworkSyncAdaptor::Error);
            return;
        }

        QFile file(fileInfo.absoluteFilePath());
        if (!file.open(QIODevice::WriteOnly)) {
            SOCIALD_LOG_ERROR("could not open" << file.fileName() << "locally for writing!");
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
        } else if (!file.write(data)) {
            SOCIALD_LOG_ERROR("could not write data to" << file.fileName() << "locally from" <<
                              remotePath << remoteFile << "for Dropbox account:" << accountId);
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
        } else {
            SOCIALD_LOG_DEBUG("successfully wrote" << data.size() << "bytes to:" << file.fileName() << "from:" << remoteFile);
        }
        file.close();
    }

    decrementSemaphore(accountId);
}

void DropboxBackupOperationSyncAdaptor::uploadData(int accountId, const QString &accessToken, const QString &localPath, const QString &remotePath, const QString &localFile)
{
    // step one: ensure the remote path exists (and if not, create it)
    // step two: upload every single file from the local path to the remote path.

    QNetworkReply *reply = 0;
    if (localFile.isEmpty()) {
        // attempt to create the remote path directory.
        QJsonObject requestParameters;
        requestParameters.insert("path", remotePath.startsWith(QLatin1String("/")) ? remotePath : QStringLiteral("/%1").arg(remotePath));
        requestParameters.insert("autorename", false);
        QJsonDocument doc;
        doc.setObject(requestParameters);
        QByteArray postData = doc.toJson(QJsonDocument::Compact);

        QUrl url = QUrl(QStringLiteral("%1/2/files/create_folder_v2").arg(api()));
        QNetworkRequest req;
        req.setUrl(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setHeader(QNetworkRequest::ContentLengthHeader, postData.size());
        req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                         QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

        SOCIALD_LOG_DEBUG("Attempting to create the remote directory:" << remotePath << "via request:" << url.toString());

        reply = m_networkAccessManager->post(req, postData);
    } else {
        // attempt to create a remote file.
        const QString filePath = remotePath.startsWith(QLatin1String("/"))
                ? QStringLiteral("%1/%2").arg(remotePath, localFile)
                : QStringLiteral("/%1/%2").arg(remotePath, localFile);
        QJsonObject requestParameters;
        requestParameters.insert("path", filePath);
        requestParameters.insert("mode", "overwrite");
        QJsonDocument doc;
        doc.setObject(requestParameters);
        QByteArray requestParamData = doc.toJson(QJsonDocument::Compact);

        QUrl url = QUrl(QStringLiteral("%1/2/files/upload").arg(content()));
        QNetworkRequest req;
        req.setUrl(url);

        QString localFileName = QStringLiteral("%1/%2").arg(localPath).arg(localFile);
        QFile f(localFileName, this);
         if(!f.open(QIODevice::ReadOnly)){
             SOCIALD_LOG_ERROR("unable to open local file:" << localFileName << "for upload to Dropbox Backup with account:" << accountId);
         } else {
             QByteArray data(f.readAll());
             f.close();
             QNetworkRequest req(url);
             req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                              QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());
             req.setRawHeader(QString(QLatin1String("Dropbox-API-Arg")).toUtf8(),
                              requestParamData);
             req.setHeader(QNetworkRequest::ContentLengthHeader, data.size());
             req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
             SOCIALD_LOG_DEBUG("Attempting to create the remote file:" << QStringLiteral("%1/%2").arg(remotePath).arg(localFile) << "via request:" << url.toString());
             reply = m_networkAccessManager->post(req, data);
        }
    }

    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("localPath", localPath);
        reply->setProperty("remotePath", remotePath);
        reply->setProperty("localFile", localFile);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        if (localFile.isEmpty()) {
            connect(reply, SIGNAL(finished()), this, SLOT(createRemotePathFinishedHandler()));
        } else {
            connect(reply, SIGNAL(uploadProgress(qint64,qint64)), this, SLOT(uploadProgressHandler(qint64,qint64)));
            connect(reply, SIGNAL(finished()), this, SLOT(createRemoteFileFinishedHandler()));
        }

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply, 10 * 60 * 1000); // 10 minutes
    } else {
        SOCIALD_LOG_ERROR("unable to create upload request:" << localPath << localFile << "->" << remotePath <<
                          "for Dropbox account with id" << accountId);
    }
}

void DropboxBackupOperationSyncAdaptor::createRemotePathFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    bool isError = reply->property("isError").toBool();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    if (isError) {
        // we actually expect a conflict error if the folder already existed, which is fine.
        if (httpCode != 409) {
            // this must be a real error.
            SOCIALD_LOG_ERROR("remote path creation failed:" << httpCode << QString::fromUtf8(data));
            debugDumpResponse(data);
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
            return;
        } else {
            SOCIALD_LOG_DEBUG("remote path creation had conflict: already exists:" << remotePath << ".  Continuing.");
        }
    }

    // upload all files from the local path to the remote server.
    SOCIALD_LOG_DEBUG("now uploading files from" << localPath << "to Dropbox folder:" << remotePath);
    QDir dir(localPath);
    QStringList localFiles = dir.entryList(QDir::Files);
    Q_FOREACH (const QString &localFile, localFiles) {
        SOCIALD_LOG_DEBUG("about to upload:" << localFile << "to Dropbox folder:" << remotePath);
        uploadData(accountId, accessToken, localPath, remotePath, localFile);
    }

    decrementSemaphore(accountId);
}

void DropboxBackupOperationSyncAdaptor::createRemoteFileFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString localFile = reply->property("localFile").toString();

    bool ok = true;
    QJsonObject parsed = parseJsonObjectReplyData(data, &ok);
    bool isError = reply->property("isError").toBool() || !ok || !parsed.value("error_summary").toString().isEmpty();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    if (isError) {
        SOCIALD_LOG_ERROR("failed to backup file:" << localPath << localFile << "to:" << remotePath <<
                          "for Dropbox account:" << accountId << ", code:" << httpCode <<
                          ":" << parsed.value("error_summary").toString());
        debugDumpResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    SOCIALD_LOG_DEBUG("successfully uploaded backup of file:" << localPath << localFile << "to:" << remotePath <<
                      "for Dropbox account:" << accountId);
    decrementSemaphore(accountId);
}

void DropboxBackupOperationSyncAdaptor::downloadProgressHandler(qint64 bytesReceived, qint64 bytesTotal)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    int accountId = reply->property("accountId").toInt();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString localFile = reply->property("localFile").toString();
    SOCIALD_LOG_DEBUG("Have download progress: bytesReceived:" << bytesReceived <<
                      "of" << bytesTotal << ", for" << localPath << localFile <<
                      "from" << remotePath << "with account:" << accountId);
}

void DropboxBackupOperationSyncAdaptor::uploadProgressHandler(qint64 bytesSent, qint64 bytesTotal)
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    int accountId = reply->property("accountId").toInt();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString localFile = reply->property("localFile").toString();
    SOCIALD_LOG_DEBUG("Have upload progress: bytesSent:" << bytesSent <<
                      "of" << bytesTotal << ", for" << localPath << localFile <<
                      "to" << remotePath << "with account:" << accountId);
}

void DropboxBackupOperationSyncAdaptor::finalize(int accountId)
{
    SOCIALD_LOG_DEBUG("Finalize Dropbox backup sync for account" << accountId);

    if (operation() == Backup) {
        SOCIALD_LOG_DEBUG("Deleting created backup file" << m_localFileInfo.absoluteFilePath());
        QFile::remove(m_localFileInfo.absoluteFilePath());
        QDir().rmdir(m_localFileInfo.absolutePath());
    }
}

void DropboxBackupOperationSyncAdaptor::purgeAccount(int)
{
    // TODO: delete the contents of the localPath directory?  probably not, could be shared between dropbox+onedrive
}

void DropboxBackupOperationSyncAdaptor::finalCleanup()
{
    // nothing to do?
}

