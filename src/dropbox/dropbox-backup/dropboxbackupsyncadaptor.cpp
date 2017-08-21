/****************************************************************************
 **
 ** Copyright (C) 2015 Jolla Ltd.
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

#include "dropboxbackupsyncadaptor.h"
#include "trace.h"

#include <QtCore/QPair>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QCryptographicHash>

#include <Accounts/Manager>
#include <Accounts/Account>

#include <MGConfItem>

#include <ssudeviceinfo.h>

static void debugDumpResponse(const QByteArray &data)
{
    QStringList lines = QString::fromUtf8(data).split('\n');
    Q_FOREACH (const QString &line, lines) {
        SOCIALD_LOG_DEBUG(line);
    }
}

DropboxBackupSyncAdaptor::DropboxBackupSyncAdaptor(QObject *parent)
    : DropboxDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Backup, parent)
{
    setInitialActive(true);
}

DropboxBackupSyncAdaptor::~DropboxBackupSyncAdaptor()
{
}

QString DropboxBackupSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("dropbox-backup");
}

void DropboxBackupSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    DropboxDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void DropboxBackupSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    purgeAccount(oldId);
}

void DropboxBackupSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    SsuDeviceInfo deviceInfo;
    QString deviceId = deviceInfo.deviceUid();
    QByteArray hashedDeviceId = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha256);
    QString encodedDeviceId = QString::fromUtf8(hashedDeviceId.toBase64(QByteArray::Base64UrlEncoding)).mid(0,12);
    if (deviceId.isEmpty()) {
        SOCIALD_LOG_ERROR("Could not determine device identifier; cannot create remote per-device backup directory!");
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    QString deviceDisplayNamePrefix = deviceInfo.displayName(Ssu::DeviceModel);
    if (!deviceDisplayNamePrefix.isEmpty()) {
        deviceDisplayNamePrefix = deviceDisplayNamePrefix.replace(' ', '-') + '_';
    }
    QString defaultRemotePath = QString::fromLatin1("Backups/%1%2").arg(deviceDisplayNamePrefix).arg(encodedDeviceId);
    QString defaultLocalPath = QString::fromLatin1("%1/Backups/")
                               .arg(QString::fromLatin1(PRIVILEGED_DATA_DIR));

    // read from dconf some key values, which determine the direction of sync etc.
    MGConfItem localPathConf("/SailfishOS/vault/Dropbox/localPath");
    MGConfItem remotePathConf("/SailfishOS/vault/Dropbox/remotePath");
    MGConfItem remoteFileConf("/SailfishOS/vault/Dropbox/remoteFile");
    MGConfItem directionConf("/SailfishOS/vault/Dropbox/direction");
    QString localPath = localPathConf.value(QString()).toString();
    QString remotePath = remotePathConf.value(QString()).toString();
    QString remoteFile = remoteFileConf.value(QString()).toString();
    QString direction = directionConf.value(QString()).toString();

    // Immediately unset the keys to ensure that future scheduled
    // or manually triggered syncs fail, until the keys are set.
    // Specifically, the value of the direction key is important.
    localPathConf.set(QString());
    remotePathConf.set(QString());
    remoteFileConf.set(QString());
    directionConf.set(QString());

    // set defaults if required.
    if (localPath.isEmpty()) {
        localPath = defaultLocalPath;
    }
    if (remotePath.isEmpty()) {
        remotePath = defaultRemotePath;
    }
    if (!remoteFile.isEmpty()) {
        // dropbox requestData() function takes remoteFile param which has a fully specified path.
        remoteFile = QStringLiteral("%1/%2").arg(remotePath).arg(remoteFile);
    }

    // create local directory if it doesn't exist
    QDir localDir;
    if (!localDir.mkpath(localPath)) {
        SOCIALD_LOG_ERROR("Could not create local backup directory:" << localPath << "for Dropbox account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // either upsync or downsync as required.
    if (direction == Buteo::VALUE_TO_REMOTE) {
        uploadData(accountId, accessToken, localPath, remotePath);
    } else if (direction == Buteo::VALUE_FROM_REMOTE) {
        // step one: get the remote path and its children metadata.
        // step two: for each (non-folder) child in metadata, download it.
        requestList(accountId, accessToken, localPath, remotePath, remoteFile, QString());
    } else {
        SOCIALD_LOG_ERROR("No direction set for Dropbox Backup sync with account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }
}

void DropboxBackupSyncAdaptor::requestList(int accountId,
                                           const QString &accessToken,
                                           const QString &localPath,
                                           const QString &remotePath,
                                           const QString &remoteFile,
                                           const QString &continuationCursor)
{
    QJsonObject requestParameters;
    if (continuationCursor.isEmpty()) {
        requestParameters.insert("path", remotePath);
        requestParameters.insert("recursive", false);
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

    SOCIALD_LOG_DEBUG("performing directory request:" << url.toString() << ":" << remotePath << continuationCursor);

    QNetworkReply *reply = m_networkAccessManager->post(req, postData);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("localPath", localPath);
        reply->setProperty("remotePath", remotePath);
        reply->setProperty("remoteFile", remoteFile);
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

void DropboxBackupSyncAdaptor::remotePathFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString remoteFile = reply->property("remoteFile").toString();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    if (isError) {
        SOCIALD_LOG_ERROR("error occurred when performing Backup remote path request for Dropbox account" << accountId);
        debugDumpResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(data, &ok);
    if (!ok || !parsed.contains("entries")) {
        SOCIALD_LOG_ERROR("no backup data exists in reply from Dropbox with account" << accountId);
        debugDumpResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    QJsonArray entries = parsed.value("entries").toArray();
    Q_FOREACH (const QJsonValue &child, entries) {
        const QString tag = child.toObject().value(".tag").toString();
        const QString childPath = child.toObject().value("path_display").toString();
        if (tag.compare("folder", Qt::CaseInsensitive) == 0) {
            SOCIALD_LOG_DEBUG("ignoring folder:" << childPath << "under remote backup path:" << remotePath << "for Dropbox account:" << accountId);
        } else if (tag.compare("file", Qt::CaseInsensitive) == 0){
            SOCIALD_LOG_DEBUG("found remote backup object:" << childPath << "for Dropbox account:" << accountId);
            m_backupFiles.insert(childPath);
        }
    }

    QString continuationCursor = parsed.value("cursor").toString();
    bool hasMore = parsed.value("has_more").toBool();
    if (hasMore) {
        requestList(accountId, accessToken, localPath, remotePath, remoteFile, continuationCursor);
    } else {
        for (QSet<QString>::const_iterator it = m_backupFiles.constBegin(); it != m_backupFiles.constEnd(); it++) {
            requestData(accountId, accessToken, localPath, remotePath, *it);
        }
    }

    decrementSemaphore(accountId);
}


void DropboxBackupSyncAdaptor::requestData(int accountId,
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

void DropboxBackupSyncAdaptor::remoteFileFinishedHandler()
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

void DropboxBackupSyncAdaptor::uploadData(int accountId, const QString &accessToken, const QString &localPath, const QString &remotePath, const QString &localFile)
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

void DropboxBackupSyncAdaptor::createRemotePathFinishedHandler()
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

void DropboxBackupSyncAdaptor::createRemoteFileFinishedHandler()
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

void DropboxBackupSyncAdaptor::downloadProgressHandler(qint64 bytesReceived, qint64 bytesTotal)
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

void DropboxBackupSyncAdaptor::uploadProgressHandler(qint64 bytesSent, qint64 bytesTotal)
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

void DropboxBackupSyncAdaptor::finalize(int accountId)
{
    SOCIALD_LOG_DEBUG("finished Dropbox backup sync for account" << accountId);
}

void DropboxBackupSyncAdaptor::purgeAccount(int)
{
    // TODO: delete the contents of the localPath directory?  probably not, could be shared between dropbox+onedrive
}

void DropboxBackupSyncAdaptor::finalCleanup()
{
    // nothing to do?
}

