/****************************************************************************
 **
 ** Copyright (C) 2015-2019 Jolla Ltd.
 ** Copyright (C) 2019 Open Mobile Platform LLC
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

#include "onedrivebackupsyncadaptor.h"
#include "trace.h"

#include <QtCore/QPair>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QCryptographicHash>

#include <Accounts/Manager>
#include <Accounts/Account>

// buteo
#include <ProfileManager.h>
#include <SyncProfile.h>

static void debugDumpResponse(const QByteArray &data)
{
    QString alldata = QString::fromUtf8(data);
    QStringList alldatasplit = alldata.split('\n');
    Q_FOREACH (const QString &s, alldatasplit) {
        SOCIALD_LOG_DEBUG(s);
    }
}

static void debugDumpJsonResponse(const QByteArray &data)
{
    // 8 is the minimum log level for TRACE logs
    // as defined in Buteo's LogMacros.h
    if (Buteo::Logger::instance()->getLogLevel() < 8) {
        return;
    }

    // Prettify the json for outputting line-by-line.
    QString output;
    QString json = QString::fromUtf8(data);
    QString leadingSpace = "";
    for (int i = 0; i < json.size(); ++i) {
        if (json[i] == '{') {
            leadingSpace = leadingSpace + "    ";
            output = output + json[i] + '\n' + leadingSpace;
        } else if (json[i] == '}') {
            if (leadingSpace.size() >= 4) {
                leadingSpace.chop(4);
            }
            output = output + '\n' + leadingSpace + json[i];
        } else if (json[i] == ',') {
            output = output + json[i] + '\n' + leadingSpace;
        } else if (json[i] == '\n' || json[i] == '\r') {
            // ignore newlines/carriage returns
        } else {
            output = output + json[i];
        }
    }
    debugDumpResponse(output.toUtf8());
}

OneDriveBackupSyncAdaptor::OneDriveBackupSyncAdaptor(const QString &profileName, QObject *parent)
    : OneDriveDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Backup, parent)
    , m_profileManager(new Buteo::ProfileManager)
    , m_profileName(profileName)
    , m_remoteAppDir(QStringLiteral("drive/special/approot"))
{
    setInitialActive(true);
}

OneDriveBackupSyncAdaptor::~OneDriveBackupSyncAdaptor()
{
    delete m_profileManager;
}

QString OneDriveBackupSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("onedrive-backup");
}

void OneDriveBackupSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    OneDriveDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void OneDriveBackupSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    purgeAccount(oldId);
}

void OneDriveBackupSyncAdaptor::beginSync(int accountId, const QString &accessToken)
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
                          << "for OneDrive account:" << accountId);
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
        backupRestoreOptions.remoteDirPath = "Backups/" + backupDeviceName;
    }

    switch (backupRestoreOptions.operation) {
    case BackupRestoreOptions::DirectoryListing:
        beginListOperation(accountId, accessToken, backupRestoreOptions);
        break;
    case BackupRestoreOptions::Upload:
    case BackupRestoreOptions::Download:
        beginSyncOperation(accountId, accessToken, backupRestoreOptions);
        break;
    default:
        SOCIALD_LOG_ERROR("Unrecognized sync operation: " + backupRestoreOptions.operation);
        setStatus(SocialNetworkSyncAdaptor::Error);
        break;
    }
}

void OneDriveBackupSyncAdaptor::beginListOperation(int accountId, const QString &accessToken, const BackupRestoreOptions &options)
{
    if (options.localDirPath.isEmpty() || options.fileName.isEmpty()) {
        SOCIALD_LOG_ERROR("Cannot fetch directory listing, no local results file path set");
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    QUrl url(QStringLiteral("https://api.onedrive.com/v1.0/drive/special/approot:/%1:/").arg(options.remoteDirPath));
    QUrlQuery query(url);
    QList<QPair<QString, QString> > queryItems;
    queryItems.append(QPair<QString, QString>(QStringLiteral("expand"), QStringLiteral("children")));
    query.setQueryItems(queryItems);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());
    QNetworkReply *reply = m_networkAccessManager->get(req);
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("remotePath", options.remoteDirPath);
        reply->setProperty("listResultPath", options.localDirPath + '/' + options.fileName);
        connect(reply, &QNetworkReply::finished, this, &OneDriveBackupSyncAdaptor::listOperationFinished);

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply, 10 * 60 * 1000); // 10 minutes
    } else {
        SOCIALD_LOG_ERROR("unable to start directory listing request for OneDrive account with id" << accountId);
    }
}

void OneDriveBackupSyncAdaptor::listOperationFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString listResultPath = reply->property("listResultPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    if (isError) {
        // Show error but don't set error status until error code is checked more thoroughly.
        SOCIALD_LOG_ERROR("error occurred when performing Backup remote path request for OneDrive account" << accountId);
        debugDumpResponse(data);
    }

    bool ok = false;
    const QJsonObject parsed = parseJsonObjectReplyData(data, &ok);
    const QJsonArray entries = parsed.value("children").toArray();

    if (!ok || entries.isEmpty()) {
        QString errorMessage = parsed.value("error").toString();
        if (!errorMessage.isEmpty()) {
            SOCIALD_LOG_ERROR("OneDrive returned error message:" << errorMessage);
            errorMessage.clear();
        }

        // Directory may be not found or be empty if user has deleted backups. Only emit the error
        // signal if parsing failed or there was an unexpected error code.
        if (!ok) {
            errorMessage = QStringLiteral("Failed to parse directory listing at %1 for account %2").arg(remotePath).arg(accountId);
        } else if (httpCode != 200
                   && httpCode != 404
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

    if (entries.isEmpty()) {
        SOCIALD_LOG_DEBUG("No entries found in dir listing, but not an error (e.g. maybe file was deleted on server)");
        debugDumpResponse(data);
    } else {
        SOCIALD_LOG_DEBUG("Parsed dir listing entries:" << entries);
    }

    QStringList dirListing;
    for (const QJsonValue &child : entries) {
        const QString childName = child.toObject().value("name").toString();
        if (child.toObject().keys().contains("folder")) {
            SOCIALD_LOG_DEBUG("ignoring folder:" << childName << "under remote backup path:" << remotePath
                              << "for account:" << accountId);
        } else {
            SOCIALD_LOG_DEBUG("found remote backup object:" << childName
                              << "for account:" << accountId
                              << "under remote backup path:" << remotePath);
            dirListing.append(remotePath + '/' + childName);
        }
    }

    QFile file(listResultPath);
    if (!file.open(QFile::WriteOnly | QFile::Text)) {
        SOCIALD_LOG_ERROR("Cannot open" << file.fileName() << "to write directory listing results");
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    const QByteArray dirListingBytes = dirListing.join('\n').toUtf8();
    if (file.write(dirListingBytes) < 0) {
        SOCIALD_LOG_ERROR("Cannot write directory listing results to" << file.fileName());
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }
    file.close();
    SOCIALD_LOG_DEBUG("Wrote directory listing to" << file.fileName());

    decrementSemaphore(accountId);
}


void OneDriveBackupSyncAdaptor::beginSyncOperation(int accountId, const QString &accessToken, const BackupRestoreOptions &options)
{
    QString direction = options.operation == BackupRestoreOptions::Upload
            ? Buteo::VALUE_TO_REMOTE
            : (options.operation == BackupRestoreOptions::Download ? Buteo::VALUE_FROM_REMOTE : QString());
    if (direction.isEmpty()) {
        SOCIALD_LOG_ERROR("Invalid sync operation" << options.operation << "for OneDrive account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }

    // either upsync or downsync as required.
    if (direction == Buteo::VALUE_TO_REMOTE || direction == Buteo::VALUE_FROM_REMOTE) {
        // Perform an initial app folder request before upload/download.
        initialiseAppFolderRequest(accountId, accessToken, options.localDirPath, options.remoteDirPath, options.fileName, direction);
    } else {
        SOCIALD_LOG_ERROR("No direction set for OneDrive Backup sync with account:" << accountId);
        setStatus(SocialNetworkSyncAdaptor::Error);
        return;
    }
}

void OneDriveBackupSyncAdaptor::initialiseAppFolderRequest(int accountId, const QString &accessToken, const QString &localPath, const QString &remotePath, const QString &remoteFile, const QString &syncDirection)
{
    // initialise the app folder and get the remote id of the drive/special/approot path.
    // e.g., let's say we have a final path like: drive/special/approot/Backups/ABCDEFG/backup.tar
    // this request will get us the id of the drive/special/approot bit.
    QUrl url = QUrl(QStringLiteral("https://api.onedrive.com/v1.0/drive/special/approot"));

    QNetworkRequest req(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(req);

    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("localPath", localPath);
        reply->setProperty("remotePath", remotePath);
        reply->setProperty("remoteFile", remoteFile);
        reply->setProperty("syncDirection", syncDirection);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(initialiseAppFolderFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply, 10 * 60 * 1000); // 10 minutes
    } else {
        SOCIALD_LOG_ERROR("unable to create app folder initialisation request for OneDrive account with id" << accountId);
    }
}

void OneDriveBackupSyncAdaptor::initialiseAppFolderFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString remoteFile = reply->property("remoteFile").toString();
    QString syncDirection = reply->property("syncDirection").toString();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(data, &ok);

    if (isError || !ok) {
        SOCIALD_LOG_ERROR("error occurred when performing initialiseAppFolder request with OneDrive account:" << accountId);
        debugDumpJsonResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
    } else {
        SOCIALD_LOG_DEBUG("initialiseAppFolder request succeeded with OneDrive account:" << accountId);
        SOCIALD_LOG_DEBUG("app folder has remote ID:" << parsed.value("id").toString());

        // Initialize our list of remote directories to create
        if (syncDirection == Buteo::VALUE_TO_REMOTE) {
            QString remoteParentPath = m_remoteAppDir;
            Q_FOREACH (const QString &dir, remotePath.split('/', QString::SkipEmptyParts)) {
                OneDriveBackupSyncAdaptor::RemoteDirectory remoteDir;
                remoteDir.dirName = dir;
                remoteDir.remoteId = QString();
                remoteDir.parentPath = remoteParentPath;
                remoteDir.parentId = QString();
                remoteDir.created = false;
                m_remoteDirectories.append(remoteDir);
                remoteParentPath = QStringLiteral("%1/%2").arg(remoteParentPath).arg(dir);
            }

            // Read out the app folder remote ID from the response and set it as the parent ID of the first remote dir.
            m_remoteDirectories[0].parentId = parsed.value("id").toString();
            SOCIALD_LOG_DEBUG("Set the parentId of the first subfolder:" << m_remoteDirectories[0].dirName << "to:" << m_remoteDirectories[0].parentId);

            // and begin creating the remote directory structure as required, prior to uploading the files.
            // We will create the first (intermediate) remote directory
            // e.g. if final path is: drive/special/approot/Backups/ABCDEFG/backup.tar
            // then the first intermediate remote directory is "Backups"
            // Once that is complete, we will request the folder metadata for drive/special/approot
            // and from that, parse the children array to get the remote ID of the "Backups" dir.
            // Then, we can create the next intermediate remote directory "ABCDEFG" etc.
            uploadData(accountId, accessToken, localPath, remotePath);
        } else if (syncDirection == Buteo::VALUE_FROM_REMOTE) {
            // download the required data.
            requestData(accountId, accessToken, localPath, remotePath, remoteFile);
        } else {
            SOCIALD_LOG_ERROR("invalid syncDirection specified to initialiseAppFolder request with OneDrive account:" << accountId << ":" << syncDirection);
            setStatus(SocialNetworkSyncAdaptor::Error);
        }
    }

    decrementSemaphore(accountId);
}


void OneDriveBackupSyncAdaptor::getRemoteFolderMetadata(int accountId, const QString &accessToken, const QString &localPath, const QString &remotePath, const QString &parentId, const QString &remoteDirName)
{
    // we request the parent folder metadata
    // e.g., let's say we have a final path like: drive/special/approot/Backups/ABCDEFG/backup.tar
    // this request will, when first called, be passed the remote id of the drive/special/approot bit
    // so we request the metadata for that (expanding children)
    QUrl url = QUrl(QStringLiteral("https://api.onedrive.com/v1.0/drive/items/%1").arg(parentId));
    QUrlQuery query(url);
    QList<QPair<QString, QString> > queryItems;
    queryItems.append(QPair<QString, QString>(QStringLiteral("expand"), QStringLiteral("children")));
    query.setQueryItems(queryItems);
    url.setQuery(query);

    QNetworkRequest req(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(req);

    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("localPath", localPath);
        reply->setProperty("remotePath", remotePath);
        reply->setProperty("parentId", parentId); // the id of the parent folder containing the remote folder we're interested in
        reply->setProperty("remoteDirName", remoteDirName); // the name of the remote folder we're interested in
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(getRemoteFolderMetadataFinishedHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply, 10 * 60 * 1000); // 10 minutes
    } else {
        SOCIALD_LOG_ERROR("unable to perform remote folder metadata request for OneDrive account with id" << accountId);
    }
}

void OneDriveBackupSyncAdaptor::getRemoteFolderMetadataFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString parentId = reply->property("parentId").toString();
    QString remoteDirName = reply->property("remoteDirName").toString();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(data, &ok);

    if (isError || !ok) {
        SOCIALD_LOG_ERROR("error occurred when performing remote folder metadata request with OneDrive account:" << accountId);
        debugDumpJsonResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
    } else {
        SOCIALD_LOG_DEBUG("remote folder metadata request succeeded with OneDrive account:" << accountId);
        SOCIALD_LOG_DEBUG("remote folder:" << parsed.value("name").toString() << "has remote ID:" << parsed.value("id").toString());
        debugDumpJsonResponse(data);
        if (!parsed.contains("children")) {
            SOCIALD_LOG_ERROR("folder metadata request result had no children!");
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
            return;
        }

        // parse the response, and find the child folder which we're interested in.
        // once we've found it, store the remote id associated with that child folder in our folder metadata list.
        // then, trigger creation of the next child subfolder, now we know the id of that subfolder's parent folder.
        bool foundChildFolder = false;
        QJsonArray children = parsed.value("children").toArray();
        Q_FOREACH (const QJsonValue &child, children) {
            const QJsonObject childObject = child.toObject();
            const QString childName = childObject.value("name").toString();
            const QString childId = childObject.value("id").toString();
            const bool isDir = childObject.keys().contains("folder");
            SOCIALD_LOG_DEBUG("Looking for:" << remoteDirName << ", checking child object:" << childName << "with id:" << childId << ", isDir?" << isDir);
            if (isDir && childName.compare(remoteDirName, Qt::CaseInsensitive) == 0) {
                SOCIALD_LOG_DEBUG("found folder:" << childName << "with remote id:" << childId << "for OneDrive account:" << accountId);
                foundChildFolder = true;
                bool updatedMetadata = false;
                for (int i = 0; i < m_remoteDirectories.size(); i++) {
                    if (m_remoteDirectories[i].parentId == parentId && m_remoteDirectories[i].dirName.compare(remoteDirName, Qt::CaseInsensitive) == 0) {
                        // found the directory whose metadata we should update
                        m_remoteDirectories[i].remoteId = childId;
                        m_remoteDirectories[i].dirName = childName;
                        if ((i+1) < (m_remoteDirectories.size())) {
                            // also, this directory will be the parent of the next directory
                            // so set the parentId of the next directory to be this directory's id.
                            m_remoteDirectories[i+1].parentId = childId;
                        }
                        updatedMetadata = true;
                        break;
                    }
                }
                if (!updatedMetadata) {
                    SOCIALD_LOG_ERROR("could not find remote dir in directory metadata:" << remoteDirName);
                    setStatus(SocialNetworkSyncAdaptor::Error);
                    decrementSemaphore(accountId);
                    return;
                }

                // we now know the remote id of this folder, so we can trigger creation of the next subfolder.
                uploadData(accountId, accessToken, localPath, remotePath);
                break;
            }
        }
        if (!foundChildFolder) {
            SOCIALD_LOG_ERROR("could not find remote dir in folder metadata response:" << remoteDirName);
            setStatus(SocialNetworkSyncAdaptor::Error);
        }
    }

    decrementSemaphore(accountId);
}

void OneDriveBackupSyncAdaptor::requestData(int accountId, const QString &accessToken,
                                            const QString &localPath, const QString &remotePath,
                                            const QString &remoteFile, const QString &redirectUrl)
{
    // step one: get the remote path and its children metadata.
    // step two: for each (non-folder) child in metadata, download it.

    QUrl url;
    if (accessToken.isEmpty()) {
        // content request to a temporary URL, since it doesn't require access token.
        url = QUrl(redirectUrl);
    } else {
        // directory or file info request.  We use the path and sign with access token.
        if (remoteFile.isEmpty()) {
            // directory request.  expand the children.
            url = QUrl(QStringLiteral("https://api.onedrive.com/v1.0/%1:/%2:/").arg(m_remoteAppDir).arg(remotePath));
            QUrlQuery query(url);
            QList<QPair<QString, QString> > queryItems;
            queryItems.append(QPair<QString, QString>(QStringLiteral("expand"), QStringLiteral("children")));
            query.setQueryItems(queryItems);
            url.setQuery(query);
            SOCIALD_LOG_DEBUG("performing directory request:" << url.toString());
        } else {
            // file request, download its metadata.  That will contain a content URL which we will redirect to.
            url = QUrl(QStringLiteral("https://api.onedrive.com/v1.0/%1:/%2/%3").arg(m_remoteAppDir).arg(remotePath).arg(remoteFile));
            SOCIALD_LOG_DEBUG("performing file request:" << url.toString());
        }
    }

    QNetworkRequest req(url);
    req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                     QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());

    QNetworkReply *reply = m_networkAccessManager->get(req);

    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("localPath", localPath);
        reply->setProperty("remotePath", remotePath);
        reply->setProperty("remoteFile", remoteFile);
        reply->setProperty("redirectUrl", redirectUrl);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        if (remoteFile.isEmpty()) {
            connect(reply, SIGNAL(finished()), this, SLOT(remotePathFinishedHandler()));
        } else {
            connect(reply, SIGNAL(downloadProgress(qint64,qint64)), this, SLOT(downloadProgressHandler(qint64,qint64)));
            connect(reply, SIGNAL(finished()), this, SLOT(remoteFileFinishedHandler()));
        }

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply, 10 * 60 * 1000); // 10 minutes
    } else {
        SOCIALD_LOG_ERROR("unable to create download request:" << remotePath << remoteFile << redirectUrl <<
                          "for OneDrive account with id" << accountId);
    }
}

void OneDriveBackupSyncAdaptor::remotePathFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    bool isError = reply->property("isError").toBool();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    if (isError) {
        SOCIALD_LOG_ERROR("error occurred when performing Backup remote path request for OneDrive account" << accountId << ":");
        debugDumpJsonResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(data, &ok);
    if (!ok || !parsed.contains("children")) {
        SOCIALD_LOG_ERROR("no backup data exists in reply from OneDrive with account" << accountId << ", got:");
        debugDumpJsonResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    QJsonArray children = parsed.value("children").toArray();
    Q_FOREACH (const QJsonValue &child, children) {
        const QString childName = child.toObject().value("name").toString();
        if (child.toObject().keys().contains("folder")) {
            SOCIALD_LOG_DEBUG("ignoring folder:" << childName << "under remote backup path:" << remotePath << "for OneDrive account:" << accountId);
        } else {
            SOCIALD_LOG_DEBUG("found remote backup object:" << childName << "for OneDrive account:" << accountId);
            requestData(accountId, accessToken, localPath, remotePath, childName);
        }
    }

    decrementSemaphore(accountId);
}

void OneDriveBackupSyncAdaptor::remoteFileFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString remoteFile = reply->property("remoteFile").toString();
    QString redirectUrl = reply->property("redirectUrl").toString();
    bool isError = reply->property("isError").toBool();
    QString remoteFileName = QStringLiteral("%1/%2").arg(remotePath).arg(remoteFile);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    if (isError) {
        SOCIALD_LOG_ERROR("error occurred when performing Backup remote file request for OneDrive account" << accountId << ", got:");
        debugDumpJsonResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    // if it was a file metadata request, then parse the content url location and redirect to that.
    // otherwise it was a file content request and we should save the data.
    if (redirectUrl.isEmpty()) {
        // we expect to be redirected from the file path to a temporary url to GET content/data.
        // note: no access token is required to access the content redirect url.
        bool ok = false;
        QJsonObject parsed = parseJsonObjectReplyData(data, &ok);
        if (!ok || !parsed.contains("@content.downloadUrl")) {
            SOCIALD_LOG_ERROR("no content redirect url exists in file metadata for file:" << remoteFile);
            debugDumpJsonResponse(data);
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
            return;
        }
        redirectUrl = parsed.value("@content.downloadUrl").toString();
        SOCIALD_LOG_DEBUG("redirected from:" << remoteFileName << "to:" << redirectUrl);
        requestData(accountId, QString(), localPath, remotePath, remoteFile, redirectUrl);
    } else {
        if (data.isEmpty()) {
            SOCIALD_LOG_INFO("remote file:" << remoteFileName << "is empty; ignoring");
        } else {
            const QString filename = QStringLiteral("%1/%2").arg(localPath).arg(remoteFile);
            QFile file(filename);
            file.open(QIODevice::WriteOnly); // TODO: error checking
            file.write(data);
            file.close();
            SOCIALD_LOG_DEBUG("successfully wrote" << data.size() << "bytes to:" << filename << "from:" << remoteFileName);
        }
    }

    decrementSemaphore(accountId);
}

void OneDriveBackupSyncAdaptor::uploadData(int accountId, const QString &accessToken, const QString &localPath, const QString &remotePath, const QString &localFile)
{
    // step one: ensure the remote path exists (and if not, create it)
    // step two: upload every single file from the local path to the remote path.

    QNetworkReply *reply = 0;
    QString intermediatePath;
    if (localFile.isEmpty()) {
        // attempt to create the remote path directory.
        QString remoteDir;
        QString remoteParentId;
        QString remoteParentPath;
        for (int i = 0; i < m_remoteDirectories.size(); ++i) {
            if (m_remoteDirectories[i].created == false) {
                m_remoteDirectories[i].created = true; // we're creating this intermediate dir this time.
                remoteDir = m_remoteDirectories[i].dirName;
                remoteParentId = m_remoteDirectories[i].parentId;
                remoteParentPath = m_remoteDirectories[i].parentPath;
                break;
            }
        }
        if (remoteDir.isEmpty()) {
            SOCIALD_LOG_ERROR("No remote directory to create, but no file specified for upload - aborting");
            return;
        }
        if (remoteParentId.isEmpty()) {
            SOCIALD_LOG_ERROR("No remote parent id known for directory:" << remoteDir << "- aborting");
            return;
        }

        QString createFolderJson = QStringLiteral(
            "{"
                "\"name\": \"%1\","
                "\"folder\": { }"
            "}").arg(remoteDir);
        QByteArray data = createFolderJson.toUtf8();

        QUrl url = QUrl(QStringLiteral("https://api.onedrive.com/v1.0/drive/items/%1/children").arg(remoteParentId));
        intermediatePath = QStringLiteral("%1/%2").arg(remoteParentPath).arg(remoteDir);
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentLengthHeader, data.size());
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QVariant::fromValue<QString>(QString::fromLatin1("application/json")));
        request.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                             QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());
        SOCIALD_LOG_DEBUG("Attempting to create the remote directory:" << intermediatePath << "via request:" << url.toString());
        SOCIALD_LOG_DEBUG("with data:" << createFolderJson);

        reply = m_networkAccessManager->post(request, data);
    } else {
        // attempt to create a remote file.
        QUrl url = QUrl(QStringLiteral("https://api.onedrive.com/v1.0/%1:/%2/%3:/content").arg(m_remoteAppDir).arg(remotePath).arg(localFile));
        QString localFileName = QStringLiteral("%1/%2").arg(localPath).arg(localFile);
        QFile f(localFileName, this);
         if(!f.open(QIODevice::ReadOnly)){
             SOCIALD_LOG_ERROR("unable to open local file:" << localFileName << "for upload to OneDrive Backup with account:" << accountId);
         } else {
             QByteArray data(f.readAll());
             f.close();
             QNetworkRequest req(url);
             req.setHeader(QNetworkRequest::ContentLengthHeader, data.size());
             req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
             req.setRawHeader(QString(QLatin1String("Authorization")).toUtf8(),
                              QString(QLatin1String("Bearer ")).toUtf8() + accessToken.toUtf8());
             SOCIALD_LOG_DEBUG("Attempting to create the remote file:" << QStringLiteral("%1/%2").arg(remotePath).arg(localFile) << "via request:" << url.toString());
             reply = m_networkAccessManager->put(req, data);
        }
    }

    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        reply->setProperty("localPath", localPath);
        reply->setProperty("remotePath", remotePath);
        reply->setProperty("intermediatePath", intermediatePath);
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
                          "for OneDrive account with id" << accountId);
    }
}

void OneDriveBackupSyncAdaptor::createRemotePathFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString accessToken = reply->property("accessToken").toString();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString intermediatePath = reply->property("intermediatePath").toString();
    bool isError = reply->property("isError").toBool();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    if (isError) {
        if (httpCode == 409) {
            // we actually expect a conflict error if the folder already existed, which is fine.
            SOCIALD_LOG_DEBUG("remote path creation had conflict: already exists:" << intermediatePath << ".  Continuing.");
        } else {
            // this must be a real error.
            SOCIALD_LOG_ERROR("remote path creation failed:" << httpCode);
            debugDumpJsonResponse(data);
            setStatus(SocialNetworkSyncAdaptor::Error);
            decrementSemaphore(accountId);
            return;
        }
    }

    // Check to see if we need to create any more intermediate directories
    QString createdDirectoryParentFolderId;
    QString createdDirectoryName;
    for (int i = 0; i < m_remoteDirectories.size(); ++i) {
        if (m_remoteDirectories[i].created) {
            // the last one of these will be the one for which this response was received.
            createdDirectoryParentFolderId = m_remoteDirectories[i].parentId;
            createdDirectoryName = m_remoteDirectories[i].dirName;
        } else {
            SOCIALD_LOG_DEBUG("successfully created folder:" << createdDirectoryName << ", now performing parent request to get its remote id");
            SOCIALD_LOG_DEBUG("need to create another remote directory:" << m_remoteDirectories[i].dirName << "with parent:" << m_remoteDirectories[i].parentPath);
            // first, get the metadata for the most recently created path's parent, to get its remote ID.
            // after that's done, the next intermediate directory can be created.
            // NOTE: we do this (rather than attempting to parse the folder id from the response in this function)
            // because in the case where the remote path creation failed due to conflict, the response doesn't
            // contain the remote id.  So, better to have uniform code to handle all cases.
            getRemoteFolderMetadata(accountId, accessToken, localPath, remotePath, createdDirectoryParentFolderId, createdDirectoryName);
            decrementSemaphore(accountId);
            return;
        }
    }

    // upload all files from the local path to the remote server.
    SOCIALD_LOG_DEBUG("remote path now exists, attempting to upload local files");
    QDir dir(localPath);
    QStringList localFiles = dir.entryList(QDir::Files);
    Q_FOREACH (const QString &localFile, localFiles) {
        SOCIALD_LOG_DEBUG("uploading file:" << localFile << "from" << localPath << "to:" << remotePath);
        uploadData(accountId, accessToken, localPath, remotePath, localFile);
    }
    decrementSemaphore(accountId);
}

void OneDriveBackupSyncAdaptor::createRemoteFileFinishedHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    QByteArray data = reply->readAll();
    int accountId = reply->property("accountId").toInt();
    QString localPath = reply->property("localPath").toString();
    QString remotePath = reply->property("remotePath").toString();
    QString localFile = reply->property("localFile").toString();
    bool isError = reply->property("isError").toBool();
    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);
    if (isError) {
        SOCIALD_LOG_ERROR("failed to backup file:" << localPath << localFile << "to:" << remotePath <<
                          "for OneDrive account:" << accountId << ", code:" << httpCode);
        debugDumpJsonResponse(data);
        setStatus(SocialNetworkSyncAdaptor::Error);
        decrementSemaphore(accountId);
        return;
    }

    SOCIALD_LOG_DEBUG("successfully uploaded backup of file:" << localPath << localFile << "to:" << remotePath <<
                      "for OneDrive account:" << accountId);
    decrementSemaphore(accountId);
}

void OneDriveBackupSyncAdaptor::downloadProgressHandler(qint64 bytesReceived, qint64 bytesTotal)
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

void OneDriveBackupSyncAdaptor::uploadProgressHandler(qint64 bytesSent, qint64 bytesTotal)
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

void OneDriveBackupSyncAdaptor::finalize(int accountId)
{
    SOCIALD_LOG_DEBUG("finished OneDrive backup sync for account" << accountId);
}

void OneDriveBackupSyncAdaptor::purgeAccount(int)
{
    // TODO: delete the contents of the localPath directory?  probably not, could be shared between onedrive+dropbox
}

void OneDriveBackupSyncAdaptor::finalCleanup()
{
    // nothing to do?
}

