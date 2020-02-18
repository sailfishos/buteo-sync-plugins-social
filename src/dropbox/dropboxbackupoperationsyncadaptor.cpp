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

#include <Accounts/Manager>
#include <Accounts/Account>

// buteo
#include <ProfileManager.h>
#include <SyncProfile.h>

static void debugDumpResponse(const QByteArray &data)
{
    QStringList lines = QString::fromUtf8(data).split('\n');
    Q_FOREACH (const QString &line, lines) {
        SOCIALD_LOG_DEBUG(line);
    }
}

DropboxBackupOperationSyncAdaptor::DropboxBackupOperationSyncAdaptor(SocialNetworkSyncAdaptor::DataType dataType, const QString &profileName, QObject *parent)
    : DropboxDataTypeSyncAdaptor(dataType, parent)
    , m_profileManager(new Buteo::ProfileManager)
    , m_profileName(profileName)
{
}

DropboxBackupOperationSyncAdaptor::~DropboxBackupOperationSyncAdaptor()
{
    delete m_profileManager;
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
    bool backupRestoreOptionsLoaded = false;
    Buteo::SyncProfile *syncProfile = m_profileManager->syncProfile(m_profileName);
    BackupRestoreOptions backupRestoreOptions =
            BackupRestoreOptions::fromProfile(syncProfile, &backupRestoreOptionsLoaded);
    if (!backupRestoreOptionsLoaded) {
        SOCIALD_LOG_ERROR("Could not load backup/restore options for" << m_profileName);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // Immediately unset the backup/restore to ensure that future scheduled
    // or manually triggered syncs fail, until the options are set again.
    BackupRestoreOptions emptyOptions;
    if (!emptyOptions.copyToProfile(syncProfile)
            || m_profileManager->updateProfile(*syncProfile).isEmpty()) {
        SOCIALD_LOG_ERROR("Warning: failed to reset backup/restore options for profile: " + m_profileName);
    }

    if (backupRestoreOptions.localDirPath.isEmpty()) {
        backupRestoreOptions.localDirPath = QString::fromLatin1("%1/Backups/").arg(PRIVILEGED_DATA_DIR);
    }
    // create local directory if it doesn't exist
    QDir localDir;
    if (!localDir.mkpath(backupRestoreOptions.localDirPath)) {
        SOCIALD_LOG_ERROR("Could not create local backup directory:"
                          << backupRestoreOptions.localDirPath
                          << "for Dropbox account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    if (backupRestoreOptions.remoteDirPath.isEmpty()) {
        QString backupDeviceName = BackupRestoreOptions::backupDeviceName();
        if (backupDeviceName.isEmpty()) {
            SOCIALD_LOG_ERROR("backupDeviceName() returned empty string!");
            setStatus(SocialNetworkSyncAdaptor::Error);
            return;
        }
        backupRestoreOptions.remoteDirPath = QString::fromLatin1("/Backups/%1").arg(backupDeviceName);
    }

    switch (operation()) {
    case BackupQuery:
        beginListOperation(accountId, accessToken, backupRestoreOptions);
        break;
    case Backup:
    case BackupRestore:
        beginSyncOperation(accountId, accessToken, backupRestoreOptions);
        break;
    default:
        SOCIALD_LOG_ERROR("Unrecognized sync operation: " + operation());
        setStatus(SocialNetworkSyncAdaptor::Error);
        break;
    }
}

void DropboxBackupOperationSyncAdaptor::beginListOperation(int accountId, const QString &accessToken, const BackupRestoreOptions &options)
{
    if (options.localDirPath.isEmpty() || options.fileName.isEmpty()) {
        SOCIALD_LOG_ERROR("Cannot fetch directory listing, no local results file path set!");
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    QVariantMap properties = {
        { QStringLiteral("listResultPath"), options.localDirPath + '/' + options.fileName },
    };
    requestList(accountId, accessToken, options.remoteDirPath, QString(), properties);
}

void DropboxBackupOperationSyncAdaptor::beginSyncOperation(int accountId, const QString &accessToken, const BackupRestoreOptions &options)
{
    QString remoteFile = options.fileName;
    if (!remoteFile.isEmpty()) {
        // dropbox requestData() function takes remoteFile param which has a fully specified path.
        remoteFile = QStringLiteral("%1/%2").arg(options.remoteDirPath).arg(remoteFile);
    }

    // either upsync or downsync as required.
    if (operation() == Backup) {
        uploadData(accountId, accessToken, options.localDirPath, options.remoteDirPath);
    } else if (operation() == BackupRestore) {
        // step one: get the remote path and its children metadata.
        // step two: for each (non-folder) child in metadata, download it.
        QVariantMap properties = {
            { QStringLiteral("localPath"), options.localDirPath },
            { QStringLiteral("remoteFile"), remoteFile },
        };
        requestList(accountId, accessToken, options.remoteDirPath, QString(), properties);
    } else {
        SOCIALD_LOG_ERROR("No direction set for Dropbox Backup sync with account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }
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
        QString listResultPath = reply->property("listResultPath").toString();
        if (listResultPath.isEmpty()) {
            SOCIALD_LOG_ERROR("Cannot save directory listing, no local results file path set");
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
            return;
        }

        if (hasMore) {
            QVariantMap properties = {
                { QStringLiteral("listResultPath"), listResultPath },
            };
            requestList(accountId, accessToken, remotePath, continuationCursor, properties);
        } else {
            QFile file(listResultPath);
            if (!file.open(QFile::WriteOnly | QFile::Text)) {
                SOCIALD_LOG_ERROR("Cannot open" << file.fileName() << "to write directory listing results");
                setStatus(SocialNetworkSyncAdaptor::Error);
                decrementSemaphore(accountId);
                return;
            }
            QByteArray dirListingBytes = m_backupFiles.toList().join('\n').toUtf8();
            if (file.write(dirListingBytes) < 0) {
                SOCIALD_LOG_ERROR("Cannot write directory listing results to" << file.fileName());
                setStatus(SocialNetworkSyncAdaptor::Error);
                decrementSemaphore(accountId);
                return;
            }
            file.close();
            SOCIALD_LOG_DEBUG("Wrote directory listing to" << file.fileName());
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
            for (QSet<QString>::const_iterator it = m_backupFiles.constBegin(); it != m_backupFiles.constEnd(); it++) {
                requestData(accountId, accessToken, localPath, remotePath, *it);
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
        const QString filename = QStringLiteral("%1/%2").arg(localPath).arg(remoteFile.split('/').last());
        QFile file(filename);
        if (!file.open(QIODevice::WriteOnly)) {
            SOCIALD_LOG_ERROR("could not open" << filename << "locally for writing!");
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
        } else if (!file.write(data)) {
            SOCIALD_LOG_ERROR("could not write data to" << filename << "locally from" <<
                              remotePath << remoteFile << "for Dropbox account:" << accountId);
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
        } else {
            SOCIALD_LOG_DEBUG("successfully wrote" << data.size() << "bytes to:" << filename << "from:" << remoteFile);
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
    SOCIALD_LOG_DEBUG("finished Dropbox backup sync for account" << accountId);
}

void DropboxBackupOperationSyncAdaptor::purgeAccount(int)
{
    // TODO: delete the contents of the localPath directory?  probably not, could be shared between dropbox+onedrive
}

void DropboxBackupOperationSyncAdaptor::finalCleanup()
{
    // nothing to do?
}

